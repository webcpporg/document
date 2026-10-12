// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The order of an object's keys through the public handles, with keys that look like array indices
// and one that holds a zero byte: JavaScript's order, with UTF-8 and UTF-16 keys, insertion order
// and sorted order, by UTF-16 code units whatever the encoding; an assignment keeps a key's place,
// and a key erased and created again takes the place its policy gives a new one; `contains` and
// `erase` find exactly the key asked.

#include <webcpp/document/document.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

/** Default traits whose objects keep their keys in insertion order. */
struct insertion_traits : wd::default_traits {
    using object = wd::insertion_order;
};

/** Default traits whose objects sort their keys. */
struct sorted_traits : wd::default_traits {
    using object = wd::sorted;
};

/** JavaScript's traits, UTF-16 keys, whose objects sort their keys. */
struct sorted16_traits : wd::js_traits {
    using object = wd::sorted;
};

using namespace std::string_view_literals;

/** Nine keys, in an order. */
using key_list = std::array<std::string_view, 9>;

// The keys that look like array indices, and one that holds a zero byte, in the order they are set.
constexpr key_list keys_set{
    "x"sv,          "01"sv, "4294967295"sv, "-0"sv, "1e2"sv, ""sv,  // no array index
    "4294967294"sv, "0"sv,  // the largest index, and the smallest
    "\0k"sv,                // a zero byte
};

// JavaScript's order of those keys: the array indices ascending, then the others as created.
constexpr key_list javascript_keys{
    "0"sv, "4294967294"sv,                                                  // the indices
    "x"sv, "01"sv,         "4294967295"sv, "-0"sv, "1e2"sv, ""sv, "\0k"sv,  // the others
};

// The order by code units, which is UTF-8's and UTF-16's alike for these keys.
constexpr key_list sorted_keys{
    ""sv,   "\0k"sv,  // a key before every longer one, a zero byte before every other unit
    "-0"sv, "0"sv,   "01"sv, "1e2"sv, "4294967294"sv, "4294967295"sv, "x"sv,
};

// The keys a test erases and creates again, in that order.
constexpr std::array<std::string_view, 6> keys_recreated{
    "x"sv,   "0"sv,          "4294967295"sv,  // a key, an index, a key that is almost one
    "\0k"sv, "4294967294"sv, ""sv,
};

// A key as UTF-8, from the encoding the traits store.
std::string utf8_of(std::string_view key) {
    return std::string(key);
}

std::string utf8_of(std::u16string_view key) {
    std::string out(static_cast<std::size_t>(wd::detail::utf8_size(key)), '\0');
    wd::detail::to_utf8(key, out.data());
    return out;
}

template <class Traits>
std::vector<std::string> keys_of(const wd::basic_document<Traits>& doc) {
    std::vector<std::string> out;
    for (const auto key : doc.root().keys()) {
        out.push_back(utf8_of(key));
    }
    return out;
}

// The values of the keys in the order the entries give them: each key's place in keys_set.
template <class Traits>
std::vector<std::int64_t> values_of(const wd::basic_document<Traits>& doc) {
    std::vector<std::int64_t> out;
    for (const auto& [key, v] : doc.root().entries()) {
        out.push_back(v.template get<std::int64_t>().value());
    }
    return out;
}

std::int64_t place_of(std::string_view key) {
    for (std::size_t i = 0; i < keys_set.size(); ++i) {
        if (keys_set.at(i) == key) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

template <class Traits>
wd::basic_document<Traits> with_every_key() {
    auto made = wd::basic_document<Traits>::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    wd::basic_document<Traits> doc = std::move(*made);
    webcpp::test::require(BOOST_TEST(doc.root().set(wd::object()).has_value()));
    for (std::size_t i = 0; i < keys_set.size(); ++i) {
        webcpp::test::require(BOOST_TEST(doc[keys_set.at(i)].set(i).has_value()));
    }
    return doc;
}

// Where a key goes when it is erased and created again: last, or back among the indices, or in its
// sorted place, which in every case is the place the policy gives a new key.
template <class Traits>
std::vector<std::string> recreated(std::vector<std::string> order, const std::string& key) {
    if constexpr (std::is_same_v<typename Traits::object, wd::sorted>) {
        return order;
    } else {
        if constexpr (std::is_same_v<typename Traits::object, wd::javascript_order>) {
            if (wd::detail::array_index(key)) {
                return order;
            }
        }
        std::erase(order, key);
        order.push_back(key);
        return order;
    }
}

std::vector<std::string> strings_of(const key_list& keys) {
    return {keys.begin(), keys.end()};
}

// Every key is in the policy's order, with its value.
template <class Traits>
void keys_in_policy_order(const key_list& order) {
    const std::vector<std::string> expected = strings_of(order);
    wd::basic_document<Traits> doc = with_every_key<Traits>();
    BOOST_TEST(keys_of(doc) == expected);
    std::vector<std::int64_t> places;
    places.reserve(expected.size());
    for (const std::string& key : expected) {
        places.push_back(place_of(key));
    }
    BOOST_TEST(values_of(doc) == places);
    BOOST_TEST_EQ(doc.root().size(), keys_set.size());
}

// Every key is found exactly, and nothing that only spells the same number.
template <class Traits>
void keys_found_exactly() {
    wd::basic_document<Traits> doc = with_every_key<Traits>();

    for (const std::string_view key : keys_set) {
        BOOST_TEST(doc.root().contains(key));
        BOOST_TEST_EQ(doc[key].template get<std::int64_t>().value(), place_of(key));
    }
    auto root = doc.root();
    for (const std::string_view other : {"1"sv, "00"sv, "+0"sv, "100"sv, "k"sv, "\0"sv, "1E2"sv}) {
        BOOST_TEST(!root.contains(other));
        BOOST_TEST(doc[other].is_undefined());
        BOOST_TEST(!root.erase(other).value());
    }
}

// An assignment keeps every key's place; a key erased and created again takes the place of a new
// key, by erase and by undefined.
template <class Traits>
void places_kept_and_taken(const key_list& order) {
    const std::vector<std::string> expected = strings_of(order);
    wd::basic_document<Traits> doc = with_every_key<Traits>();
    for (const std::string_view key : keys_set) {
        BOOST_TEST(doc[key].set(wd::array{key}).has_value());
    }
    BOOST_TEST(keys_of(doc) == expected);
    BOOST_TEST(doc["01"][0].template get<std::string>().value() == "01");

    std::vector<std::string> now = expected;
    auto root = doc.root();
    for (const std::string_view key : keys_recreated) {
        BOOST_TEST(root.erase(key).value());
        BOOST_TEST(!root.contains(key));
        BOOST_TEST_EQ(root.size(), keys_set.size() - 1);
        BOOST_TEST(doc[key].set(nullptr).has_value());
        now = recreated<Traits>(now, std::string(key));
        BOOST_TEST(keys_of(doc) == now);
    }
    BOOST_TEST(doc["-0"].set(wd::undefined).has_value());
    BOOST_TEST(!doc.root().contains("-0"));
    BOOST_TEST(doc["-0"].set(1).has_value());
    now = recreated<Traits>(now, "-0");
    BOOST_TEST(keys_of(doc) == now);
}

// A literal object and Object.assign place their keys as set does, a repeated key keeping its first
// place with its last value.
template <class Traits>
void literals_and_assign_in_policy_order(const key_list& order) {
    const std::vector<std::string> expected = strings_of(order);
    auto made = wd::basic_document<Traits>::create();
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    wd::basic_document<Traits> doc = std::move(*made);
    BOOST_TEST(doc.root()
                   .set(wd::object{
                       {"x", 0},
                       {"01", 1},
                       {"4294967295", 2},
                       {"-0", 3},
                       {"1e2", 4},
                       {"", 5},
                       {"4294967294", 6},
                       {"0", 7},
                       {std::string("\0k", 2), 8},
                       {"x", 0},
                   })
                   .has_value());
    BOOST_TEST(keys_of(doc) == expected);
    BOOST_TEST_EQ(doc.root().size(), keys_set.size());

    const wd::basic_document<Traits> every = with_every_key<Traits>();
    BOOST_TEST(doc == every);
    auto target = wd::basic_document<Traits>::create();
    if (!BOOST_TEST(target.has_value())) {
        return;
    }
    BOOST_TEST(target->root().set(wd::object()).has_value());
    BOOST_TEST(target->root().assign_from(every.root()).has_value());
    BOOST_TEST(keys_of(*target) == expected);
    BOOST_TEST(*target == every);
}

// Each check, with the traits and the order of keys they give.
template <class Traits>
void in_policy_order(const key_list& order) {
    keys_in_policy_order<Traits>(order);
    keys_found_exactly<Traits>();
    places_kept_and_taken<Traits>(order);
    literals_and_assign_in_policy_order<Traits>(order);
}

// Sorted keys follow their UTF-16 code units, as JavaScript's sort and RFC 8785 order them, with
// UTF-8 keys too, whose bytes sort as code points: U+1F600, the surrogate pair D83D DE00, comes
// after U+D7FF and before U+E000 and U+FFFF.
template <class Traits>
void sorted_by_utf16_code_units() {
    constexpr std::array<std::string_view, 6> created{
        "\xef\xbf\xbf"sv,       // U+FFFF
        "a\xef\xbf\xbf"sv,      // a, U+FFFF
        "\xf0\x9f\x98\x80"sv,   // U+1F600
        "\xee\x80\x80"sv,       // U+E000
        "a\xf0\x9f\x98\x80"sv,  // a, U+1F600
        "\xed\x9f\xbf"sv,       // U+D7FF
    };
    const std::vector<std::string> expected{
        "a\xf0\x9f\x98\x80", "a\xef\xbf\xbf", "\xed\x9f\xbf",
        "\xf0\x9f\x98\x80",  "\xee\x80\x80",  "\xef\xbf\xbf",
    };
    auto made = wd::basic_document<Traits>::create();
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    wd::basic_document<Traits>& doc = *made;
    BOOST_TEST(doc.root().set(wd::object()).has_value());
    for (std::size_t i = 0; i < created.size(); ++i) {
        BOOST_TEST(doc[created.at(i)].set(i).has_value());
    }
    BOOST_TEST(keys_of(doc) == expected);
    for (const std::string_view key : created) {
        BOOST_TEST(doc.root().contains(key));
    }
    auto root = doc.root();
    BOOST_TEST(!root.contains("\xef\xbf"));
    BOOST_TEST(root.erase("\xf0\x9f\x98\x80").value());
    BOOST_TEST(!root.contains("\xf0\x9f\x98\x80"));
    BOOST_TEST_EQ(root.size(), created.size() - 1);
}

}  // namespace

int main() {
    in_policy_order<wd::default_traits>(javascript_keys);
    in_policy_order<wd::js_traits>(javascript_keys);
    in_policy_order<insertion_traits>(keys_set);
    in_policy_order<sorted_traits>(sorted_keys);
    in_policy_order<sorted16_traits>(sorted_keys);
    sorted_by_utf16_code_units<sorted_traits>();
    sorted_by_utf16_code_units<sorted16_traits>();
    return boost::report_errors();
}
