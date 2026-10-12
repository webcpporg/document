// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The containers' views: as_object and as_array on handles; an object as a sorted map in
// JavaScript's key order, and in key order through by_key with its bounds; an array as a vector;
// the standard's iterator and range concepts, and <algorithm> and <ranges> over both; what a change
// leaves valid, iterators held across 20,000 insertions that grow the image among it; and the
// errors, an insertion with no room changing nothing; and the image's capacity, which reserve grows
// in one step.

#include <webcpp/document/containers.hpp>
#include <webcpp/document/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

using namespace std::string_view_literals;

/** The members' view of key order, of a view of type `Ref`. */
template <class Ref>
using by_key_of = decltype(std::declval<const Ref&>().by_key());

// Each view is a bidirectional, common, sized and borrowed view, and no more: a jump costs
// O(log n) a step.
template <class Range>
constexpr bool bidirectional_view =
    std::ranges::bidirectional_range<Range> && std::ranges::bidirectional_range<const Range> &&
    !std::ranges::random_access_range<Range> && std::ranges::common_range<Range> &&
    std::ranges::sized_range<Range> && std::ranges::view<Range> &&
    std::ranges::borrowed_range<Range> && std::ranges::viewable_range<Range> &&
    std::bidirectional_iterator<std::ranges::iterator_t<Range>> &&
    !std::random_access_iterator<std::ranges::iterator_t<Range>> &&
    std::sentinel_for<std::ranges::sentinel_t<Range>, std::ranges::iterator_t<Range>> &&
    std::bidirectional_iterator<std::reverse_iterator<std::ranges::iterator_t<Range>>>;

static_assert(bidirectional_view<wd::object_ref>);
static_assert(bidirectional_view<wd::const_object_ref>);
static_assert(bidirectional_view<wd::array_ref>);
static_assert(bidirectional_view<wd::const_array_ref>);
static_assert(bidirectional_view<by_key_of<wd::object_ref>>);
static_assert(bidirectional_view<by_key_of<wd::const_object_ref>>);

// An object yields pairs of a key and a handle, an array handles; a read-only view read-only ones.
static_assert(std::same_as<std::ranges::range_reference_t<wd::object_ref>,
                           std::pair<std::string_view, wd::value>>);
static_assert(std::same_as<std::ranges::range_reference_t<wd::const_object_ref>,
                           std::pair<std::string_view, wd::const_value>>);
static_assert(std::same_as<std::ranges::range_reference_t<wd::array_ref>, wd::value>);
static_assert(std::same_as<std::ranges::range_reference_t<wd::const_array_ref>, wd::const_value>);
static_assert(std::same_as<std::ranges::range_reference_t<by_key_of<wd::object_ref>>,
                           std::pair<std::string_view, wd::value>>);

// A handle gives views that write, a const or read-only handle views that read.
static_assert(
    std::same_as<decltype(std::declval<wd::value&>().as_object()), wd::result<wd::object_ref>>);
static_assert(std::same_as<decltype(std::declval<const wd::value&>().as_object()),
                           wd::result<wd::const_object_ref>>);
static_assert(std::same_as<decltype(std::declval<wd::const_value&>().as_array()),
                           wd::result<wd::const_array_ref>>);
static_assert(
    std::same_as<decltype(std::declval<wd::value&>().as_array()), wd::result<wd::array_ref>>);

// A view that reads has no member that writes.
template <class Ref>
concept inserts = requires(const Ref& r) { r.insert("k"sv, 1); };
template <class Ref>
concept pushes = requires(const Ref& r) { r.push_back(1); };
static_assert(inserts<wd::object_ref> && !inserts<wd::const_object_ref>);
static_assert(pushes<wd::array_ref> && !pushes<wd::const_array_ref>);

/** Default traits whose objects sort their keys. */
struct sorted_traits : wd::default_traits {
    using object = wd::sorted;
};

wd::document make_document() {
    auto made = wd::document::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

// A document holding `v`, set at its root.
template <class T>
wd::document document_of(const T& v) {
    wd::document d = make_document();
    wd::value root = d.root();
    webcpp::test::require(BOOST_TEST(root.set(v).has_value()));
    return d;
}

// The view of the object a handle names, which must be one.
template <class Handle>
auto object_of(Handle&& v) {
    auto made = v.as_object();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return *made;
}

// The view of the array a handle names, which must be one.
template <class Handle>
auto array_of(Handle&& v) {
    auto made = v.as_array();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return *made;
}

// The keys of a range of members, in UTF-8, in its order.
template <class Range>
std::vector<std::string> keys_of(Range&& members) {
    std::vector<std::string> out;
    for (const auto& member : members) {
        out.emplace_back(member.first);
    }
    return out;
}

// The integers of a range of handles, in its order.
template <class Range>
std::vector<std::int64_t> ints_of(Range&& handles) {
    std::vector<std::int64_t> out;
    for (const auto& h : handles) {
        out.push_back(h.template get<std::int64_t>().value());
    }
    return out;
}

// The integer a handle reads.
template <class Handle>
std::int64_t int_of(const Handle& h) {
    return h.template get<std::int64_t>().value();
}

// A buffer of `size` bytes at a 16-aligned address, as a browser's memory would be.
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

void views_are_made_from_handles() {
    wd::document doc = document_of(wd::object{{"o", wd::object{{"a", 1}}}, {"a", wd::array{1}}});
    wd::value o = doc["o"];
    wd::value a = doc["a"];
    BOOST_TEST(o.as_object().has_value());
    BOOST_TEST(a.as_array().has_value());
    // Another kind, a missing member and a handle of nothing have no view.
    BOOST_TEST(a.as_object().error() == wd::errc::type);
    BOOST_TEST(o.as_array().error() == wd::errc::type);
    wd::value missing = doc["missing"];
    BOOST_TEST(missing.as_object().error() == wd::errc::type);
    BOOST_TEST(wd::value().as_array().error() == wd::errc::type);
    // A read-only handle and a const document give views that read.
    const wd::document& read_only = doc;
    const wd::const_object_ref r = object_of(read_only["o"]);
    BOOST_TEST_EQ(r.size(), 1U);
    const wd::const_array_ref ra = array_of(read_only["a"]);
    BOOST_TEST_EQ(ra.size(), 1U);
    // A default view is empty and names nothing.
    const wd::object_ref none;
    BOOST_TEST(none.empty());
    BOOST_TEST(none.begin() == none.end());
    BOOST_TEST(none.insert("k", 1).error() == wd::errc::type);
    const wd::array_ref nothing;
    BOOST_TEST(nothing.empty());
    BOOST_TEST(nothing.push_back(1).error() == wd::errc::type);
    BOOST_TEST(nothing.front().is_undefined());
}

void an_object_reads_as_a_map_in_javascript_order() {
    // JSON.parse('{"b":1,"2":2,"a":3,"1":4}') is {"1":4,"2":2,"b":1,"a":3} in node.
    wd::document doc = document_of(wd::object{{"b", 1}, {"2", 2}, {"a", 3}, {"1", 4}});
    const wd::object_ref obj = object_of(doc.root());
    BOOST_TEST_EQ(obj.size(), 4U);
    BOOST_TEST(!obj.empty());
    BOOST_TEST(keys_of(obj) == (std::vector<std::string>{"1", "2", "b", "a"}));
    BOOST_TEST(keys_of(std::ranges::subrange(obj.rbegin(), obj.rend())) ==
               (std::vector<std::string>{"a", "b", "2", "1"}));
    const wd::object_ref::iterator a = obj.find("a");
    if (!BOOST_TEST(a != obj.end())) {
        return;
    }
    BOOST_TEST_EQ(a->first, "a"sv);
    BOOST_TEST_EQ(int_of(a->second), 3);
    const wd::object_ref::iterator absent = obj.find("zz");
    BOOST_TEST(absent == obj.end());
    BOOST_TEST(obj.contains("2"));
    BOOST_TEST(!obj.contains("zz"));
    BOOST_TEST_EQ(obj.count("b"), 1U);
    BOOST_TEST_EQ(obj.count("zz"), 0U);
    // Walking back from the end reaches every member, the first last.
    wd::object_ref::iterator it = obj.end();
    --it;
    BOOST_TEST_EQ((*it).first, "a"sv);
    it--;
    --it;
    --it;
    BOOST_TEST(it == obj.begin());
    // A handle from the iterator writes its member.
    BOOST_TEST(a->second.set(30).has_value());
    BOOST_TEST_EQ(doc.stringify(), R"({"1":4,"2":2,"b":1,"a":30})");
}

void an_object_inserts_and_erases_as_a_map() {
    wd::document doc = document_of(wd::object{{"b", 1}, {"a", 2}});
    const wd::object_ref obj = object_of(doc.root());
    // insert leaves a member that is there as it is.
    const auto kept = obj.insert("a", 9);
    if (!BOOST_TEST(kept.has_value())) {
        return;
    }
    BOOST_TEST(!kept->second);
    BOOST_TEST_EQ(int_of(kept->first->second), 2);
    // A new key goes last, an index key first, as JavaScript places them.
    const auto c = obj.insert("c", 3);
    BOOST_TEST(c.has_value() && c->second && c->first->first == "c"sv);
    const auto zero = obj.insert("0", wd::array{true});
    BOOST_TEST(zero.has_value() && zero->second && zero->first == obj.begin());
    BOOST_TEST_EQ(doc.stringify(), R"({"0":[true],"b":1,"a":2,"c":3})");
    // insert_or_assign assigns where the key is, and inserts last where it is not.
    const auto assigned = obj.insert_or_assign("b", 10);
    BOOST_TEST(assigned.has_value() && !assigned->second);
    const auto added = obj.insert_or_assign("d", 4);
    BOOST_TEST(added.has_value() && added->second);
    BOOST_TEST_EQ(doc.stringify(), R"({"0":[true],"b":10,"a":2,"c":3,"d":4})");
    // try_emplace is insert.
    const auto tried = obj.try_emplace("a", 99);
    BOOST_TEST(tried.has_value() && !tried->second && int_of(doc["a"]) == 2);
    const auto emplaced = obj.try_emplace("e", 5);
    BOOST_TEST(emplaced.has_value() && emplaced->second);
    // A handle is copied deeply.
    const auto copied = obj.insert("f", doc["0"]);
    BOOST_TEST(copied.has_value() && copied->second);
    BOOST_TEST_EQ(doc.stringify(), R"({"0":[true],"b":10,"a":2,"c":3,"d":4,"e":5,"f":[true]})");
    // erase at an iterator gives the member after it.
    const wd::object_ref::iterator after = obj.erase(obj.find("a"));
    BOOST_TEST_EQ(after->first, "c"sv);
    BOOST_TEST_EQ(obj.erase("zz").value(), 0U);
    BOOST_TEST_EQ(obj.erase("0").value(), 1U);
    BOOST_TEST_EQ(doc.stringify(), R"({"b":10,"c":3,"d":4,"e":5,"f":[true]})");
    // A range, its last excluded.
    const wd::object_ref::iterator last = obj.erase(obj.find("c"), obj.find("f"));
    BOOST_TEST(last == obj.find("f"));
    BOOST_TEST_EQ(doc.stringify(), R"({"b":10,"f":[true]})");
    // A key erased and created again goes last.
    BOOST_TEST_EQ(obj.erase("b").value(), 1U);
    BOOST_TEST(obj.insert("b", 1).has_value());
    BOOST_TEST_EQ(doc.stringify(), R"({"f":[true],"b":1})");
    obj.clear();
    BOOST_TEST(obj.empty());
    BOOST_TEST_EQ(doc.stringify(), "{}");
}

void by_key_answers_range_queries() {
    wd::document doc = document_of(wd::object{});
    const wd::object_ref obj = object_of(doc.root());
    for (const std::string_view key : {
             "user_b"sv,
             "admin"sv,
             "user_a"sv,
             "zeta"sv,
             "user_c"sv,
             "user"sv,
             "usera"sv,
             "10"sv,
             "9"sv,
         }) {
        webcpp::test::require(BOOST_TEST(obj.insert(key, key).has_value()));
    }
    // The object keeps JavaScript's order; by_key gives the keys in lexicographic order.
    BOOST_TEST(keys_of(obj) == (std::vector<std::string>{"9", "10", "user_b", "admin", "user_a",
                                                         "zeta", "user_c", "user", "usera"}));
    const auto keys = obj.by_key();
    BOOST_TEST_EQ(keys.size(), 9U);
    BOOST_TEST(keys_of(keys) == (std::vector<std::string>{"10", "9", "admin", "user", "user_a",
                                                          "user_b", "user_c", "usera", "zeta"}));
    BOOST_TEST(keys_of(std::ranges::subrange(keys.rbegin(), keys.rend())) ==
               (std::vector<std::string>{"zeta", "usera", "user_c", "user_b", "user_a", "user",
                                         "admin", "9", "10"}));
    // The keys with the prefix "user_": from it to the first key at or after "user`", '`' being
    // the character after '_'.
    const std::ranges::subrange prefixed(keys.lower_bound("user_"), keys.lower_bound("user`"));
    BOOST_TEST(keys_of(prefixed) == (std::vector<std::string>{"user_a", "user_b", "user_c"}));
    // equal_range holds the key, or nothing.
    const auto [first, last] = keys.equal_range("user_b");
    BOOST_TEST_EQ(std::ranges::distance(first, last), 1);
    BOOST_TEST_EQ(first->first, "user_b"sv);
    const auto [none, also_none] = keys.equal_range("user_");
    BOOST_TEST(none == also_none);
    BOOST_TEST_EQ(none->first, "user_a"sv);
    // Bounds past every key, before every key, and between two.
    BOOST_TEST(keys.lower_bound("zzz") == keys.end());
    BOOST_TEST(keys.upper_bound("zeta") == keys.end());
    BOOST_TEST(keys.lower_bound("") == keys.begin());
    BOOST_TEST_EQ(keys.upper_bound("user")->first, "user_a"sv);
    BOOST_TEST_EQ(keys.find("usera")->first, "usera"sv);
    BOOST_TEST(keys.find("nope") == keys.end());
    // Text that is not UTF-8 bounds nothing.
    BOOST_TEST(keys.lower_bound("\xff") == keys.end());
}

void by_key_reads_utf16_keys_and_the_sorted_policy() {
    auto made = wd::basic_document<wd::js_traits>::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    wd::basic_value<wd::js_traits> root = made->root();
    webcpp::test::require(
        BOOST_TEST(root.set(wd::object{{"b", 1}, {"a\xc3\xa9", 2}, {"a", 3}}).has_value()));
    const auto obj = object_of(root);
    const auto keys = obj.by_key();
    std::vector<std::u16string> seen;
    for (const auto& [key, value] : keys) {
        seen.emplace_back(key);
    }
    BOOST_TEST(seen == (std::vector<std::u16string>{u"a", u"aé", u"b"}));
    BOOST_TEST(keys.lower_bound("a\xc3\xa9")->first == u"aé");
    BOOST_TEST(keys.upper_bound("a\xc3\xa9")->first == u"b");

    // Under sorted, the object's own order is key order, both ways.
    auto sorted_made = wd::basic_document<sorted_traits>::create();
    webcpp::test::require(BOOST_TEST(sorted_made.has_value()));
    wd::basic_value<sorted_traits> sorted_root = sorted_made->root();
    webcpp::test::require(
        BOOST_TEST(sorted_root.set(wd::object{{"b", 1}, {"2", 2}, {"a", 3}}).has_value()));
    const auto sorted_obj = object_of(sorted_root);
    BOOST_TEST(keys_of(sorted_obj) == (std::vector<std::string>{"2", "a", "b"}));
    BOOST_TEST(keys_of(sorted_obj) == keys_of(sorted_obj.by_key()));
    BOOST_TEST(keys_of(std::ranges::subrange(sorted_obj.rbegin(), sorted_obj.rend())) ==
               (std::vector<std::string>{"b", "a", "2"}));
}

void an_array_reads_and_writes_as_a_vector() {
    wd::document doc = document_of(wd::object{{"a", wd::array{1, 2, 3}}});
    const wd::array_ref arr = array_of(doc["a"]);
    BOOST_TEST_EQ(arr.size(), 3U);
    BOOST_TEST_EQ(int_of(arr.front()), 1);
    BOOST_TEST_EQ(int_of(arr.back()), 3);
    BOOST_TEST_EQ(int_of(arr[1]), 2);
    BOOST_TEST_EQ(int_of(arr.at(2).value()), 3);
    BOOST_TEST(arr.at(3).error() == wd::errc::range);
    // operator[] past the end is a missing element, as a handle's is: set creates it.
    BOOST_TEST(arr[3].is_undefined());
    BOOST_TEST(ints_of(arr) == (std::vector<std::int64_t>{1, 2, 3}));
    BOOST_TEST(ints_of(std::ranges::subrange(arr.rbegin(), arr.rend())) ==
               (std::vector<std::int64_t>{3, 2, 1}));

    BOOST_TEST(arr.push_back(4).has_value());
    const auto five = arr.emplace_back(5);
    BOOST_TEST(five.has_value() && int_of(*five) == 5);
    BOOST_TEST_EQ(doc.stringify(), R"({"a":[1,2,3,4,5]})");
    arr.pop_back();
    BOOST_TEST_EQ(doc.stringify(), R"({"a":[1,2,3,4]})");
    // insert before an iterator, at the front, and at the end.
    const auto inserted = arr.insert(std::ranges::next(arr.begin(), 2), "x");
    if (!BOOST_TEST(inserted.has_value())) {
        return;
    }
    BOOST_TEST_EQ((*inserted)->get<std::string_view>().value(), "x"sv);
    BOOST_TEST(arr.insert(arr.begin(), 0).has_value());
    BOOST_TEST(arr.insert(arr.end(), wd::object{{"k", nullptr}}).has_value());
    BOOST_TEST_EQ(doc.stringify(), R"({"a":[0,1,2,"x",3,4,{"k":null}]})");
    // erase at an iterator gives the element after it; a range gives its last.
    const wd::array_ref::iterator after = arr.erase(*inserted);
    BOOST_TEST_EQ(int_of(*after), 3);
    const wd::array_ref::iterator last = arr.erase(arr.begin(), std::ranges::next(arr.begin(), 2));
    BOOST_TEST(last == arr.begin());
    BOOST_TEST_EQ(doc.stringify(), R"({"a":[2,3,4,{"k":null}]})");
    // A handle is copied deeply, and one that reads undefined is null, as [undefined] is written.
    BOOST_TEST(arr.push_back(doc["a"]).has_value());
    BOOST_TEST(arr.push_back(doc["missing"]).has_value());
    BOOST_TEST_EQ(doc.stringify(), R"({"a":[2,3,4,{"k":null},[2,3,4,{"k":null}],null]})");
    // resize truncates or appends null, as a.length = n.
    BOOST_TEST(arr.resize(2).has_value());
    BOOST_TEST(arr.resize(4).has_value());
    BOOST_TEST_EQ(doc.stringify(), R"({"a":[2,3,null,null]})");
    arr.clear();
    BOOST_TEST(arr.empty());
    BOOST_TEST(arr.front().is_undefined());
    BOOST_TEST(arr.back().is_undefined());
    arr.pop_back();
    BOOST_TEST_EQ(doc.stringify(), R"({"a":[]})");
}

void algorithms_and_ranges_work_on_the_views() {
    wd::document doc = document_of(
        wd::object{{"n", wd::object{{"b", 1}, {"a", 2}, {"c", 3}}}, {"l", wd::array{5, 1, 4, 2}}});
    const wd::object_ref obj = object_of(doc["n"]);
    const wd::array_ref arr = array_of(doc["l"]);

    const auto c = std::ranges::find_if(obj, [](const auto& m) { return m.first == "c"sv; });
    BOOST_TEST(c != obj.end() && int_of(c->second) == 3);
    BOOST_TEST_EQ(std::ranges::count_if(obj, [](const auto& m) { return int_of(m.second) > 1; }),
                  2);
    std::vector<std::string> big;
    for (const std::string_view key :
         obj | std::views::filter([](const auto& m) { return int_of(m.second) >= 2; }) |
             std::views::transform([](const auto& m) { return m.first; })) {
        big.emplace_back(key);
    }
    BOOST_TEST(big == (std::vector<std::string>{"a", "c"}));
    BOOST_TEST(keys_of(std::views::reverse(obj)) == (std::vector<std::string>{"c", "a", "b"}));
    BOOST_TEST_EQ(std::ranges::distance(obj), 3);

    const auto four = std::ranges::find_if(arr, [](const wd::value& v) { return int_of(v) == 4; });
    BOOST_TEST_EQ(std::ranges::distance(arr.begin(), four), 2);
    BOOST_TEST_EQ(std::ranges::count_if(arr, [](const wd::value& v) { return int_of(v) % 2 == 1; }),
                  2);
    std::vector<std::int64_t> doubled;
    for (const std::int64_t x :
         arr | std::views::transform([](const wd::value& v) { return 2 * int_of(v); })) {
        doubled.push_back(x);
    }
    BOOST_TEST(doubled == (std::vector<std::int64_t>{10, 2, 8, 4}));
    BOOST_TEST(ints_of(arr | std::views::reverse) == (std::vector<std::int64_t>{2, 4, 1, 5}));
    const auto max = std::ranges::max_element(
        arr, [](const wd::value& x, const wd::value& y) { return int_of(x) < int_of(y); });
    BOOST_TEST_EQ(int_of(*max), 5);

    // A view is a borrowed range: an algorithm over a temporary view gives a usable iterator.
    const auto one =
        std::ranges::find_if(array_of(doc["l"]), [](const wd::value& v) { return int_of(v) == 1; });
    static_assert(!std::same_as<decltype(one), const std::ranges::dangling>);
    BOOST_TEST_EQ(int_of(*one), 1);
}

void iterators_survive_insertions_that_grow_the_image() {
    wd::document doc = document_of(
        wd::object{{"o", wd::object{{"first", 1}, {"second", 2}}}, {"a", wd::array{10, 20, 30}}});
    const wd::object_ref obj = object_of(doc["o"]);
    const wd::array_ref arr = array_of(doc["a"]);
    const wd::object_ref::iterator first = obj.begin();
    const wd::object_ref::iterator second = obj.find("second");
    const wd::object_ref::iterator object_end = obj.end();
    const wd::array_ref::iterator twenty = std::ranges::next(arr.begin());
    const wd::value thirty = arr[2];
    const std::uint64_t before = doc.image().size();
    constexpr std::int64_t insertions = 20'000;
    for (std::int64_t i = 0; i < insertions; ++i) {
        webcpp::test::require(BOOST_TEST(obj.insert("k" + std::to_string(i), i).has_value()));
        webcpp::test::require(BOOST_TEST(arr.push_back(i).has_value()));
    }
    BOOST_TEST_GT(doc.image().size(), 4 * before);
    BOOST_TEST_EQ(first->first, "first"sv);
    BOOST_TEST_EQ(int_of(second->second), 2);
    BOOST_TEST(object_end == obj.end());
    wd::object_ref::iterator last = object_end;
    --last;
    BOOST_TEST_EQ(last->first, "k19999"sv);
    BOOST_TEST_EQ(int_of(*twenty), 20);
    BOOST_TEST_EQ(int_of(thirty), 30);
    BOOST_TEST_EQ(obj.size(), 2U + insertions);
    BOOST_TEST_EQ(arr.size(), 3U + insertions);
}

void positions_follow_their_elements() {
    wd::document doc = document_of(wd::array{10, 20, 30, 40});
    const wd::array_ref arr = array_of(doc.root());
    const wd::array_ref::iterator twenty = std::ranges::next(arr.begin());
    const wd::array_ref::iterator thirty = std::ranges::next(arr.begin(), 2);
    const wd::array_ref::iterator forty = std::ranges::next(arr.begin(), 3);
    const wd::value thirty_handle = arr[2];
    // An insertion before them moves them one index up, each still at its element.
    BOOST_TEST(arr.insert(arr.begin(), 0).has_value());
    BOOST_TEST_EQ(std::ranges::distance(arr.begin(), twenty), 2);
    BOOST_TEST_EQ(int_of(*twenty), 20);
    BOOST_TEST_EQ(int_of(*thirty), 30);
    BOOST_TEST_EQ(int_of(thirty_handle), 30);
    // Erasing an element invalidates only the iterators to it: the others follow theirs down.
    BOOST_TEST(arr.erase(twenty) == thirty);
    BOOST_TEST_EQ(std::ranges::distance(arr.begin(), thirty), 2);
    BOOST_TEST_EQ(int_of(*thirty), 30);
    BOOST_TEST_EQ(int_of(*forty), 40);
    BOOST_TEST_EQ(int_of(thirty_handle), 30);
    BOOST_TEST((*twenty).is_undefined());
    BOOST_TEST_EQ(doc.stringify(), "[0,10,30,40]");

    // The same for an object's members.
    wd::document o = document_of(wd::object{{"a", 1}, {"b", 2}, {"c", 3}});
    const wd::object_ref obj = object_of(o.root());
    const wd::object_ref::iterator b = obj.find("b");
    const wd::object_ref::iterator c = obj.find("c");
    const wd::value b_handle = o["b"];
    BOOST_TEST(obj.insert("0", 0).has_value());
    BOOST_TEST_EQ(b->first, "b"sv);
    BOOST_TEST(obj.erase(b) == c);
    BOOST_TEST_EQ(int_of(c->second), 3);
    BOOST_TEST(b_handle.is_undefined());
    BOOST_TEST((*b).second.is_undefined());
    BOOST_TEST(std::ranges::next(obj.begin(), 2) == c);
}

void errors_change_nothing() {
    wd::document doc =
        document_of(wd::object{{"o", wd::object{}}, {"a", wd::array{}}, {"b", wd::array{}}});
    wd::value o = doc["o"];
    const wd::object_ref obj = object_of(o);
    const wd::array_ref arr = array_of(doc["a"]);
    // A key that is not UTF-8, and a source that reads undefined, which no member holds.
    BOOST_TEST(obj.insert("\xff", 1).error() == wd::errc::type);
    BOOST_TEST(obj.insert_or_assign("k", doc["missing"]).error() == wd::errc::type);
    BOOST_TEST(obj.empty());
    // An iterator of another container, and one at an element erased.
    const wd::array_ref other = array_of(doc["b"]);
    BOOST_TEST(arr.insert(other.end(), 1).error() == wd::errc::type);
    BOOST_TEST(other.push_back(1).has_value());
    const wd::array_ref::iterator erased = other.begin();
    other.pop_back();
    BOOST_TEST(other.insert(erased, 2).error() == wd::errc::type);
    BOOST_TEST(other.erase(erased) == other.end());
    // A view whose value became another kind is empty, and every insertion fails.
    BOOST_TEST(o.set(wd::array{1}).has_value());
    BOOST_TEST(obj.empty());
    BOOST_TEST(obj.begin() == obj.end());
    const wd::object_ref::iterator gone = obj.find("k");
    BOOST_TEST(gone == obj.end());
    BOOST_TEST(obj.insert("k", 1).error() == wd::errc::type);
    BOOST_TEST(obj.erase("k").error() == wd::errc::type);
    BOOST_TEST_EQ(doc.stringify(), R"({"o":[1],"a":[],"b":[]})");
}

// Inserts with `insert(i)`, for i from 0, until it fails, which must be for room and leave the
// document's value and the image's header, which holds its size, as they were: the heap's probe may
// leave its own bookkeeping in free memory. Whether it failed within 10,000 insertions.
template <class Insert>
bool fills_up(const wd::basic_document<wd::default_traits, wd::external>& doc, Insert insert) {
    for (std::int64_t i = 0; i < 10'000; ++i) {
        const std::string text = doc.stringify();
        const std::vector<std::byte> header(doc.image().begin(), doc.image().begin() + 64);
        const auto inserted = insert(i);
        if (!inserted.has_value()) {
            BOOST_TEST(inserted.error() == wd::errc::capacity);
            BOOST_TEST_EQ(doc.stringify(), text);
            BOOST_TEST(std::ranges::equal(doc.image().first(64), header));
            return true;
        }
    }
    return false;
}

void no_room_changes_nothing() {
    constexpr std::size_t size = std::size_t{16} * 1024;
    const aligned_buffer for_object(size);
    auto object_doc = wd::format(for_object.bytes());
    webcpp::test::require(BOOST_TEST(object_doc.has_value()));
    auto object_root = object_doc->root();
    webcpp::test::require(BOOST_TEST(object_root.set(wd::object{}).has_value()));
    const auto obj = object_of(object_root);
    BOOST_TEST(fills_up(*object_doc, [&obj](std::int64_t i) {
        return obj.insert("member" + std::to_string(i), "a long enough value");
    }));

    const aligned_buffer for_array(size);
    auto array_doc = wd::format(for_array.bytes());
    webcpp::test::require(BOOST_TEST(array_doc.has_value()));
    auto array_root = array_doc->root();
    webcpp::test::require(BOOST_TEST(array_root.set(wd::array{}).has_value()));
    const auto arr = array_of(array_root);
    BOOST_TEST(fills_up(*array_doc, [&arr](std::int64_t /*i*/) {
        return arr.insert(arr.begin(), "a long enough value");
    }));

    // Erasing needs no room.
    const auto members = obj.size();
    obj.erase(obj.begin());
    arr.clear();
    BOOST_TEST_EQ(obj.size(), members - 1);
    BOOST_TEST(arr.empty());
}

void reserve_grows_the_image_once() {
    wd::document doc = document_of(wd::object{});
    const std::uint64_t first = doc.capacity();
    BOOST_TEST_EQ(first, doc.image().size());
    // A reservation within the capacity changes nothing, a seal included.
    BOOST_TEST(doc.seal().has_value());
    BOOST_TEST(doc.reserve(first).has_value());
    BOOST_TEST(doc.reserve(0).has_value());
    BOOST_TEST_EQ(doc.capacity(), first);
    BOOST_TEST(doc.sealed());
    // One beyond it grows the image at once, which is then a change: the seal is cleared.
    constexpr std::uint64_t reserved = std::uint64_t{4} * 1024 * 1024;
    const wd::object_ref obj = object_of(doc.root());
    BOOST_TEST(obj.insert("first", 1).has_value());
    const wd::object_ref::iterator held = obj.find("first");
    BOOST_TEST(doc.reserve(reserved).has_value());
    BOOST_TEST_GE(doc.capacity(), reserved);
    BOOST_TEST_EQ(doc.capacity(), doc.image().size());
    BOOST_TEST(!doc.sealed());
    BOOST_TEST_EQ(held->first, "first"sv);
    // Insertions that fit move the image no more.
    const std::byte* const address = doc.image().data();
    const std::uint64_t capacity = doc.capacity();
    for (std::int64_t i = 0; i < 1'000; ++i) {
        webcpp::test::require(BOOST_TEST(obj.insert("k" + std::to_string(i), i).has_value()));
    }
    BOOST_TEST(doc.image().data() == address);
    BOOST_TEST_EQ(doc.capacity(), capacity);
    BOOST_TEST_EQ(int_of(held->second), 1);
}

void reserve_fails_past_the_limit_and_changes_nothing() {
    constexpr std::uint64_t limit = std::uint64_t{64} * 1024;
    auto made = wd::document::create(limit);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    wd::document& doc = *made;
    wd::value root = doc.root();
    webcpp::test::require(BOOST_TEST(root.set(wd::array{1, 2}).has_value()));
    const std::uint64_t capacity = doc.capacity();
    BOOST_TEST(doc.reserve(limit + 1).error() == wd::errc::capacity);
    BOOST_TEST_EQ(doc.capacity(), capacity);
    BOOST_TEST_EQ(doc.stringify(), "[1,2]");
    BOOST_TEST(doc.reserve(limit).has_value());
    BOOST_TEST_EQ(doc.capacity(), limit);

    // An external image is as large as its buffer, and grows no more.
    constexpr std::size_t external = std::size_t{16} * 1024;
    const aligned_buffer buffer(external);
    auto formatted = wd::format(buffer.bytes());
    webcpp::test::require(BOOST_TEST(formatted.has_value()));
    BOOST_TEST_EQ(formatted->capacity(), external);
    BOOST_TEST(formatted->reserve(external).has_value());
    BOOST_TEST(formatted->reserve(external + 1).error() == wd::errc::capacity);
    BOOST_TEST_EQ(formatted->capacity(), external);

    // A moved-from document has no image.
    const wd::document moved = std::move(doc);
    BOOST_TEST_EQ(moved.capacity(), limit);
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the state it leaves
    BOOST_TEST_EQ(doc.capacity(), 0U);
    BOOST_TEST(doc.reserve(1).error() == wd::errc::type);
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
}

}  // namespace

int main() {
    views_are_made_from_handles();
    an_object_reads_as_a_map_in_javascript_order();
    an_object_inserts_and_erases_as_a_map();
    by_key_answers_range_queries();
    by_key_reads_utf16_keys_and_the_sorted_policy();
    an_array_reads_and_writes_as_a_vector();
    algorithms_and_ranges_work_on_the_views();
    iterators_survive_insertions_that_grow_the_image();
    positions_follow_their_elements();
    errors_change_nothing();
    no_room_changes_nothing();
    reserve_grows_the_image_once();
    reserve_fails_past_the_limit_and_changes_nothing();
    return boost::report_errors();
}
