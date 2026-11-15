// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The tree's working state in the document's allocator: what a change stages before it changes
// anything (its nodes, the values still to read, the sizes it probes, the subtrees it erases, a
// literal's keys, and the copies of what it views in an image that must move) and compaction's walk
// are allocated through the document's allocator, rebound, with no call of the global operator
// new, under the default traits and JavaScript's, and all of it is given back; a member is found by
// a key of any length with no allocation at all; and, with exceptions, an allocator that fails at
// any of an operation's allocations makes it errc::capacity with the document unchanged.

#include <webcpp/document.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/hash2/fnv1a.hpp>

#include "counting.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;
namespace detail = webcpp::document::detail;

namespace {

using webcpp::test::arena;
using webcpp::test::failing_resource;
using webcpp::test::global_count;
using webcpp::test::require;

/** The allocator of every document and tree here: a memory resource's. */
using pmr_allocator = std::pmr::polymorphic_allocator<std::byte>;

/** The region of a tree in a memory resource. */
using pmr_region = detail::basic_region<pmr_allocator>;

/** A tree of `Traits` in a memory resource. */
template <class Traits>
using pmr_tree = detail::tree<Traits, pmr_allocator>;

constexpr std::size_t mebibyte = std::size_t{1} << 20U;

/** The default traits, but every object's keys in order of their UTF-16 code units. */
struct sorted_traits : wd::default_traits {
    /** Keys sorted. */
    using object = wd::sorted;
};

/** JavaScript's traits, but every object's keys in the order they were inserted. */
struct insertion_traits : wd::js_traits {
    /** Keys in insertion order. */
    using object = wd::insertion_order;
};

/** The size an image here may grow to. */
constexpr std::uint64_t max_bytes = std::uint64_t{64} << 20U;

/** A string past any small-string buffer, which `set` stores without copying it. */
constexpr std::string_view long_text =
    "A string longer than the small buffer of any std::string, which the tree copies into the "
    "image as it is given.";

/** The same string in UTF-16, which `set` stores in a document of JavaScript's traits. */
constexpr std::u16string_view long_text16 =
    u"A string longer than the small buffer of any std::string, which the tree copies into the "
    u"image as it is given.";

static_assert(long_text.size() >= 100 && long_text16.size() == long_text.size());

/** The long string in the encoding of `Traits`, which `set` stores without copying it. */
template <class Traits>
auto long_string_of() {
    if constexpr (std::same_as<typename Traits::string, wd::utf16>) {
        return long_text16;
    } else {
        return long_text;
    }
}

/**
 A key in UTF-8 past 64 UTF-16 code units and any small-string buffer, ending with `n`: 12 times a
 stem of 10 bytes and 7 units, one of its characters past U+FFFF.
*/
std::string long_key(std::size_t n) {
    std::string key;
    for (int i = 0; i < 12; ++i) {
        key += "cl\xC3\xA9-\xF0\x9F\x98\x80-";
    }
    key += std::to_string(n);
    return key;
}

/** The value of a result that must hold one. */
template <class T>
T take(wd::result<T> made) {
    require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

/** Sets the value `i` of a run of values: an integer, a boolean, null or a long string. */
template <class Traits, class Handle>
wd::result<void> set_value(Handle& h, std::size_t i) {
    switch (i % 4) {
        case 0: return h.set(static_cast<std::int64_t>(i));
        case 1: return h.set(i % 8 == 1);
        case 2: return h.set(nullptr);
        default: return h.set(long_string_of<Traits>());
    }
}

// Members with long keys, set, assigned, found and erased, grow an image several times with no
// call of the global operator new: the tree stages each change in the document's resource, and
// gives it all back.
template <class Traits>
void members_in_resource() {
    using document = wd::pmr::basic_document<Traits>;
    constexpr std::size_t members = 240;
    arena a(16 * mebibyte);
    {
        document d = take(document::create(a.allocator()));
        require(BOOST_TEST(d.root().set(wd::object{}).has_value()));
        // The keys, and the handles that name them, are made before anything is counted.
        std::vector<std::string> keys;
        keys.reserve(members);
        for (std::size_t i = 0; i < members; ++i) {
            keys.push_back(long_key(i));
        }
        std::vector<decltype(d.root())> places;
        places.reserve(members);
        for (const std::string& key : keys) {
            places.push_back(d.root()[key]);
        }
        const std::uint64_t first_capacity = d.capacity();
        const std::uint64_t taken = a.resource().allocations();
        {
            const global_count global;
            for (std::size_t i = 0; i < members; ++i) {
                BOOST_TEST(set_value<Traits>(places[i], i).has_value());
            }
            BOOST_TEST(places[0].set(true).has_value());
            BOOST_TEST(d.root().contains(keys[1]));
            BOOST_TEST(places[2].set(wd::undefined).has_value());
            const auto erased = d.root().erase(keys[3]);
            BOOST_TEST(erased.has_value() && *erased);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        BOOST_TEST_GT(a.resource().allocations(), taken);
        // Grown from 4,096 bytes, at least doubling each time, four times or more.
        BOOST_TEST_GE(d.capacity(), 16 * first_capacity);
        BOOST_TEST_EQ(d.root().size(), members - 2);
        BOOST_TEST(d.root()[keys[0]].is_boolean());
        BOOST_TEST(d.root()[keys[members - 1]].is_string());
        BOOST_TEST(!d.root().contains(keys[2]));
        BOOST_TEST(!d.root().contains(keys[3]));
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// The array methods stage their changes in the document's resource: push, unshift, splice, pop
// and shift, whose documents are the resource's too, reverse, and set_length, which appends and
// erases.
template <class Traits>
void arrays_in_resource() {
    using document = wd::pmr::basic_document<Traits>;
    arena a(16 * mebibyte);
    {
        document d = take(document::create(a.allocator()));
        require(BOOST_TEST(d.root().set(wd::array{}).has_value()));
        auto list = d.root();
        const auto text = long_string_of<Traits>();
        const std::uint64_t taken = a.resource().allocations();
        {
            const global_count global;
            for (std::int64_t i = 0; i < 50; ++i) {
                BOOST_TEST(list.push(i, true, nullptr, text).has_value());
            }
            BOOST_TEST(list.unshift(std::int64_t{-1}, text).has_value());
            BOOST_TEST(list.splice(1, 0, false, text).has_value());
            const auto last = list.pop();
            BOOST_TEST(last.has_value() && last->root().is_string());
            const auto first = list.shift();
            BOOST_TEST(first.has_value() && first->root().is_number());
            BOOST_TEST(list.reverse().has_value());
            BOOST_TEST(list.set_length(250).has_value());
            BOOST_TEST(list.set_length(10).has_value());
            BOOST_TEST_EQ(global.news(), 0U);
        }
        BOOST_TEST_GT(a.resource().allocations(), taken);
        BOOST_TEST_EQ(d.root().size(), 10U);
        // Reversed, the list starts with the last push's values, its string popped.
        BOOST_TEST(d.root()[std::uint64_t{0}].is_null());
        BOOST_TEST(d.root()[std::uint64_t{1}].is_boolean());
        BOOST_TEST(d.root()[std::uint64_t{2}].is_number());
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

/** A JSON text of nested containers, strings and scalars, for deep copies and failures. */
constexpr std::string_view sample_json = R"({
    "small": 1,
    "nested": {"a": [1, 2, {"deep": [[["x", {"y": [true]}]]]}], "b": true, "c": "text"},
    "list": [1, "two", {"x": 1}, [1, 2, 3], null]
})";

/** The sample, with a long string at a long key, parsed into `alloc`. */
template <class Traits>
wd::pmr::basic_document<Traits> sample(const pmr_allocator& alloc) {
    auto d = take(wd::parse<Traits>(sample_json, wd::parse_options(), alloc));
    require(BOOST_TEST(d.root()[long_key(0)].set(long_string_of<Traits>()).has_value()));
    return d;
}

// A deep copy stages its nodes in the document's resource: of a document in another resource,
// and of the document's own value into an image that must grow first, whose views into the image
// are copied, in the resource, before it moves; and shrink_to_fit, reserve and compact make no
// call of the global operator new either.
template <class Traits>
void copies_in_resource() {
    using document = wd::pmr::basic_document<Traits>;
    arena a(16 * mebibyte);
    arena b(4 * mebibyte);
    {
        const document source = sample<Traits>(b.allocator());
        document d = take(document::create(a.allocator()));
        require(BOOST_TEST(d.root().set(wd::object{}).has_value()));
        auto copy = d.root()["copy"];
        auto again = d.root()["again"];
        const std::uint64_t taken = a.resource().allocations();
        {
            const global_count global;
            BOOST_TEST(copy.set(source).has_value());
            BOOST_TEST(d.shrink_to_fit().has_value());
            const std::uint64_t full = d.capacity();
            BOOST_TEST(again.set(d.root()["copy"]).has_value());
            BOOST_TEST_GT(d.capacity(), full);
            BOOST_TEST(d.root()["copy"].set(std::int64_t{1}).has_value());
            BOOST_TEST(d.reserve(2 * d.capacity()).has_value());
            BOOST_TEST(d.compact().has_value());
            BOOST_TEST_EQ(global.news(), 0U);
        }
        BOOST_TEST_GT(a.resource().allocations(), taken);
        BOOST_TEST(d.root()["again"] == source.root());
        BOOST_TEST(d.root()["copy"].is_number());
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
    BOOST_TEST_EQ(b.resource().bytes_in_use(), 0U);
}

/** An item holding the integer `v`. */
template <class Traits>
detail::item<Traits> integer(std::int64_t v) {
    detail::item<Traits> i;
    i.kind = wd::kind::number;
    i.number = detail::repr::integer;
    i.store(v);
    return i;
}

/** An item holding `v`. */
template <class Traits>
detail::item<Traits> boolean(bool v) {
    detail::item<Traits> i;
    i.kind = wd::kind::boolean;
    i.store(v);
    return i;
}

/** An item holding the long string, in the traits' encoding. */
template <class Traits>
detail::item<Traits> long_string() {
    detail::item<Traits> i;
    i.kind = wd::kind::string;
    i.text = std::as_bytes(std::span(long_string_of<Traits>()));
    return i;
}

/** An item holding an empty container of `k`, or a literal one of `entries`, which outlive it. */
template <class Traits>
detail::item<Traits> container(wd::kind k,
                               std::span<const detail::literal_entry<Traits>> entries = {}) {
    detail::item<Traits> i;
    i.kind = k;
    i.entries = entries.empty() ? nullptr : entries.data();
    i.entry_count = entries.size();
    return i;
}

/** An item that copies the node `id` of `from`, deeply. */
template <class Traits>
detail::item<Traits> copy_of(const detail::tree_view<Traits>& from, std::uint64_t id) {
    detail::item<Traits> i;
    i.from = &from;
    i.from_id = id;
    return i;
}

/** A tree in a region of its own, in a memory resource; it is not moved, its tree naming it. */
template <class Traits>
struct pmr_fixture {
    /** A new tree holding null, in `alloc`. */
    explicit pmr_fixture(const pmr_allocator& alloc)
        : r(take(pmr_region::owned(4096, max_bytes, alloc))),
          t(take(pmr_tree<Traits>::format(r))) {}

    /** A tree over a copy of the image of `other`, in `alloc`. */
    pmr_fixture(const pmr_fixture& other, const pmr_allocator& alloc)
        : r(take(pmr_region::owned_copy(other.image(), max_bytes, alloc))),
          t(take(pmr_tree<Traits>::attach(r))) {}

    pmr_fixture(const pmr_fixture&) = delete;
    pmr_fixture& operator=(const pmr_fixture&) = delete;
    pmr_fixture(pmr_fixture&&) = delete;
    pmr_fixture& operator=(pmr_fixture&&) = delete;
    ~pmr_fixture() = default;

    /** The image. */
    [[nodiscard]] std::span<const std::byte> image() const {
        return {r.data(), static_cast<std::size_t>(r.size())};
    }

    /** The image's next id. */
    [[nodiscard]] std::uint64_t next_id() const {
        wd::image_header header{};
        std::memcpy(&header, r.data(), sizeof header);
        return header.next_id;
    }

    /** The region. */
    pmr_region r;

    /** The tree over it. */
    pmr_tree<Traits> t;
};

/** The id of the member `key` of `object`, which must be there. */
template <class Traits>
std::uint64_t id_of(const detail::tree_view<Traits>& t, std::uint64_t object,
                    std::string_view key) {
    const detail::node<Traits>* n = t.member(object, key);
    require(BOOST_TEST(n != nullptr));
    return n->id;
}

/**
 Whether two trees hold the same nodes, each with the same id, place, key and value, and the same
 next id: what an operation that fails must leave as it was. It allocates nothing.
*/
template <class Traits>
bool same_tree(const pmr_fixture<Traits>& a, const pmr_fixture<Traits>& b) {
    const auto same_node = [](const detail::node<Traits>& x, const detail::node<Traits>& y) {
        return x.id == y.id && x.parent == y.parent && x.group == y.group &&
               x.ordinal == y.ordinal && x.kind == y.kind && x.number == y.number &&
               x.scalar == y.scalar && std::ranges::equal(x.key.view(), y.key.view()) &&
               std::ranges::equal(x.text.view(), y.text.view());
    };
    return a.next_id() == b.next_id() &&
           std::ranges::equal(a.t.nodes()->template get<detail::by_id>(),
                              b.t.nodes()->template get<detail::by_id>(), same_node);
}

/**
 Builds in `t` the sample's shape: an object of a small member, a nested object, a list holding a
 copy of it, and a long string at a long key; returns the ids of the nested object and the list.
*/
template <class Traits>
std::pair<std::uint64_t, std::uint64_t> build_sample(pmr_tree<Traits>& t) {
    const std::uint64_t root = detail::root_id;
    require(BOOST_TEST(t.assign(root, container<Traits>(wd::kind::object)).has_value()));
    require(BOOST_TEST(t.set_member(root, "small", integer<Traits>(1)).has_value()));
    const std::uint64_t nested =
        take(t.set_member(root, "nested", container<Traits>(wd::kind::object)));
    const std::uint64_t inner = take(t.set_member(nested, "a", container<Traits>(wd::kind::array)));
    for (std::uint64_t i = 0; i < 6; ++i) {
        const auto value =
            i % 2 == 0 ? long_string<Traits>() : integer<Traits>(static_cast<std::int64_t>(i));
        require(BOOST_TEST(t.set_element(inner, i, value).has_value()));
    }
    require(BOOST_TEST(t.set_member(nested, long_key(1), boolean<Traits>(true)).has_value()));
    const std::uint64_t list = take(t.set_member(root, "list", container<Traits>(wd::kind::array)));
    for (std::uint64_t i = 0; i < 5; ++i) {
        require(BOOST_TEST(
            t.set_element(list, i, integer<Traits>(static_cast<std::int64_t>(i))).has_value()));
    }
    require(BOOST_TEST(t.set_element(list, 2, copy_of<Traits>(t, nested)).has_value()));
    require(BOOST_TEST(t.set_member(root, long_key(0), long_string<Traits>()).has_value()));
    return {nested, list};
}

// The tree's own operations, which no handle or conversion takes part in, stage in the region's
// resource: members with long keys, whose UTF-16 the tree writes into the image itself, a
// literal's members, a repeated key keeping its first place and its last value, lookups by long
// keys, a splice that erases a subtree and copies one, and erasures.
template <class Traits>
void tree_operations_in_resource() {
    using entry = detail::literal_entry<Traits>;
    const std::uint64_t root = detail::root_id;
    arena a(4 * mebibyte);
    {
        pmr_fixture<Traits> f(a.allocator());
        const auto [nested, list] = build_sample(f.t);
        const std::array<std::string, 3> keys{long_key(10), long_key(11), long_key(12)};
        const std::string nested_key = long_key(1);
        const std::string near_miss = keys[0] + "x";
        const std::string_view prefix = std::string_view(keys[0]).substr(0, keys[0].size() - 1);
        const auto inner = std::to_array<entry>({
            {.key = keys[2], .value = integer<Traits>(7)},
            {.key = "x", .value = boolean<Traits>(true)},
        });
        const auto entries = std::to_array<entry>({
            {.key = "x", .value = integer<Traits>(1)},
            {.key = keys[1], .value = long_string<Traits>()},
            {.key = "x", .value = integer<Traits>(2)},
            {.key = "inner", .value = container<Traits>(wd::kind::object, inner)},
            {.key = keys[0], .value = integer<Traits>(3)},
        });
        const std::array<detail::item<Traits>, 2> inserted{
            copy_of<Traits>(f.t, nested),
            long_string<Traits>(),
        };
        const std::uint64_t taken = a.resource().allocations();
        std::uint64_t first = 0;
        {
            const global_count global;
            const auto set = f.t.set_member(root, keys[0], long_string<Traits>());
            BOOST_TEST(set.has_value());
            first = set.has_value() ? *set : 0;
            BOOST_TEST(f.t.set_members(root, entries).has_value());
            const detail::node<Traits>* found = f.t.member(root, keys[0]);
            BOOST_TEST(found != nullptr && found->id == first);
            BOOST_TEST(f.t.member(root, near_miss) == nullptr);
            BOOST_TEST(f.t.member(root, prefix) == nullptr);
            BOOST_TEST(f.t.member(nested, nested_key) != nullptr);
            BOOST_TEST(f.t.splice(list, 1, 2, inserted).has_value());
            const auto erased = f.t.erase_member(root, keys[1]);
            BOOST_TEST(erased.has_value() && *erased);
            const auto gone = f.t.erase_member(root, "nested");
            BOOST_TEST(gone.has_value() && *gone);
            BOOST_TEST_EQ(global.news(), 0U);
        }
        BOOST_TEST_GT(a.resource().allocations(), taken);
        // "x" keeps the place of its first entry and the value of its last; the key set first is
        // assigned in place.
        const detail::node<Traits>* x = f.t.member(root, "x");
        require(BOOST_TEST(x != nullptr));
        BOOST_TEST_EQ(x->template load<std::int64_t>(), 2);
        const detail::node<Traits>* assigned = f.t.member(root, keys[0]);
        require(BOOST_TEST(assigned != nullptr));
        BOOST_TEST_EQ(assigned->id, first);
        BOOST_TEST_EQ(assigned->template load<std::int64_t>(), 3);
        if constexpr (std::same_as<typename Traits::object, wd::sorted>) {
            // Sorted keys take their places by their bytes, UTF-8 and UTF-16 alike here.
            std::string_view previous;
            for (const detail::node<Traits>* n = f.t.first_child(root); n != nullptr;
                 n = f.t.next_sibling(*n)) {
                BOOST_TEST(previous < n->key.chars());
                previous = n->key.chars();
            }
        } else {
            BOOST_TEST_EQ(f.t.next_sibling(*assigned)->id, x->id);
        }
        const std::uint64_t literal = id_of(f.t, root, "inner");
        BOOST_TEST_EQ(f.t.count(literal), 2U);
        BOOST_TEST(f.t.member(literal, keys[2]) != nullptr);
        BOOST_TEST_EQ(f.t.count(list), 5U);
        BOOST_TEST(f.t.member(root, "nested") == nullptr);
        BOOST_TEST(f.t.member(root, keys[1]) == nullptr);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

// A key given in UTF-8 finds the member stored under it in UTF-16, by code units, among
// characters past U+FFFF and from U+E000 to U+FFFF, which UTF-8 orders otherwise, with no
// allocation; a key between two stored ones, or longer than one, finds nothing.
void lookups_by_utf8_keys() {
    using traits = wd::js_traits;
    const std::array<std::string_view, 6> letters{
        "a",                 // U+0061
        "\xC3\xA9",          // U+00E9
        "\xEE\x80\x80",      // U+E000, which UTF-16 orders after a surrogate pair, UTF-8 before
        "\xEF\xBD\xA1",      // U+FF61
        "\xF0\x9F\x98\x80",  // U+1F600, a surrogate pair
        "\xF4\x8F\xBF\xBF",  // U+10FFFF, the last
    };
    std::vector<std::string> keys;
    for (const std::string_view x : letters) {
        for (const std::string_view y : letters) {
            for (const std::string_view z : letters) {
                keys.push_back(std::string(x) + std::string(y) + std::string(z));
            }
        }
    }
    const std::string longer = keys[8] + "a";
    arena a(4 * mebibyte);
    {
        pmr_fixture<traits> f(a.allocator());
        const std::uint64_t root = detail::root_id;
        require(BOOST_TEST(f.t.assign(root, container<traits>(wd::kind::object)).has_value()));
        // Every other key is stored, so that each one left out falls between two stored ones.
        for (std::size_t i = 0; i < keys.size(); i += 2) {
            require(BOOST_TEST(
                f.t.set_member(root, keys[i], integer<traits>(static_cast<std::int64_t>(i)))
                    .has_value()));
        }
        const global_count global;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const detail::node<traits>* n = f.t.member(root, keys[i]);
            if (i % 2 == 0) {
                BOOST_TEST(n != nullptr &&
                           n->template load<std::int64_t>() == static_cast<std::int64_t>(i));
            } else {
                BOOST_TEST(n == nullptr);
            }
        }
        BOOST_TEST(f.t.member(root, longer) == nullptr);
        BOOST_TEST(f.t.member(root, "\xFF") == nullptr);
        BOOST_TEST_EQ(global.news(), 0U);
    }
    BOOST_TEST_EQ(a.resource().bytes_in_use(), 0U);
}

/** A budget that refuses nothing. */
constexpr std::uint64_t unlimited = std::numeric_limits<std::uint64_t>::max();

/** Whether an erasure erased. */
bool erased(const wd::result<bool>& r) {
    return r.has_value() && *r;
}

// An erasure needs no room: members erased by key and by id, elements erased and cut off, through
// the tree and through the handles, allocate nothing at all, so that a resource that refuses
// every allocation lets them all through. A refusal would end a build without exceptions as
// surely as it fails this one.
template <class Traits>
void erasures_allocate_nothing() {
    const std::uint64_t root = detail::root_id;
    failing_resource failing(std::pmr::new_delete_resource(), unlimited);
    {
        pmr_fixture<Traits> f{pmr_allocator(&failing)};
        const auto [nested, list] = build_sample(f.t);
        const std::uint64_t small = id_of(f.t, root, "small");
        const std::uint64_t keyed = id_of(f.t, root, long_key(0));
        const std::uint64_t element = f.t.element(list, 0)->id;
        auto d = sample<Traits>(pmr_allocator(&failing));
        const std::string key = long_key(0);
        auto items = d.root()["list"];
        failing.allow(0);
        {
            const global_count global;
            BOOST_TEST(erased(f.t.erase_member(root, "nested")));
            BOOST_TEST(!erased(f.t.erase_member(root, "absent")));
            BOOST_TEST(!erased(f.t.erase_member_by_id(root, element)));
            BOOST_TEST(f.t.erase_member_by_id(list, element).error() == wd::errc::type);
            BOOST_TEST(erased(f.t.erase_member_by_id(root, keyed)));
            BOOST_TEST(f.t.erase_elements(list, 1, 2).has_value());
            BOOST_TEST(f.t.splice(list, 0, 1, {}).has_value());
            BOOST_TEST(f.t.set_length(list, 0).has_value());
            BOOST_TEST(erased(f.t.erase_member_by_id(root, list)));
            BOOST_TEST(erased(f.t.erase_member_by_id(root, small)));
            BOOST_TEST(erased(d.root().erase(key)));
            BOOST_TEST(erased(d.root().erase("nested")));
            BOOST_TEST(items.set_length(1).has_value());
            BOOST_TEST_EQ(global.news(), 0U);
        }
        BOOST_TEST_EQ(failing.refusals(), 0U);
        failing.allow(unlimited);
        BOOST_TEST_EQ(f.t.count(root), 0U);
        BOOST_TEST_EQ(f.t.node_count(), 1U);
        BOOST_TEST_EQ(d.root().size(), 2U);
        BOOST_TEST_EQ(d.root()["list"].size(), 1U);
    }
    BOOST_TEST_EQ(failing.bytes_in_use(), 0U);
}

/**
 Builds a tree of the sample's shape and an array of 16 objects, then erases from it, as members,
 elements, a value assigned over a subtree and arrays cut off: the digests of the image, FNV-1a of
 64 bits, before the erasures and after each.
*/
template <class Traits>
std::vector<std::uint64_t> erasure_digests(pmr_fixture<Traits>& f) {
    const std::uint64_t root = detail::root_id;
    const auto [nested, list] = build_sample(f.t);
    const std::uint64_t many =
        take(f.t.set_member(root, "many", container<Traits>(wd::kind::array)));
    for (std::uint64_t i = 0; i < 16; ++i) {
        require(
            BOOST_TEST(f.t.set_element(many, i, container<Traits>(wd::kind::object)).has_value()));
        const std::uint64_t e = f.t.element(many, i)->id;
        const std::uint64_t x = take(f.t.set_member(e, "x", container<Traits>(wd::kind::array)));
        const auto n = static_cast<std::int64_t>(i);
        require(BOOST_TEST(f.t.set_element(x, 0, integer<Traits>(n)).has_value()));
        require(BOOST_TEST(f.t.set_element(x, 1, long_string<Traits>()).has_value()));
        require(BOOST_TEST(f.t.set_member(e, long_key(i), boolean<Traits>(true)).has_value()));
    }
    std::vector<std::uint64_t> digests;
    const auto digest = [&] {
        boost::hash2::fnv1a_64 h;
        h.update(f.image().data(), f.image().size());
        digests.push_back(h.result());
    };
    const std::string key = long_key(0);
    digest();
    require(BOOST_TEST(f.t.erase_member(root, "nested").has_value()));
    digest();
    require(BOOST_TEST(f.t.erase_elements(list, 1, 2).has_value()));
    digest();
    require(BOOST_TEST(f.t.splice(list, 0, 1, {}).has_value()));
    digest();
    require(BOOST_TEST(f.t.set_element(many, 3, integer<Traits>(7)).has_value()));
    digest();
    require(BOOST_TEST(f.t.set_length(many, 12).has_value()));
    digest();
    require(BOOST_TEST(f.t.erase_elements(many, 2, 5).has_value()));
    digest();
    require(BOOST_TEST(f.t.erase_member(root, key).has_value()));
    digest();
    require(BOOST_TEST(f.t.set_length(list, 0).has_value()));
    digest();
    require(BOOST_TEST(f.t.erase_elements(many, 0, 1).has_value()));
    digest();
    require(BOOST_TEST(f.t.erase_member(root, "many").has_value()));
    digest();
    return digests;
}

// Erasures leave every byte of the image as the tree did when it listed each erased subtree in a
// vector, whose digests are pinned here: the order the nodes of a subtree are erased in decides
// the heap's free blocks and the indices' links, so a change of it changes them.
template <class Traits>
void erasures_keep_the_image(const std::array<std::uint64_t, 11>& pinned) {
    pmr_fixture<Traits> f{pmr_allocator(std::pmr::new_delete_resource())};
    const std::vector<std::uint64_t> digests = erasure_digests(f);
    BOOST_TEST_ALL_EQ(digests.begin(), digests.end(), pinned.begin(), pinned.end());
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

/** More allocations than any operation here makes: a run that reaches it never succeeded. */
constexpr std::uint64_t step_limit = 100000;

/**
 Runs `operation` on a copy of `source` in a resource that refuses every allocation past the first
 `budget`, for each budget from 0 until it succeeds: each run that fails must be errc::capacity,
 refused by the resource, with the copy still equal to `source`, and the one that succeeds must
 leave it equal to what an unhindered run makes. `operation` takes the copy and a function that
 starts the budget, which it calls once it has made its handles. Returns the budget it succeeded
 with: how many allocations the operation makes.
*/
template <class Traits, class Operation>
std::uint64_t fail_each_allocation(const wd::pmr::basic_document<Traits>& source,
                                   Operation operation) {
    failing_resource failing(std::pmr::new_delete_resource(), unlimited);
    const pmr_allocator alloc(&failing);
    std::uint64_t budget = 0;
    {
        auto expected = take(wd::from_image<Traits>(source.image(), alloc));
        require(BOOST_TEST(operation(expected, [] {}).has_value()));
        for (; budget < step_limit; ++budget) {
            failing.allow(unlimited);
            auto copy = take(wd::from_image<Traits>(source.image(), alloc));
            const std::uint64_t refused = failing.refusals();
            const auto done = operation(copy, [&failing, budget] { failing.allow(budget); });
            failing.allow(unlimited);
            if (done.has_value()) {
                BOOST_TEST_EQ(failing.refusals(), refused);
                BOOST_TEST(copy == expected);
                break;
            }
            BOOST_TEST(done.error() == wd::errc::capacity);
            BOOST_TEST_GT(failing.refusals(), refused);
            BOOST_TEST(copy == source);
        }
    }
    BOOST_TEST_LT(budget, step_limit);
    BOOST_TEST_EQ(failing.bytes_in_use(), 0U);
    return budget;
}

/**
 As @ref fail_each_allocation, on a copy of the tree of `source`, which `operation` takes alone:
 each run that fails must leave the copy's nodes as they were.
*/
template <class Traits, class Operation>
std::uint64_t fail_each_allocation_in_tree(const pmr_fixture<Traits>& source, Operation operation) {
    failing_resource failing(std::pmr::new_delete_resource(), unlimited);
    const pmr_allocator alloc(&failing);
    std::uint64_t budget = 0;
    {
        pmr_fixture<Traits> expected(source, alloc);
        require(BOOST_TEST(operation(expected.t).has_value()));
        for (; budget < step_limit; ++budget) {
            failing.allow(unlimited);
            pmr_fixture<Traits> copy(source, alloc);
            const std::uint64_t refused = failing.refusals();
            failing.allow(budget);
            const auto done = operation(copy.t);
            failing.allow(unlimited);
            if (done.has_value()) {
                BOOST_TEST_EQ(failing.refusals(), refused);
                BOOST_TEST(same_tree(copy, expected));
                break;
            }
            BOOST_TEST(done.error() == wd::errc::capacity);
            BOOST_TEST_GT(failing.refusals(), refused);
            BOOST_TEST(same_tree(copy, source));
        }
    }
    BOOST_TEST_LT(budget, step_limit);
    BOOST_TEST_EQ(failing.bytes_in_use(), 0U);
    return budget;
}

// Through the handles: a new member at a long key, a value assigned over a subtree, a deep copy
// of the document's own value, a push, a pop and a compaction each fail at every one of their
// allocations with errc::capacity and nothing changed; an erasure has no allocation to fail at.
template <class Traits>
void handles_fail_softly() {
    const auto text = long_string_of<Traits>();
    const std::string key = long_key(5);
    const wd::pmr::basic_document<Traits> source =
        sample<Traits>(pmr_allocator(std::pmr::new_delete_resource()));
    const std::uint64_t set_member = fail_each_allocation(source, [&](auto& d, auto start) {
        auto place = d.root()[key];
        start();
        return place.set(text);
    });
    BOOST_TEST_GT(set_member, 0U);
    const std::uint64_t assign = fail_each_allocation(source, [](auto& d, auto start) {
        auto place = d.root()["nested"];
        start();
        return place.set(std::int64_t{5});
    });
    BOOST_TEST_GT(assign, 0U);
    const std::uint64_t copy = fail_each_allocation(source, [](auto& d, auto start) {
        auto place = d.root()["copy"];
        const auto from = d.root()["nested"];
        start();
        return place.set(from);
    });
    BOOST_TEST_GT(copy, 0U);
    // An erasure allocates nothing: it succeeds with no allocation allowed.
    const std::uint64_t erase = fail_each_allocation(source, [](auto& d, auto start) {
        start();
        return d.root().erase("nested");
    });
    BOOST_TEST_EQ(erase, 0U);
    const std::uint64_t push = fail_each_allocation(source, [&](auto& d, auto start) {
        auto list = d.root()["list"];
        start();
        return list.push(std::int64_t{6}, text);
    });
    BOOST_TEST_GT(push, 0U);
    const std::uint64_t pop = fail_each_allocation(source, [](auto& d, auto start) {
        auto list = d.root()["list"];
        start();
        return list.pop();
    });
    BOOST_TEST_GT(pop, 0U);
    const std::uint64_t compact = fail_each_allocation(source, [](auto& d, auto start) {
        start();
        return d.compact();
    });
    // The image's buffer, then the walk's stack, which the nesting grows several times.
    BOOST_TEST_GT(compact, 3U);
}

// The tree's own operations fail at every one of their allocations with errc::capacity and
// nothing changed: a member at a long key, a literal's members that assign one holding a subtree,
// and a splice that erases subtrees and copies one; erasures, by key, by id and of elements, have
// no allocation to fail at.
template <class Traits>
void tree_fails_softly() {
    using entry = detail::literal_entry<Traits>;
    const std::uint64_t root = detail::root_id;
    pmr_fixture<Traits> source{pmr_allocator(std::pmr::new_delete_resource())};
    const auto [nested, list] = build_sample(source.t);
    const std::array<std::string, 2> keys{long_key(20), long_key(21)};
    const auto inner = std::to_array<entry>({
        {.key = keys[1], .value = long_string<Traits>()},
        {.key = "y", .value = boolean<Traits>(false)},
    });
    const auto entries = std::to_array<entry>({
        {.key = keys[0], .value = integer<Traits>(1)},
        {.key = "nested", .value = long_string<Traits>()},
        {.key = keys[0], .value = container<Traits>(wd::kind::object, inner)},
        {.key = "z", .value = container<Traits>(wd::kind::array, inner)},
        {.key = "small", .value = boolean<Traits>(true)},
    });
    const std::uint64_t set_member = fail_each_allocation_in_tree(
        source, [&](auto& t) { return t.set_member(root, keys[0], long_string<Traits>()); });
    BOOST_TEST_GT(set_member, 0U);
    const std::uint64_t set_members =
        fail_each_allocation_in_tree(source, [&](auto& t) { return t.set_members(root, entries); });
    BOOST_TEST_GT(set_members, set_member);
    const std::uint64_t splice = fail_each_allocation_in_tree(source, [&](auto& t) {
        const std::array<detail::item<Traits>, 2> items{
            copy_of<Traits>(t, nested),
            long_string<Traits>(),
        };
        return t.splice(list, 1, 3, items);
    });
    BOOST_TEST_GT(splice, 0U);
    // An erasure allocates nothing: it succeeds with no allocation allowed.
    const std::uint64_t erase = fail_each_allocation_in_tree(
        source, [&](auto& t) { return t.erase_member(root, "nested"); });
    BOOST_TEST_EQ(erase, 0U);
    const std::uint64_t erase_by_id = fail_each_allocation_in_tree(
        source, [&](auto& t) { return t.erase_member_by_id(root, nested); });
    BOOST_TEST_EQ(erase_by_id, 0U);
    const std::uint64_t erase_elements =
        fail_each_allocation_in_tree(source, [&](auto& t) { return t.erase_elements(list, 1, 3); });
    BOOST_TEST_EQ(erase_elements, 0U);
}

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

}  // namespace

int main() {
    members_in_resource<wd::default_traits>();
    members_in_resource<wd::js_traits>();
    arrays_in_resource<wd::default_traits>();
    arrays_in_resource<wd::js_traits>();
    copies_in_resource<wd::default_traits>();
    copies_in_resource<wd::js_traits>();
    tree_operations_in_resource<wd::default_traits>();
    tree_operations_in_resource<wd::js_traits>();
    members_in_resource<sorted_traits>();
    members_in_resource<insertion_traits>();
    arrays_in_resource<sorted_traits>();
    arrays_in_resource<insertion_traits>();
    copies_in_resource<sorted_traits>();
    copies_in_resource<insertion_traits>();
    tree_operations_in_resource<sorted_traits>();
    tree_operations_in_resource<insertion_traits>();
    lookups_by_utf8_keys();
    erasures_allocate_nothing<wd::default_traits>();
    erasures_allocate_nothing<wd::js_traits>();
    erasures_allocate_nothing<sorted_traits>();
    erasures_allocate_nothing<insertion_traits>();
    erasures_keep_the_image<wd::default_traits>({
        0x38c76b4a395b0326,
        0x2c14970cfa6dc330,
        0x35ea097c472ac20c,
        0xa19a73e9032fd691,
        0x58bf4826b5bdea1c,
        0x1e6bf1349054d6b2,
        0x704f7f300d9124fc,
        0x8ba81cffbbc560cc,
        0x040a767c2a348e42,
        0x74fb654128b4b9dd,
        0xbdec20dae902004d,
    });
    erasures_keep_the_image<wd::js_traits>({
        0x850a39306007c1dc,
        0x961d2dd418e3b7be,
        0x946f2e49edf7876b,
        0x66f5808c528b7a0e,
        0x1963759dd134db47,
        0xbca805ce38c0d5f6,
        0xdbeb1136b36154e9,
        0xc30c77810664054e,
        0xeb9d9db5c1c49417,
        0xcce96cf5f1d6a0e4,
        0xc72fa788f772579e,
    });
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    handles_fail_softly<wd::default_traits>();
    handles_fail_softly<wd::js_traits>();
    tree_fails_softly<wd::default_traits>();
    tree_fails_softly<wd::js_traits>();
    handles_fail_softly<sorted_traits>();
    handles_fail_softly<insertion_traits>();
    tree_fails_softly<sorted_traits>();
    tree_fails_softly<insertion_traits>();
#endif
    return boost::report_errors();
}
