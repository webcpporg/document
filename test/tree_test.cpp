// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The tree of a document: its key order under each object policy, keys that look like array
// indices, elements renumbered, deep copies and literals, the header it keeps, and operations that
// either complete or change nothing, in buffers of every size and in owned images that grow.

#include <webcpp/document/detail/tree.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;
namespace detail = webcpp::document::detail;
using traits = wd::default_traits;
using tree = detail::tree<traits>;
using entry = detail::literal_entry<traits>;

namespace {

/** Default traits whose objects keep their keys in insertion order. */
struct insertion_traits : wd::default_traits {
    using object = wd::insertion_order;
};

/** Default traits whose objects sort their keys. */
struct sorted_traits : wd::default_traits {
    using object = wd::sorted;
};

/** JavaScript's traits, UTF-16 strings and keys, whose objects sort their keys. */
struct sorted16_traits : wd::js_traits {
    using object = wd::sorted;
};

constexpr std::uint64_t mebibyte = std::uint64_t{1} << 20U;

template <class Traits = traits>
detail::item<Traits> integer(std::int64_t v) {
    detail::item<Traits> i;
    i.kind = wd::kind::number;
    i.number = detail::repr::integer;
    i.store(v);
    return i;
}

template <class Traits = traits>
detail::item<Traits> boolean(bool v) {
    detail::item<Traits> i;
    i.kind = wd::kind::boolean;
    i.store(v);
    return i;
}

template <class Traits = traits>
detail::item<Traits> null() {
    return {};
}

// A string in UTF-8, for traits that store UTF-8; the text must outlive the item.
template <class Traits = traits>
detail::item<Traits> string(std::string_view utf8) {
    static_assert(std::same_as<typename Traits::string, wd::utf8>);
    detail::item<Traits> i;
    i.kind = wd::kind::string;
    i.text = std::as_bytes(std::span(utf8.data(), utf8.size()));
    return i;
}

// A string in UTF-16, for traits that store UTF-16; the text must outlive the item.
template <class Traits>
detail::item<Traits> string16(std::u16string_view utf16) {
    static_assert(std::same_as<typename Traits::string, wd::utf16>);
    detail::item<Traits> i;
    i.kind = wd::kind::string;
    i.text = std::as_bytes(std::span(utf16.data(), utf16.size()));
    return i;
}

template <class Traits = traits>
detail::item<Traits> object() {
    detail::item<Traits> i;
    i.kind = wd::kind::object;
    return i;
}

template <class Traits = traits>
detail::item<Traits> array() {
    detail::item<Traits> i;
    i.kind = wd::kind::array;
    return i;
}

template <class Traits = traits>
detail::item<Traits> undefined() {
    detail::item<Traits> i;
    i.kind = wd::kind::undefined;
    return i;
}

// A literal object or array of the entries, which must outlive the item.
detail::item<traits> literal(wd::kind k, std::span<const detail::literal_entry<traits>> entries) {
    detail::item<traits> i;
    i.kind = k;
    i.entries = entries.data();
    i.entry_count = entries.size();
    return i;
}

template <class Traits = traits>
detail::item<Traits> copy_of(const detail::tree<Traits>& from, std::uint64_t id) {
    detail::item<Traits> i;
    i.from = &from;
    i.from_id = id;
    return i;
}

// A key or a string as UTF-8, whatever the traits' encoding.
template <class Traits>
std::string text_of(const detail::byte_string& s) {
    if constexpr (std::same_as<typename Traits::string, wd::utf16>) {
        const std::u16string_view units = s.units();
        std::string out(static_cast<std::size_t>(detail::utf8_size(units)), '\0');
        detail::to_utf8(units, out.data());
        return out;
    } else {
        return std::string(s.chars());
    }
}

// Writes a node and its subtree in a compact JSON-like text, children in the policy's order; the
// recursion is as deep as the small trees these cases build.
template <class Traits>
// NOLINTNEXTLINE(misc-no-recursion)
void write_node(const detail::tree<Traits>& t, const detail::node<Traits>& n, std::string& out) {
    switch (n.kind) {
        case wd::kind::undefined: out += "undefined"; return;
        case wd::kind::null: out += "null"; return;
        case wd::kind::boolean: out += n.template load<bool>() ? "true" : "false"; return;
        case wd::kind::number:
            if (n.number == detail::repr::integer) {
                out += std::to_string(n.template load<std::int64_t>());
            } else {
                using number = Traits::number;
                out += wd::number_codec<number>::write(n.template load<number>()).view();
            }
            return;
        case wd::kind::string: out += '"' + text_of<Traits>(n.text) + '"'; return;
        case wd::kind::date: {
            const auto since_epoch = n.template load<wd::date_ms>().time_since_epoch();
            out += "date(" + std::to_string(since_epoch.count()) + ")";
            return;
        }
        case wd::kind::bytes: out += "bytes(" + std::to_string(n.text.size) + ")"; return;
        case wd::kind::array:
        case wd::kind::object: {
            const bool is_object = n.kind == wd::kind::object;
            out += is_object ? '{' : '[';
            bool first = true;
            for (const auto* c = t.first_child(n.id); c != nullptr; c = t.next_sibling(*c)) {
                if (!first) {
                    out += ',';
                }
                first = false;
                if (is_object) {
                    out += '"' + text_of<Traits>(c->key) + "\":";
                }
                write_node(t, *c, out);
            }
            out += is_object ? '}' : ']';
            return;
        }
    }
}

// The whole tree, from the root, in the policy's order.
template <class Traits>
std::string dump(const detail::tree<Traits>& t) {
    const detail::node<Traits>* root = t.find(t.root());
    webcpp::test::require(BOOST_TEST(root != nullptr));
    std::string out;
    write_node(t, *root, out);
    return out;
}

// The keys of an object, in UTF-8, in the policy's order.
template <class Traits>
std::vector<std::string> keys(const detail::tree<Traits>& t, std::uint64_t object) {
    std::vector<std::string> out;
    t.for_each_child(object,
                     [&](const detail::node<Traits>& c) { out.push_back(text_of<Traits>(c.key)); });
    return out;
}

// The id of a member, which must be there.
template <class Traits>
std::uint64_t id_of(const detail::tree<Traits>& t, std::uint64_t object, std::string_view key) {
    const detail::node<Traits>* n = t.member(object, key);
    webcpp::test::require(BOOST_TEST(n != nullptr));
    return n->id;
}

wd::image_header header_of(const detail::region& r) {
    wd::image_header h{};
    std::memcpy(&h, r.data(), sizeof h);
    return h;
}

std::span<const std::byte> image_of(const detail::region& r) {
    return {r.data(), static_cast<std::size_t>(r.size())};
}

bool contains(std::span<const std::byte> image, std::string_view needle) {
    return !std::ranges::search(image, std::as_bytes(std::span(needle.data(), needle.size())))
                .empty();
}

// A buffer of the process heap with a 16-aligned span of `size` bytes inside it.
class aligned_buffer {
public:
    explicit aligned_buffer(std::size_t size) : raw_(size + 16) {
        void* p = raw_.data();
        std::size_t space = raw_.size();
        std::align(16, size, p, space);
        bytes_ = {static_cast<std::byte*>(p), size};
    }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::vector<std::byte> raw_;
    std::span<std::byte> bytes_;
};

template <class Traits = traits>
struct fixture {
    detail::region r = *detail::region::owned(4096, 16 * mebibyte);
    detail::tree<Traits> t = *detail::tree<Traits>::format(r);
};

void a_new_tree_holds_null() {
    fixture f;
    BOOST_TEST_EQ(f.t.root(), 1U);
    BOOST_TEST(f.t.find(1)->kind == wd::kind::null);
    BOOST_TEST_EQ(f.t.find(1)->parent, 0U);
    BOOST_TEST_EQ(f.t.node_count(), 1U);
    BOOST_TEST(&f.t.storage() == &f.r);
    const wd::image_header h = header_of(f.r);
    BOOST_TEST(h.magic == wd::image_magic);
    BOOST_TEST_EQ(h.version, wd::format_version);
    BOOST_TEST_EQ(h.byte_order, wd::byte_order_mark);
    BOOST_TEST_EQ(h.reserved_4, 0U);
    BOOST_TEST_EQ(h.layout, detail::layout_tag<traits>());
    BOOST_TEST_EQ(h.size, f.r.size());
    BOOST_TEST_EQ(h.next_id, 2U);
    BOOST_TEST_EQ(h.container, detail::container_offset);
    BOOST_TEST(wd::check_image(image_of(f.r), detail::layout_tag<traits>()).has_value());
}

// The layout tag tells traits and their node types apart.
void layout_tags_differ() {
    BOOST_TEST_NE(detail::layout_tag<traits>(), detail::layout_tag<wd::js_traits>());
    BOOST_TEST_NE(detail::layout_tag<traits>(), detail::layout_tag<sorted_traits>());
    BOOST_TEST_NE(detail::layout_tag<traits>(), detail::layout_tag<insertion_traits>());
    static_assert(detail::layout_tag<traits>() != detail::layout_tag<wd::js_traits>());
}

// JavaScript's OrdinaryOwnPropertyKeys, measured in Node 26.
void javascript_key_order() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    for (const std::string_view k : {"b", "2", "a", "1"}) {
        BOOST_TEST(f.t.set_member(1, k, integer(0)).has_value());
    }
    BOOST_TEST((keys(f.t, 1) == std::vector<std::string>{"1", "2", "b", "a"}));
    BOOST_TEST(f.t.set_member(1, "b", integer(9)).has_value());  // assigned: keeps its place
    BOOST_TEST((keys(f.t, 1) == std::vector<std::string>{"1", "2", "b", "a"}));
    BOOST_TEST(*f.t.erase_member(1, "b"));
    BOOST_TEST(f.t.set_member(1, "b", integer(1)).has_value());  // re-created: last
    BOOST_TEST((keys(f.t, 1) == std::vector<std::string>{"1", "2", "a", "b"}));
    BOOST_TEST(*f.t.erase_member(1, "1"));
    BOOST_TEST(f.t.set_member(1, "1", integer(1)).has_value());  // an index: back among the indices
    BOOST_TEST((keys(f.t, 1) == std::vector<std::string>{"1", "2", "a", "b"}));
    BOOST_TEST_EQ(f.t.count(1), 4U);
    BOOST_TEST(!*f.t.erase_member(1, "missing"));
    BOOST_TEST_EQ(f.t.count(1), 4U);
}

void index_lookalikes() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    for (const std::string_view k : std::initializer_list<std::string_view>{
             "x",
             "01",
             "4294967295",
             "-0",
             "1e2",
             "",
             "4294967294",
             "0",
             std::string_view("\0k", 2),
         }) {
        BOOST_TEST(f.t.set_member(1, k, integer(0)).has_value());
    }
    BOOST_TEST((keys(f.t, 1) == std::vector<std::string>{"0", "4294967294", "x", "01", "4294967295",
                                                         "-0", "1e2", "", std::string("\0k", 2)}));
    BOOST_TEST(f.t.member(1, std::string_view("\0k", 2)) != nullptr);
    BOOST_TEST(f.t.member(1, "k") == nullptr);
    BOOST_TEST(f.t.member(1, "") != nullptr);
    // An index key erased and created again goes back among the indices, not last.
    BOOST_TEST(*f.t.erase_member(1, "4294967294"));
    BOOST_TEST(f.t.member(1, "4294967294") == nullptr);
    BOOST_TEST(f.t.set_member(1, "4294967294", integer(1)).has_value());
    BOOST_TEST((keys(f.t, 1) == std::vector<std::string>{"0", "4294967294", "x", "01", "4294967295",
                                                         "-0", "1e2", "", std::string("\0k", 2)}));
    // A look-alike erased and created again goes last.
    BOOST_TEST(*f.t.erase_member(1, "01"));
    BOOST_TEST(f.t.set_member(1, "01", integer(1)).has_value());
    BOOST_TEST((keys(f.t, 1) == std::vector<std::string>{"0", "4294967294", "x", "4294967295", "-0",
                                                         "1e2", "", std::string("\0k", 2), "01"}));
}

template <class Traits>
std::vector<std::string> keys_of_b2a1() {
    fixture<Traits> f;
    BOOST_TEST(f.t.assign(1, object<Traits>()).has_value());
    for (const std::string_view k : {"b", "2", "a", "1"}) {
        BOOST_TEST(f.t.set_member(1, k, integer<Traits>(0)).has_value());
    }
    // Assigning a key keeps its place under every policy.
    BOOST_TEST(f.t.set_member(1, "2", integer<Traits>(7)).has_value());
    return keys(f.t, 1);
}

void insertion_and_sorted_orders() {
    BOOST_TEST((keys_of_b2a1<insertion_traits>() == std::vector<std::string>{"b", "2", "a", "1"}));
    BOOST_TEST((keys_of_b2a1<sorted_traits>() == std::vector<std::string>{"1", "2", "a", "b"}));
    BOOST_TEST((keys_of_b2a1<traits>() == std::vector<std::string>{"1", "2", "b", "a"}));
}

// U+FF61 comes before U+1F600 in UTF-8's bytes and after it in UTF-16's code units, whose order
// sorted keys follow with either encoding.
void sorted_by_code_units() {
    const std::string_view halfwidth = "\xef\xbd\xa1";
    const std::string_view emoji = "\xf0\x9f\x98\x80";
    {
        fixture<sorted_traits> f;
        BOOST_TEST(f.t.assign(1, object<sorted_traits>()).has_value());
        BOOST_TEST(f.t.set_member(1, emoji, integer<sorted_traits>(1)).has_value());
        BOOST_TEST(f.t.set_member(1, halfwidth, integer<sorted_traits>(2)).has_value());
        BOOST_TEST(
            (keys(f.t, 1) == std::vector<std::string>{std::string(emoji), std::string(halfwidth)}));
    }
    {
        fixture<sorted16_traits> f;
        BOOST_TEST(f.t.assign(1, object<sorted16_traits>()).has_value());
        BOOST_TEST(f.t.set_member(1, halfwidth, integer<sorted16_traits>(2)).has_value());
        BOOST_TEST(f.t.set_member(1, emoji, integer<sorted16_traits>(1)).has_value());
        BOOST_TEST(
            (keys(f.t, 1) == std::vector<std::string>{std::string(emoji), std::string(halfwidth)}));
    }
}

// With UTF-16 keys, a key is given in UTF-8 and stored in UTF-16; one that is no UTF-8 is refused
// and finds nothing.
void utf16_keys() {
    fixture<wd::js_traits> f;
    BOOST_TEST(f.t.assign(1, object<wd::js_traits>()).has_value());
    auto id = f.t.set_member(1, "caf\xc3\xa9", string16<wd::js_traits>(u"été"));
    if (!BOOST_TEST(id.has_value())) {
        return;
    }
    const auto* n = f.t.member(1, "caf\xc3\xa9");
    if (!BOOST_TEST(n != nullptr)) {
        return;
    }
    BOOST_TEST_EQ(n->id, *id);
    BOOST_TEST(n->key.units() == u"café");
    BOOST_TEST(n->text.units() == u"été");
    BOOST_TEST_EQ(dump(f.t), "{\"caf\xc3\xa9\":\"\xc3\xa9t\xc3\xa9\"}");
    BOOST_TEST(f.t.member(1, "caf\xc3") == nullptr);
    BOOST_TEST(f.t.set_member(1, "caf\xc3", integer<wd::js_traits>(1)).error() == wd::errc::type);
    BOOST_TEST(f.t.set_member(1, "\xed\xa0\x80", integer<wd::js_traits>(1)).error() ==
               wd::errc::type);
    BOOST_TEST_EQ(f.t.count(1), 1U);
}

// Scalars are stored by their codecs, bytes as they are; an item of no kind, or a number held as
// nothing, is refused.
void scalars_are_stored_as_given() {
    fixture f;
    detail::item<traits> date;
    date.kind = wd::kind::date;
    date.store(wd::date_ms(std::chrono::milliseconds(1234)));
    static constexpr std::array<std::byte, 3> raw{std::byte{1}, std::byte{0}, std::byte{255}};
    detail::item<traits> bytes;
    bytes.kind = wd::kind::bytes;
    bytes.text = raw;
    detail::item<traits> decimal;
    decimal.kind = wd::kind::number;
    decimal.number = detail::repr::number;
    decimal.store(boost::decimal::decimal128_t{15, -1});
    const std::vector<entry> list{
        {.key = "", .value = date},    {.key = "", .value = bytes},
        {.key = "", .value = decimal}, {.key = "", .value = boolean(false)},
        {.key = "", .value = null()},
    };
    BOOST_TEST(f.t.assign(1, literal(wd::kind::array, list)).has_value());
    BOOST_TEST_EQ(dump(f.t), "[date(1234),bytes(3),1.5,false,null]");
    BOOST_TEST(std::ranges::equal(f.t.element(1, 1)->text.view(), raw));
    detail::item<traits> no_kind;
    no_kind.kind = static_cast<wd::kind>(42);
    BOOST_TEST(f.t.set_element(1, 0, no_kind).error() == wd::errc::type);
    detail::item<traits> held_as_nothing;
    held_as_nothing.kind = wd::kind::number;
    BOOST_TEST(f.t.set_element(1, 0, held_as_nothing).error() == wd::errc::type);
    BOOST_TEST_EQ(dump(f.t), "[date(1234),bytes(3),1.5,false,null]");
}

void elements_are_renumbered() {
    fixture f;
    BOOST_TEST(f.t.assign(1, array()).has_value());
    const std::vector<detail::item<traits>> four{integer(0), integer(1), integer(2), integer(3)};
    BOOST_TEST(f.t.insert_elements(1, 0, four).has_value());
    const std::vector<detail::item<traits>> two{integer(8), integer(9)};
    BOOST_TEST(f.t.insert_elements(1, 1, two).has_value());
    BOOST_TEST_EQ(dump(f.t), "[0,8,9,1,2,3]");
    BOOST_TEST(f.t.erase_elements(1, 2, 3).has_value());
    BOOST_TEST_EQ(dump(f.t), "[0,8,3]");
    BOOST_TEST(f.t.reverse(1).has_value());
    BOOST_TEST_EQ(dump(f.t), "[3,8,0]");
    BOOST_TEST(f.t.set_length(1, 5).has_value());
    BOOST_TEST_EQ(dump(f.t), "[3,8,0,null,null]");
    BOOST_TEST(f.t.set_length(1, 1).has_value());
    BOOST_TEST_EQ(dump(f.t), "[3]");
    BOOST_TEST_EQ(f.t.count(1), 1U);
    BOOST_TEST(f.t.insert_elements(1, 1, two).has_value());
    BOOST_TEST_EQ(dump(f.t), "[3,8,9]");
    BOOST_TEST(f.t.reverse(1).has_value());
    BOOST_TEST_EQ(dump(f.t), "[9,8,3]");
    BOOST_TEST_EQ(f.t.element(1, 2)->template load<std::int64_t>(), 3);
    BOOST_TEST(f.t.element(1, 3) == nullptr);
    BOOST_TEST(f.t.erase_elements(1, 0, 3).has_value());
    BOOST_TEST_EQ(dump(f.t), "[]");
    BOOST_TEST_EQ(f.t.node_count(), 1U);
}

// What a container is not, and an index past an array's end, are refused and change nothing.
void wrong_places_are_refused() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    const auto arr = f.t.set_member(1, "a", array());
    if (!BOOST_TEST(arr.has_value())) {
        return;
    }
    const auto num = f.t.set_member(1, "n", integer(1));
    if (!BOOST_TEST(num.has_value())) {
        return;
    }
    const std::vector<detail::item<traits>> one{integer(1)};
    BOOST_TEST(f.t.insert_elements(*arr, 0, one).has_value());
    const std::string before = dump(f.t);
    BOOST_TEST(f.t.set_member(*arr, "x", integer(1)).error() == wd::errc::type);
    BOOST_TEST(f.t.set_member(*num, "x", integer(1)).error() == wd::errc::type);
    BOOST_TEST(f.t.set_member(99, "x", integer(1)).error() == wd::errc::type);
    BOOST_TEST(f.t.erase_member(*arr, "x").error() == wd::errc::type);
    BOOST_TEST(f.t.insert_elements(1, 0, one).error() == wd::errc::type);
    BOOST_TEST(f.t.insert_elements(*arr, 2, one).error() == wd::errc::range);
    BOOST_TEST(f.t.erase_elements(*arr, 0, 2).error() == wd::errc::range);
    BOOST_TEST(f.t.splice(*arr, 1, 1, one).error() == wd::errc::range);
    BOOST_TEST(f.t.set_element(1, 0, integer(1)).error() == wd::errc::type);
    BOOST_TEST(f.t.reverse(*num).error() == wd::errc::type);
    BOOST_TEST(f.t.set_length(1, 3).error() == wd::errc::type);
    BOOST_TEST(f.t.assign(99, integer(1)).error() == wd::errc::type);
    BOOST_TEST(f.t.set_element(*arr, std::uint64_t{1} << 60U, integer(1)).error() ==
               wd::errc::capacity);
    BOOST_TEST(f.t.set_length(*arr, std::uint64_t{1} << 60U).error() == wd::errc::capacity);
    // A member is no element and an element no member, under any key that spells an index.
    BOOST_TEST(f.t.set_member(1, "0", integer(1)).has_value());
    BOOST_TEST(f.t.element(1, 0) == nullptr);
    BOOST_TEST(f.t.member(*arr, "") == nullptr);
    BOOST_TEST(f.t.member(*arr, "0") == nullptr);
    BOOST_TEST(*f.t.erase_member(1, "0"));
    BOOST_TEST_EQ(dump(f.t), before);
    BOOST_TEST_EQ(f.t.count(*num), 0U);
    BOOST_TEST_EQ(f.t.count(99), 0U);
}

void replacing_a_container_frees_its_subtree() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    auto inner = f.t.set_member(1, "a", object());
    if (!BOOST_TEST(inner.has_value())) {
        return;
    }
    BOOST_TEST(f.t.set_member(*inner, "b", string("a long string that is not short")).has_value());
    BOOST_TEST_EQ(f.t.node_count(), 3U);
    auto& h = detail::heap_of(f.r);
    BOOST_TEST(f.t.assign(1, integer(5)).has_value());
    BOOST_TEST_EQ(f.t.node_count(), 1U);
    BOOST_TEST_EQ(dump(f.t), "5");
    BOOST_TEST(h.check_sanity());
}

// Runs `op(i)` for `i` from 0 until it fails, at most `limit` times: the failure must be capacity
// and leave the tree, the header's next id and the heap as they were. Whether it failed.
template <class Operation>
bool until_no_room(tree& t, const detail::region& r, int limit, Operation&& op) {
    for (int i = 0; i < limit; ++i) {
        const std::string before = dump(t);
        const std::uint64_t nodes = t.node_count();
        const std::uint64_t next_id = header_of(r).next_id;
        const wd::result<void> done = op(i);
        if (!done) {
            BOOST_TEST(done.error() == wd::errc::capacity);
            BOOST_TEST_EQ(dump(t), before);
            BOOST_TEST_EQ(t.node_count(), nodes);
            BOOST_TEST_EQ(header_of(r).next_id, next_id);
            BOOST_TEST(detail::heap_of(r).check_sanity());
            return true;
        }
    }
    return false;
}

// In an external buffer too small for what is asked, every operation fails with capacity and the
// content stays as it was; a buffer of every size from none to the one that holds every insertion.
void nothing_changes_when_there_is_no_room() {
    std::size_t formatted = 0;
    std::size_t filled = 0;
    for (std::size_t size = 0; size <= 16384; size += 16) {
        const aligned_buffer buffer(size);
        auto r = detail::region::external_empty(buffer.bytes());
        if (!r) {
            BOOST_TEST(r.error() == wd::errc::capacity);
            continue;
        }
        auto t = tree::format(*r);
        if (!t) {
            BOOST_TEST(t.error() == wd::errc::capacity);
            continue;
        }
        ++formatted;
        if (!t->assign(1, array())) {
            BOOST_TEST_EQ(dump(*t), "null");
            continue;
        }
        const bool full = until_no_room(*t, *r, 200, [&t](int i) {
            const std::vector<detail::item<traits>> items{
                string("a string long enough to take a block"),
                object(),
                integer(i),
            };
            return t->insert_elements(1, t->count(1) / 2, items);
        });
        filled += full ? 1 : 0;
    }
    BOOST_TEST_GT(formatted, 0U);
    BOOST_TEST_GT(filled, 0U);
}

// The base every operation below starts from: {"arr":[1,2,3],"a":{"s":"..."}}.
bool build_base(tree& t) {
    static constexpr std::string_view text = "a string long enough to take a block";
    const std::vector<entry> numbers{
        {.key = "", .value = integer(1)},
        {.key = "", .value = integer(2)},
        {.key = "", .value = integer(3)},
    };
    const std::vector<entry> inner{
        {.key = "s", .value = string(text)},
    };
    const std::vector<entry> base{
        {.key = "arr", .value = literal(wd::kind::array, std::span(numbers))},
        {.key = "a", .value = literal(wd::kind::object, std::span(inner))},
    };
    return t.assign(1, literal(wd::kind::object, std::span(base))).has_value();
}

// One operation of each kind, numbered by `i`, on a tree built by build_base.
wd::result<void> operation_of_kind(std::size_t kind, tree& t, int i) {
    static constexpr std::string_view text = "another string long enough to take a block";
    const std::uint64_t arr = id_of(t, 1, "arr");
    const std::string key = "k" + std::to_string(i);
    switch (kind) {
        case 0: {
            const std::vector<entry> list{
                {.key = "", .value = integer(i)},
                {.key = "", .value = string(text)},
            };
            const std::vector<entry> entries{
                {.key = "x", .value = literal(wd::kind::array, std::span(list))},
                {.key = "y", .value = boolean(true)},
            };
            const auto done = t.set_member(1, key, literal(wd::kind::object, std::span(entries)));
            return done ? wd::result<void>() : wd::result<void>(done.error());
        }
        case 1: return t.assign(id_of(t, 1, "a"), copy_of(t, 1));
        case 2: {
            const std::vector<entry> members{
                {.key = key, .value = string(text)},
                {.key = "0", .value = integer(i)},
                {.key = "a", .value = object()},
                {.key = key, .value = copy_of(t, arr)},
            };
            return t.set_members(1, members);
        }
        case 3: {
            const std::vector<detail::item<traits>> items{string(text), object(), array()};
            return t.splice(arr, 1, 1, items);
        }
        case 4: return t.set_element(arr, t.count(arr) + 5, string(text));
        case 5: return t.set_length(arr, t.count(arr) + 7);
        default: {
            const std::vector<detail::item<traits>> items{integer(i), string(text)};
            return t.insert_elements(arr, 0, items);
        }
    }
}

// Every kind of operation, each run in an external buffer of every size until there is no room:
// the failure is capacity and the tree, its header and its heap are as they were.
void every_operation_changes_nothing_without_room() {
    constexpr std::size_t kinds = 7;
    std::array<std::size_t, kinds> filled{};
    for (std::size_t size = 512; size <= 12288; size += 16) {
        for (std::size_t kind = 0; kind < kinds; ++kind) {
            const aligned_buffer buffer(size);
            auto r = detail::region::external_empty(buffer.bytes());
            auto t = r ? tree::format(*r) : wd::result<tree>(wd::errc::capacity);
            if (!t || !build_base(*t)) {
                continue;
            }
            const bool full = until_no_room(
                *t, *r, 100, [&t, kind](int i) { return operation_of_kind(kind, *t, i); });
            filled.at(kind) += full ? 1 : 0;
        }
    }
    for (const std::size_t f : filled) {
        BOOST_TEST_GT(f, 0U);
    }
}

// xorshift64: the same numbers on every run.
class xorshift {
public:
    explicit xorshift(std::uint64_t seed) : state_(seed * 0x9e3779b97f4a7c15U) {}

    std::uint64_t below(std::uint64_t bound) {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_ % bound;
    }

private:
    std::uint64_t state_;
};

constexpr std::string_view letters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Fills an object with strings of random lengths until there is no room, then erases one in two.
void fragment(tree& t, xorshift& random) {
    int made = 0;
    while (t.set_member(1, "h" + std::to_string(made),
                        string(letters.substr(0, 1 + random.below(letters.size() - 1))))) {
        ++made;
    }
    for (int i = 0; i < made; i += 2) {
        BOOST_TEST(*t.erase_member(1, "h" + std::to_string(i)));
    }
}

// A heap left with holes of many sizes, by strings of many lengths erased one in two, then filled
// with values that mix strings and nodes: the room an operation probes is the room it takes, in
// the order it takes it, so it completes or fails with capacity and changes nothing.
void a_fragmented_heap_keeps_its_word() {
    std::size_t filled = 0;
    for (std::size_t size = 2048; size <= 8192; size += 16) {
        for (std::uint64_t seed = 1; seed <= 4; ++seed) {
            const aligned_buffer buffer(size);
            auto r = detail::region::external_empty(buffer.bytes());
            auto t = r ? tree::format(*r) : wd::result<tree>(wd::errc::capacity);
            if (!t || !t->assign(1, object())) {
                continue;
            }
            xorshift random(seed);
            fragment(*t, random);
            const bool full = until_no_room(*t, *r, 100, [&](int i) -> wd::result<void> {
                const std::vector<entry> entries{
                    {.key = "a", .value = string(letters.substr(0, 1 + random.below(160)))},
                    {.key = "b", .value = object()},
                    {.key = "c", .value = string(letters.substr(0, 1 + random.below(160)))},
                    {.key = "d", .value = integer(i)},
                };
                const auto done = t->set_member(1, "v" + std::to_string(i),
                                                literal(wd::kind::object, std::span(entries)));
                return done ? wd::result<void>() : wd::result<void>(done.error());
            });
            filled += full ? 1 : 0;
        }
    }
    BOOST_TEST_GT(filled, 0U);
}

// The bound a node's probe asks for covers the block the container really takes.
void the_node_probe_covers_a_node() {
    fixture f;
    BOOST_TEST(f.t.assign(1, array()).has_value());
    const auto& h = detail::heap_of(f.r);
    const auto before = h.get_free_memory();
    const std::vector<detail::item<traits>> one{integer(1)};
    BOOST_TEST(f.t.insert_elements(1, 0, one).has_value());
    BOOST_TEST_LE(before - h.get_free_memory(), detail::node_probe_bytes<traits>);
    BOOST_TEST_GT(before - h.get_free_memory(), sizeof(detail::node<traits>));
}

void a_tree_reopens_after_its_image_moves() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    BOOST_TEST(f.t.set_member(1, "k", string("value")).has_value());
    const aligned_buffer moved_bytes(static_cast<std::size_t>(f.r.size()));
    std::memcpy(moved_bytes.bytes().data(), f.r.data(), static_cast<std::size_t>(f.r.size()));
    detail::region moved = detail::region::external(moved_bytes.bytes());
    auto t = tree::attach(moved);
    if (!BOOST_TEST(t.has_value())) {
        return;
    }
    BOOST_TEST_EQ(dump(*t), dump(f.t));
    BOOST_TEST_EQ(dump(*t), "{\"k\":\"value\"}");
    // It changes in its new place, and its source does not.
    BOOST_TEST(t->set_member(1, "j", integer(2)).has_value());
    BOOST_TEST_EQ(dump(*t), "{\"k\":\"value\",\"j\":2}");
    BOOST_TEST_EQ(dump(f.t), "{\"k\":\"value\"}");
    // A region moved into another object is rebound.
    detail::region taken = std::move(f.r);
    f.t.rebind(taken);
    BOOST_TEST_EQ(dump(f.t), "{\"k\":\"value\"}");
    BOOST_TEST(&f.t.storage() == &taken);
}

void attach_refuses_what_is_no_tree() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    {
        // Too small for a header and a heap: refused before the heap is read.
        std::vector<std::byte> copy(f.r.data(), f.r.data() + 100);
        const aligned_buffer small(copy.size());
        std::memcpy(small.bytes().data(), copy.data(), copy.size());
        detail::region r = detail::region::external(small.bytes());
        BOOST_TEST(tree::attach(r).error() == wd::errc::not_a_document);
    }
    {
        const aligned_buffer zeros(4096);
        std::fill(zeros.bytes().begin(), zeros.bytes().end(), std::byte{0});
        detail::region r = detail::region::external(zeros.bytes());
        BOOST_TEST(tree::attach(r).error() == wd::errc::not_a_document);
    }
    {
        // Another traits' image.
        detail::region r =
            detail::region::external({f.r.data(), static_cast<std::size_t>(f.r.size())});
        BOOST_TEST(detail::tree<wd::js_traits>::attach(r).error() ==
                   wd::errc::incompatible_document);
        BOOST_TEST(detail::tree<sorted_traits>::attach(r).error() ==
                   wd::errc::incompatible_document);
        BOOST_TEST(tree::attach(r).has_value());
    }
    {
        // A header whose container lies past the image.
        const aligned_buffer bad(static_cast<std::size_t>(f.r.size()));
        std::memcpy(bad.bytes().data(), f.r.data(), static_cast<std::size_t>(f.r.size()));
        wd::image_header h{};
        std::memcpy(&h, bad.bytes().data(), sizeof h);
        h.container = h.size;
        std::memcpy(bad.bytes().data(), &h, sizeof h);
        detail::region r = detail::region::external(bad.bytes());
        BOOST_TEST(tree::attach(r).error() == wd::errc::not_a_document);
    }
    {
        // A header that states a size smaller than its heap: the heap is not walked.
        const aligned_buffer bad(static_cast<std::size_t>(f.r.size()));
        std::memcpy(bad.bytes().data(), f.r.data(), static_cast<std::size_t>(f.r.size()));
        wd::image_header h{};
        std::memcpy(&h, bad.bytes().data(), sizeof h);
        h.size = h.container + sizeof(detail::container<traits>);
        std::memcpy(bad.bytes().data(), &h, sizeof h);
        detail::region r = detail::region::external(bad.bytes());
        BOOST_TEST(tree::attach(r).error() == wd::errc::not_a_document);
    }
}

void a_deep_copy_between_trees() {
    fixture source;
    const std::vector<entry> inner{
        {.key = "b", .value = boolean(true)},
    };
    const std::vector<entry> list{
        {.key = "", .value = integer(1)},
        {.key = "", .value = string("x")},
        {.key = "", .value = literal(wd::kind::object, std::span(inner))},
    };
    const std::vector<entry> outer{
        {.key = "a", .value = literal(wd::kind::array, std::span(list))},
    };
    BOOST_TEST(source.t.assign(1, literal(wd::kind::object, std::span(outer))).has_value());
    BOOST_TEST_EQ(dump(source.t), "{\"a\":[1,\"x\",{\"b\":true}]}");
    fixture target;
    BOOST_TEST(target.t.assign(1, copy_of(source.t, id_of(source.t, 1, "a"))).has_value());
    BOOST_TEST_EQ(dump(target.t), "[1,\"x\",{\"b\":true}]");
    BOOST_TEST_EQ(target.t.count(1), 3U);
    BOOST_TEST_EQ(target.t.count(target.t.element(1, 2)->id), 1U);
    // The source changes; the copy does not.
    BOOST_TEST(source.t.set_element(id_of(source.t, 1, "a"), 1, string("changed")).has_value());
    BOOST_TEST(*source.t.erase_member(source.t.element(id_of(source.t, 1, "a"), 2)->id, "b"));
    BOOST_TEST_EQ(dump(source.t), "{\"a\":[1,\"changed\",{}]}");
    BOOST_TEST_EQ(dump(target.t), "[1,\"x\",{\"b\":true}]");
    // A copy of nothing is a type error.
    BOOST_TEST(target.t.assign(1, copy_of(source.t, 99)).error() == wd::errc::type);
    const tree nothing;
    BOOST_TEST(target.t.assign(1, copy_of(nothing, 1)).error() == wd::errc::type);
}

// A copy into its own descendant copies the subtree as it was before the copy began, here in an
// owned image that grows while it copies.
void a_copy_into_its_own_descendant() {
    auto r = detail::region::owned(1024, 16 * mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto t = tree::format(*r);
    if (!BOOST_TEST(t.has_value())) {
        return;
    }
    BOOST_TEST(t->assign(1, object()).has_value());
    const auto a = t->set_member(1, "a", object());
    if (!BOOST_TEST(a.has_value())) {
        return;
    }
    const auto b = t->set_member(*a, "b", array());
    if (!BOOST_TEST(b.has_value())) {
        return;
    }
    const std::vector<detail::item<traits>> items{
        integer(1),
        string("a string long enough to take a block of its own"),
    };
    BOOST_TEST(t->insert_elements(*b, 0, items).has_value());
    const std::uint64_t size = r->size();
    for (int i = 0; i < 4; ++i) {
        BOOST_TEST(t->assign(id_of(*t, *a, "b"), copy_of(*t, 1)).has_value());
    }
    BOOST_TEST_GT(r->size(), size);
    BOOST_TEST_EQ(header_of(*r).size, r->size());
    std::string expected = "[1,\"a string long enough to take a block of its own\"]";
    for (int i = 0; i < 4; ++i) {
        expected.insert(0, R"({"a":{"b":)");
        expected += "}}";
    }
    BOOST_TEST_EQ(dump(*t), "{\"a\":{\"b\":" + expected + "}}");
    BOOST_TEST(detail::heap_of(*r).check_sanity());
}

// Views into the image that an item holds, its text and a literal's keys, are read before the
// image moves.
void views_into_the_image_survive_growth() {
    auto r = detail::region::owned(1024, 16 * mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto t = tree::format(*r);
    if (!BOOST_TEST(t.has_value())) {
        return;
    }
    BOOST_TEST(t->assign(1, object()).has_value());
    const std::string long_key(300, 'k');
    const std::string long_text(400, 't');
    BOOST_TEST(t->set_member(1, long_key, string(long_text)).has_value());
    for (int i = 0; i < 6; ++i) {
        const detail::node<traits>* source = t->member(1, long_key);
        if (!BOOST_TEST(source != nullptr)) {
            return;
        }
        const std::string_view key = source->key.chars();
        const std::vector<entry> entries{
            {.key = key, .value = string(source->text.chars())},
            {.key = "n", .value = integer(i)},
        };
        const std::string name = "copy" + std::to_string(i);
        BOOST_TEST(
            t->set_member(1, name, literal(wd::kind::object, std::span(entries))).has_value());
        const auto* copy = t->member(id_of(*t, 1, name), long_key);
        if (!BOOST_TEST(copy != nullptr)) {
            return;
        }
        BOOST_TEST(copy->text.chars() == long_text);
    }
    BOOST_TEST_GT(r->size(), 1024U);
}

// An owned image grows as operations need, keeps its header's size, and refuses past max_bytes.
void growth_moves_the_image() {
    auto r = detail::region::owned(1024, std::uint64_t{64} * 1024);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto t = tree::format(*r);
    if (!BOOST_TEST(t.has_value())) {
        return;
    }
    BOOST_TEST(t->assign(1, array()).has_value());
    std::size_t grown = 0;
    std::uint64_t size = r->size();
    bool full = false;
    for (int i = 0; i < 10000 && !full; ++i) {
        const std::string before = dump(*t);
        const std::vector<detail::item<traits>> items{
            integer(i),
            string("a string to fill the image"),
        };
        const auto done = t->insert_elements(1, t->count(1), items);
        if (!done) {
            BOOST_TEST(done.error() == wd::errc::capacity);
            BOOST_TEST_EQ(dump(*t), before);
            full = true;
        }
        BOOST_TEST_EQ(header_of(*r).size, r->size());
        if (r->size() != size) {
            ++grown;
            size = r->size();
        }
    }
    BOOST_TEST(full);
    BOOST_TEST_GE(grown, 3U);
    BOOST_TEST_LE(r->size(), 64U * 1024U);
    BOOST_TEST_EQ(t->element(1, 1)->text.chars(), "a string to fill the image");
    BOOST_TEST(wd::check_image(image_of(*r), detail::layout_tag<traits>()).has_value());
}

void splice_as_one() {
    fixture f;
    BOOST_TEST(f.t.assign(1, array()).has_value());
    const std::vector<detail::item<traits>> five{
        integer(0), integer(1), integer(2), integer(3), integer(4),
    };
    BOOST_TEST(f.t.insert_elements(1, 0, five).has_value());
    // More in than out, then fewer, then none, then at the end.
    const std::vector<detail::item<traits>> three{string("a"), object(), integer(9)};
    BOOST_TEST(f.t.splice(1, 1, 2, three).has_value());
    BOOST_TEST_EQ(dump(f.t), "[0,\"a\",{},9,3,4]");
    const std::vector<detail::item<traits>> one{boolean(false)};
    BOOST_TEST(f.t.splice(1, 0, 4, one).has_value());
    BOOST_TEST_EQ(dump(f.t), "[false,3,4]");
    BOOST_TEST(f.t.splice(1, 1, 1, {}).has_value());
    BOOST_TEST_EQ(dump(f.t), "[false,4]");
    BOOST_TEST(f.t.splice(1, 2, 0, three).has_value());
    BOOST_TEST_EQ(dump(f.t), "[false,4,\"a\",{},9]");
    BOOST_TEST_EQ(f.t.count(1), 5U);
    BOOST_TEST_EQ(f.t.node_count(), 6U);
    for (std::uint64_t i = 0; i < 5; ++i) {
        BOOST_TEST_EQ(f.t.element(1, i)->ordinal, i);
    }
}

void set_element_past_the_end() {
    fixture f;
    BOOST_TEST(f.t.assign(1, array()).has_value());
    BOOST_TEST(f.t.set_element(1, 0, integer(7)).has_value());
    BOOST_TEST(f.t.set_element(1, 3, string("x")).has_value());
    BOOST_TEST_EQ(dump(f.t), "[7,null,null,\"x\"]");
    BOOST_TEST(f.t.set_element(1, 1, integer(8)).has_value());
    BOOST_TEST_EQ(dump(f.t), "[7,8,null,\"x\"]");
    BOOST_TEST_EQ(f.t.count(1), 4U);
    BOOST_TEST(f.t.set_element(1, 4, array()).has_value());
    BOOST_TEST_EQ(dump(f.t), "[7,8,null,\"x\",[]]");
}

void set_members_in_order() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    const auto x = f.t.set_member(1, "x", object());
    if (!BOOST_TEST(x.has_value())) {
        return;
    }
    BOOST_TEST(f.t.set_member(*x, "inner", integer(1)).has_value());
    // A repeated key keeps its first place and takes its last value, as Object.assign does.
    const std::vector<entry> members{
        {.key = "b", .value = integer(1)}, {.key = "2", .value = integer(2)},
        {.key = "x", .value = integer(9)}, {.key = "b", .value = integer(3)},
        {.key = "a", .value = integer(4)},
    };
    BOOST_TEST(f.t.set_members(1, members).has_value());
    BOOST_TEST_EQ(dump(f.t), "{\"2\":2,\"x\":9,\"b\":3,\"a\":4}");
    BOOST_TEST_EQ(f.t.count(1), 4U);
    BOOST_TEST_EQ(f.t.node_count(), 5U);
    BOOST_TEST_EQ(f.t.member(1, "x")->id, *x);
    // A key given in no UTF-8, or an undefined value, refuses the whole set.
    const std::vector<entry> bad_key{
        {.key = "c", .value = integer(1)},
        {.key = "\xff", .value = integer(2)},
    };
    BOOST_TEST(f.t.set_members(1, bad_key).error() == wd::errc::type);
    const std::vector<entry> bad_value{
        {.key = "c", .value = integer(1)},
        {.key = "d", .value = undefined()},
    };
    BOOST_TEST(f.t.set_members(1, bad_value).error() == wd::errc::type);
    BOOST_TEST_EQ(dump(f.t), "{\"2\":2,\"x\":9,\"b\":3,\"a\":4}");
    BOOST_TEST(f.t.set_members(1, {}).has_value());
}

// A literal is stored whole, nested, and a repeated key in an object literal keeps its first place
// and its last value, as JSON.parse does.
void literals_are_stored_whole() {
    fixture f;
    const std::vector<entry> empties{
        {.key = "", .value = object()},
        {.key = "", .value = array()},
    };
    const std::vector<entry> entries{
        {.key = "a", .value = integer(1)},
        {.key = "1", .value = literal(wd::kind::array, std::span(empties))},
        {.key = "b", .value = null()},
        {.key = "a", .value = string("last")},
    };
    BOOST_TEST(f.t.assign(1, literal(wd::kind::object, std::span(entries))).has_value());
    BOOST_TEST_EQ(dump(f.t), "{\"1\":[{},[]],\"a\":\"last\",\"b\":null}");
    BOOST_TEST_EQ(f.t.count(1), 3U);
    BOOST_TEST_EQ(f.t.node_count(), 6U);
    const std::vector<entry> undefined_entry{
        {.key = "u", .value = undefined()},
    };
    BOOST_TEST(f.t.assign(1, literal(wd::kind::object, std::span(undefined_entry))).error() ==
               wd::errc::type);
    BOOST_TEST_EQ(dump(f.t), "{\"1\":[{},[]],\"a\":\"last\",\"b\":null}");
}

// Only a root holds undefined: a document that holds nothing.
void an_undefined_root() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    const auto a = f.t.set_member(1, "a", integer(1));
    if (!BOOST_TEST(a.has_value())) {
        return;
    }
    BOOST_TEST(f.t.assign(*a, undefined()).error() == wd::errc::type);
    BOOST_TEST(f.t.set_member(1, "b", undefined()).error() == wd::errc::type);
    BOOST_TEST(f.t.assign(1, undefined()).has_value());
    BOOST_TEST(f.t.find(1)->kind == wd::kind::undefined);
    BOOST_TEST_EQ(f.t.node_count(), 1U);
    BOOST_TEST_EQ(dump(f.t), "undefined");
    BOOST_TEST(f.t.assign(1, array()).has_value());
    const std::vector<detail::item<traits>> one{undefined()};
    BOOST_TEST(f.t.insert_elements(1, 0, one).error() == wd::errc::type);
    BOOST_TEST(f.t.set_element(1, 0, undefined()).error() == wd::errc::type);
    BOOST_TEST_EQ(dump(f.t), "[]");
    // A copy of an undefined root goes only to a root.
    fixture other;
    BOOST_TEST(other.t.assign(1, undefined()).has_value());
    BOOST_TEST(f.t.set_element(1, 0, copy_of(other.t, 1)).error() == wd::errc::type);
    BOOST_TEST(f.t.assign(1, copy_of(other.t, 1)).has_value());
    BOOST_TEST_EQ(dump(f.t), "undefined");
}

template <class Traits>
void siblings_agree_with_for_each_child() {
    fixture<Traits> f;
    BOOST_TEST(f.t.assign(1, object<Traits>()).has_value());
    for (const std::string_view k : {"b", "2", "a", "", "1", "10", "z"}) {
        BOOST_TEST(f.t.set_member(1, k, integer<Traits>(0)).has_value());
    }
    const auto arr = f.t.set_member(1, "arr", array<Traits>());
    if (!BOOST_TEST(arr.has_value())) {
        return;
    }
    const std::vector<detail::item<Traits>> items{
        integer<Traits>(1),
        integer<Traits>(2),
        integer<Traits>(3),
    };
    BOOST_TEST(f.t.insert_elements(*arr, 0, items).has_value());
    for (const std::uint64_t parent : {std::uint64_t{1}, *arr}) {
        std::vector<std::uint64_t> walked;
        for (const auto* c = f.t.first_child(parent); c != nullptr; c = f.t.next_sibling(*c)) {
            walked.push_back(c->id);
        }
        std::vector<std::uint64_t> visited;
        f.t.for_each_child(parent, [&](const detail::node<Traits>& c) { visited.push_back(c.id); });
        BOOST_TEST(walked == visited);
        BOOST_TEST_EQ(walked.size(), f.t.count(parent));
    }
    BOOST_TEST(f.t.first_child(id_of(f.t, 1, "b")) == nullptr);
    BOOST_TEST(f.t.first_child(99) == nullptr);
}

void iteration_by_siblings() {
    siblings_agree_with_for_each_child<traits>();
    siblings_agree_with_for_each_child<insertion_traits>();
    siblings_agree_with_for_each_child<sorted_traits>();
    siblings_agree_with_for_each_child<sorted16_traits>();
}

// The image is a function of the operations alone, at any address.
void the_same_operations_give_the_same_image() {
    const auto build = [](detail::region& r) {
        auto t = tree::format(r);
        webcpp::test::require(BOOST_TEST(t.has_value()));
        webcpp::test::require(BOOST_TEST(build_base(*t)));
        const std::vector<detail::item<traits>> items{string("abc"), object(), integer(5)};
        for (int i = 0; i < 20; ++i) {
            webcpp::test::require(BOOST_TEST(t->insert_elements(id_of(*t, 1, "arr"), 1, items)));
            webcpp::test::require(BOOST_TEST(t->erase_elements(id_of(*t, 1, "arr"), 0, 2)));
            webcpp::test::require(BOOST_TEST(t->assign(id_of(*t, 1, "a"), copy_of(*t, 1))));
            webcpp::test::require(BOOST_TEST(t->erase_member(1, "a")));
            webcpp::test::require(BOOST_TEST(t->set_member(1, "a", object())));
        }
    };
    auto a = detail::region::owned(1024, 16 * mebibyte);
    auto b = detail::region::owned(1024, 16 * mebibyte);
    if (!BOOST_TEST(a.has_value() && b.has_value())) {
        return;
    }
    build(*a);
    build(*b);
    BOOST_TEST_EQ(a->size(), b->size());
    BOOST_TEST(std::ranges::equal(image_of(*a), image_of(*b)));
}

// Strings and nodes are zeroed when freed, so an erased value leaves none of its bytes.
void an_erased_value_leaves_no_byte() {
    fixture f;
    BOOST_TEST(f.t.assign(1, object()).has_value());
    const std::string secret_key = "a secret key that is long enough";
    BOOST_TEST(
        f.t.set_member(1, secret_key, string("a secret value that is long enough")).has_value());
    BOOST_TEST(f.t.set_member(1, "kept", string("a secret replaced, long enough")).has_value());
    BOOST_TEST(contains(image_of(f.r), "a secret key that"));
    BOOST_TEST(contains(image_of(f.r), "a secret value that"));
    BOOST_TEST(*f.t.erase_member(1, secret_key));
    BOOST_TEST(f.t.set_member(1, "kept", integer(1)).has_value());
    BOOST_TEST(!contains(image_of(f.r), "a secret key that"));
    BOOST_TEST(!contains(image_of(f.r), "a secret value that"));
    BOOST_TEST(!contains(image_of(f.r), "a secret replaced"));
}

void shrink_to_fit() {
    auto r = detail::region::owned(1024, 16 * mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto t = tree::format(*r);
    if (!BOOST_TEST(t.has_value())) {
        return;
    }
    BOOST_TEST(t->assign(1, array()).has_value());
    for (int i = 0; i < 50; ++i) {
        const std::vector<detail::item<traits>> one{string("a string to fill the image")};
        BOOST_TEST(t->insert_elements(1, 0, one).has_value());
    }
    BOOST_TEST(t->set_length(1, 1).has_value());
    const std::string before = dump(*t);
    const std::uint64_t grown = r->size();
    BOOST_TEST(t->shrink_to_fit().has_value());
    BOOST_TEST_LT(r->size(), grown);
    BOOST_TEST_EQ(header_of(*r).size, r->size());
    BOOST_TEST_EQ(dump(*t), before);
    BOOST_TEST(wd::check_image(image_of(*r), detail::layout_tag<traits>()).has_value());
    // It grows again from its smaller size.
    for (int i = 0; i < 50; ++i) {
        const std::vector<detail::item<traits>> one{string("another string to fill the image")};
        BOOST_TEST(t->insert_elements(1, 0, one).has_value());
    }
    BOOST_TEST_EQ(t->count(1), 51U);
    BOOST_TEST(detail::heap_of(*r).check_sanity());
    // An external image keeps its buffer.
    const aligned_buffer buffer(4096);
    auto e = detail::region::external_empty(buffer.bytes());
    if (!BOOST_TEST(e.has_value())) {
        return;
    }
    auto et = tree::format(*e);
    if (!BOOST_TEST(et.has_value())) {
        return;
    }
    BOOST_TEST(et->shrink_to_fit().has_value());
    BOOST_TEST_EQ(e->size(), 4096U);
    BOOST_TEST_EQ(dump(*et), "null");
}

void a_tree_attached_to_nothing() {
    tree t;
    BOOST_TEST_EQ(t.root(), 1U);
    BOOST_TEST(t.find(1) == nullptr);
    BOOST_TEST(t.member(1, "a") == nullptr);
    BOOST_TEST(t.element(1, 0) == nullptr);
    BOOST_TEST(t.first_child(1) == nullptr);
    BOOST_TEST_EQ(t.count(1), 0U);
    BOOST_TEST_EQ(t.node_count(), 0U);
    int calls = 0;
    t.for_each_child(1, [&](const detail::node<traits>&) { ++calls; });
    BOOST_TEST_EQ(calls, 0);
    BOOST_TEST(t.assign(1, integer(1)).error() == wd::errc::type);
    BOOST_TEST(t.set_member(1, "a", integer(1)).error() == wd::errc::type);
    BOOST_TEST(t.erase_member(1, "a").error() == wd::errc::type);
    BOOST_TEST(t.set_length(1, 1).error() == wd::errc::type);
    BOOST_TEST(t.shrink_to_fit().error() == wd::errc::type);
}

// A format of a region too small for an empty tree is capacity.
void format_needs_room() {
    detail::region none;
    BOOST_TEST(tree::format(none).error() == wd::errc::capacity);
    const aligned_buffer buffer(4096);
    std::size_t smallest = 0;
    for (std::size_t size = 0; size <= 4096; size += 16) {
        auto r = detail::region::external_empty(buffer.bytes().first(size));
        if (!r) {
            continue;
        }
        const auto t = tree::format(*r);
        if (t) {
            smallest = size;
            break;
        }
        BOOST_TEST(t.error() == wd::errc::capacity);
    }
    BOOST_TEST_GT(smallest, 0U);
    // An owned region of the smallest size a heap takes grows to hold the tree.
    auto r = detail::region::owned(
        detail::header_bytes + detail::heap::get_min_size(sizeof(detail::heap)), 16 * mebibyte);
    if (!BOOST_TEST(r.has_value())) {
        return;
    }
    auto t = tree::format(*r);
    if (!BOOST_TEST(t.has_value())) {
        return;
    }
    BOOST_TEST_EQ(dump(*t), "null");
    BOOST_TEST_EQ(header_of(*r).size, r->size());
}

}  // namespace

int main() {
    a_new_tree_holds_null();
    layout_tags_differ();
    javascript_key_order();
    index_lookalikes();
    insertion_and_sorted_orders();
    sorted_by_code_units();
    utf16_keys();
    scalars_are_stored_as_given();
    elements_are_renumbered();
    wrong_places_are_refused();
    replacing_a_container_frees_its_subtree();
    nothing_changes_when_there_is_no_room();
    every_operation_changes_nothing_without_room();
    a_fragmented_heap_keeps_its_word();
    the_node_probe_covers_a_node();
    a_tree_reopens_after_its_image_moves();
    attach_refuses_what_is_no_tree();
    a_deep_copy_between_trees();
    a_copy_into_its_own_descendant();
    views_into_the_image_survive_growth();
    growth_moves_the_image();
    splice_as_one();
    set_element_past_the_end();
    set_members_in_order();
    literals_are_stored_whole();
    an_undefined_root();
    iteration_by_siblings();
    the_same_operations_give_the_same_image();
    an_erased_value_leaves_no_byte();
    shrink_to_fit();
    a_tree_attached_to_nothing();
    format_needs_room();
    return boost::report_errors();
}
