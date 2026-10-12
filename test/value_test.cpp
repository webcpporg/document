// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A document read and written through its handles: navigation that never fails, set that creates
// and replaces, numbers stored by value, strings, dates and bytes, JavaScript's traits, `??`,
// delete and Object.assign, deep copies, handles kept while the image grows and moves, structural
// equality, iteration in the policy's order, a deep copy into external buffers of every size, a
// moved-from document and a document that holds undefined.

#include <webcpp/document/document.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/decimal/decimal128_t.hpp>
#include <boost/decimal/decimal64_t.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
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

using boost::decimal::decimal128_t;
using boost::decimal::decimal64_t;

using js_document = wd::basic_document<wd::js_traits>;

// A decimal from its text, exactly, as a program reads one.
decimal128_t dec(std::string_view text) {
    const auto parsed = wd::number_codec<decimal128_t>::parse(text);
    webcpp::test::require(BOOST_TEST(parsed.has_value()));
    return *parsed;
}

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

// The keys of a value, in UTF-8, in the order the range gives them.
template <class Handle>
std::vector<std::string> keys_of(const Handle& v) {
    std::vector<std::string> out;
    for (const std::string_view k : v.keys()) {
        out.emplace_back(k);
    }
    return out;
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

void a_new_document_holds_null() {
    wd::document doc = make_document();
    BOOST_TEST(doc.root().kind() == wd::kind::null);
    BOOST_TEST(doc.root().is_null());
    BOOST_TEST(doc["x"].is_undefined());
    BOOST_TEST(doc[0].is_undefined());
    BOOST_TEST_EQ(doc.root().size(), 0U);
}

void navigation_never_fails() {
    wd::document doc = document_of(wd::object{{"a", wd::object{{"b", wd::array{1, 2}}}}});
    BOOST_TEST_EQ(doc["a"]["b"][1].get<std::int64_t>().value(), 2);
    BOOST_TEST(doc["a"]["b"][9].is_undefined());
    BOOST_TEST(doc["a"]["b"][1]["x"].is_undefined());
    BOOST_TEST(doc["zz"]["y"]["w"].is_undefined());
    BOOST_TEST(doc["a"][0].is_undefined());
    BOOST_TEST(doc["a"]["b"][1].get<std::string>().error() == wd::errc::type);
    BOOST_TEST(doc["zz"].get<std::int64_t>().error() == wd::errc::type);

    const wd::document& read_only = doc;
    BOOST_TEST_EQ(read_only["a"]["b"][0].get<std::int64_t>().value(), 1);
    BOOST_TEST(read_only["a"]["c"].is_undefined());
    BOOST_TEST(read_only.root()["a"].is_object());
    BOOST_TEST(read_only[std::uint64_t{0}].is_undefined());
}

void set_fills_and_stores_null(wd::document& doc);

void set_creates_and_replaces() {
    wd::document doc = make_document();
    if (!BOOST_TEST(doc.root().set(wd::object()).has_value())) {
        return;
    }
    BOOST_TEST(doc["user"].set(wd::object()).has_value());
    BOOST_TEST(doc["user"]["age"].set(30).has_value());
    BOOST_TEST_EQ(doc["user"]["age"].get<std::int64_t>().value(), 30);
    BOOST_TEST(doc["user"]["age"].set(31).has_value());
    BOOST_TEST_EQ(doc["user"]["age"].get<std::int64_t>().value(), 31);
    BOOST_TEST_EQ(doc["user"].size(), 1U);

    auto cloned = doc.clone();
    if (!BOOST_TEST(cloned.has_value())) {
        return;
    }
    const wd::document before = std::move(*cloned);
    BOOST_TEST(doc["missing"]["x"].set(1).error() == wd::errc::type);
    BOOST_TEST(doc["user"]["age"]["x"].set(1).error() == wd::errc::type);
    BOOST_TEST(doc == before);

    BOOST_TEST(doc["user"]["age"].set(wd::undefined).has_value());
    BOOST_TEST(!doc["user"].contains("age"));
    BOOST_TEST_EQ(doc["user"].size(), 0U);
    // Erasing what is not there changes nothing and succeeds, as `o.x = undefined` does.
    BOOST_TEST(doc["user"]["age"].set(wd::undefined).has_value());
    BOOST_TEST(doc["nobody"]["age"].set(wd::undefined).error() == wd::errc::type);
    set_fills_and_stores_null(doc);
}

// An element past the end fills the elements between with null; undefined stores null; null,
// booleans and strings replace what is there.
void set_fills_and_stores_null(wd::document& doc) {
    BOOST_TEST(doc["arr"].set(wd::array{1, 2}).has_value());
    BOOST_TEST(doc["arr"][4].set(5).has_value());
    BOOST_TEST_EQ(doc["arr"].length().value(), 5U);
    BOOST_TEST(doc["arr"][2].is_null());
    BOOST_TEST(doc["arr"][3].is_null());
    BOOST_TEST_EQ(doc["arr"][4].get<std::int64_t>().value(), 5);

    // An element set to undefined holds null, as `[undefined]` stringifies `[null]`.
    BOOST_TEST(doc["arr"][0].set(wd::undefined).has_value());
    BOOST_TEST(doc["arr"][0].is_null());
    BOOST_TEST(doc["arr"][6].set(wd::undefined).has_value());
    BOOST_TEST_EQ(doc["arr"].length().value(), 7U);
    BOOST_TEST(doc["arr"][6].is_null());

    BOOST_TEST(doc["user"]["nick"].set(nullptr).has_value());
    BOOST_TEST(doc["user"]["nick"].is_null());
    BOOST_TEST(doc["user"]["admin"].set(wd::null).has_value());
    BOOST_TEST(doc["user"]["admin"].is_null());
    BOOST_TEST(doc["user"]["ok"].set(true).has_value());
    BOOST_TEST(doc["user"]["ok"].get<bool>().value());
    BOOST_TEST(doc["user"].set("replaced").has_value());
    BOOST_TEST_EQ(doc["user"].get<std::string_view>().value(), "replaced");
}

void numbers_that_do_not_fit(wd::document& doc);

void numbers_by_value() {
    wd::document doc = document_of(wd::object());
    const std::vector<std::pair<decimal128_t, std::int64_t>> integral{
        {decimal128_t{1}, 1},
        {dec("1.0"), 1},
        {dec("1e0"), 1},
        {dec("1.5e1"), 15},
    };
    for (const auto& [value, expected] : integral) {
        if (!BOOST_TEST(doc["n"].set(value).has_value())) {
            continue;
        }
        BOOST_TEST(doc["n"].is_number());
        BOOST_TEST(doc["n"].is_integer());
        BOOST_TEST_EQ(doc["n"].get<std::int64_t>().value(), expected);
    }
    BOOST_TEST(doc["i"].set(1).has_value());
    BOOST_TEST(doc["i"].is_integer());
    BOOST_TEST_EQ(doc["i"].get<std::int64_t>().value(), 1);

    BOOST_TEST(doc["f"].set(dec("1.50")).has_value());
    BOOST_TEST(doc["f"].is_number());
    BOOST_TEST(!doc["f"].is_integer());
    BOOST_TEST(doc["f"].get<std::int64_t>().error() == wd::errc::range);
    BOOST_TEST(doc["f"].get<decimal128_t>().value() == dec("1.5"));

    // A number that is an integer but does not fit integer is still one, as Number.isInteger says.
    BOOST_TEST(doc["big"].set(dec("1e30")).has_value());
    BOOST_TEST(doc["big"].is_integer());
    BOOST_TEST(doc["big"].get<std::int64_t>().error() == wd::errc::range);
    numbers_that_do_not_fit(doc);
}

// Numbers past what a type holds: read exactly by a type that holds them, refused by one that does
// not; non-finite decimals refused.
void numbers_that_do_not_fit(wd::document& doc) {
    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    BOOST_TEST(doc["u"].set(most).has_value());
    BOOST_TEST(doc["u"].is_number());
    BOOST_TEST_EQ(doc["u"].get<std::uint64_t>().value(), most);
    BOOST_TEST(doc["u"].get<std::int64_t>().error() == wd::errc::range);

    BOOST_TEST(doc["b"].set(300).has_value());
    BOOST_TEST(doc["b"].get<std::int8_t>().error() == wd::errc::range);
    BOOST_TEST_EQ(doc["b"].get<std::int16_t>().value(), 300);

    BOOST_TEST(doc["nan"].set(std::numeric_limits<decimal128_t>::quiet_NaN()).error() ==
               wd::errc::range);
    BOOST_TEST(doc["inf"].set(std::numeric_limits<decimal128_t>::infinity()).error() ==
               wd::errc::range);
    BOOST_TEST(!doc.root().contains("nan"));
    BOOST_TEST(!doc.root().contains("inf"));

    BOOST_TEST(doc["long"].set(dec("1234567890123456789012345678901234")).has_value());
    BOOST_TEST(doc["long"].get<decimal64_t>().error() == wd::errc::range);
    BOOST_TEST(doc["long"].get<decimal128_t>().value() ==
               dec("1234567890123456789012345678901234"));
}

void dates(wd::document& doc);
void bytes(wd::document& doc);

void strings_dates_bytes() {
    wd::document doc = document_of(wd::object());
    BOOST_TEST(doc["s"].set("text").has_value());
    BOOST_TEST(doc["s"].is_string());
    BOOST_TEST_EQ(doc["s"].get<std::string_view>().value(), "text");
    BOOST_TEST_EQ(doc["s"].get<std::string>().value(), "text");

    BOOST_TEST(doc["s"].set(std::string("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80")).has_value());
    BOOST_TEST_EQ(doc["s"].length().value(), 5U);
    BOOST_TEST(doc["s"].set(std::string_view("\xff")).error() == wd::errc::type);
    BOOST_TEST_EQ(doc["s"].length().value(), 5U);
    BOOST_TEST(doc["s"].set(std::u16string_view(u"ab")).has_value());
    BOOST_TEST_EQ(doc["s"].get<std::string_view>().value(), "ab");
    BOOST_TEST(doc["s"].get<std::u16string_view>().error() == wd::errc::type);
    const auto root_length = doc.root().length();
    BOOST_TEST(root_length.error() == wd::errc::type);
    BOOST_TEST(doc["absent"].length().error() == wd::errc::type);
    dates(doc);
    bytes(doc);
}

// A date, in and out of JavaScript's range; a string read as a date when it is toJSON's text.
void dates(wd::document& doc) {
    using std::chrono::milliseconds;
    const wd::date_ms when{milliseconds{1791774245678}};
    BOOST_TEST(doc["d"].set(when).has_value());
    BOOST_TEST(doc["d"].kind() == wd::kind::date);
    BOOST_TEST(doc["d"].is_date());
    BOOST_TEST(doc["d"].get<wd::date_ms>().value() == when);
    BOOST_TEST(doc["d"].set(wd::date_ms{milliseconds{wd::detail::date_limit_ms + 1}}).error() ==
               wd::errc::range);

    BOOST_TEST(doc["t"].set("2026-10-12T03:04:05.678Z").has_value());
    BOOST_TEST(doc["t"].get<wd::date_ms>().value() == when);
    BOOST_TEST(doc["x"].set("x").has_value());
    BOOST_TEST(doc["x"].get<wd::date_ms>().error() == wd::errc::type);
}

// Bytes, stored raw, and a string read as bytes when it is base64.
void bytes(wd::document& doc) {
    const std::array<std::byte, 3> raw{std::byte{'f'}, std::byte{'o'}, std::byte{'o'}};
    BOOST_TEST(doc["b"].set(std::span<const std::byte>(raw)).has_value());
    BOOST_TEST(doc["b"].kind() == wd::kind::bytes);
    BOOST_TEST(doc["b"].is_bytes());
    const auto read = doc["b"].get<std::span<const std::byte>>();
    if (BOOST_TEST(read.has_value())) {
        BOOST_TEST(std::equal(read->begin(), read->end(), raw.begin(), raw.end()));
    }

    BOOST_TEST(doc["z"].set("Zm9v").has_value());
    const auto decoded = doc["z"].get<std::vector<std::byte>>();
    if (BOOST_TEST(decoded.has_value())) {
        BOOST_TEST(std::equal(decoded->begin(), decoded->end(), raw.begin(), raw.end()));
    }
}

void js_traits_strings() {
    auto made = js_document::create();
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    js_document doc = std::move(*made);
    BOOST_TEST(doc.root().set(wd::object()).has_value());
    BOOST_TEST(doc["s"].set("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80").has_value());
    BOOST_TEST_EQ(doc["s"].length().value(), 5U);
    BOOST_TEST(doc["s"].get<std::u16string_view>().value() == u"aé€\U0001F600");
    BOOST_TEST(doc["s"].get<std::string_view>().error() == wd::errc::type);
    BOOST_TEST_EQ(doc["s"].get<std::string>().value(), "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");

    BOOST_TEST(doc["\xC3\xA9t\xC3\xA9"].set(1.5).has_value());
    BOOST_TEST_EQ(doc["\xC3\xA9t\xC3\xA9"].get<double>().value(), 1.5);
    BOOST_TEST(!doc["\xC3\xA9t\xC3\xA9"].is_integer());
    BOOST_TEST(doc["\xC3\xA9t\xC3\xA9"].is_number());

    std::vector<std::u16string> keys;
    for (const auto& [key, v] : doc.root().entries()) {
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(key)>, std::u16string_view>);
        keys.emplace_back(key);
        BOOST_TEST(!v.is_undefined());
    }
    BOOST_TEST(keys == (std::vector<std::u16string>{u"s", u"été"}));
    auto root = doc.root();
    BOOST_TEST(root.contains("\xC3\xA9t\xC3\xA9"));
    BOOST_TEST(root.erase("\xC3\xA9t\xC3\xA9").value());
    BOOST_TEST(!root.contains("\xC3\xA9t\xC3\xA9"));
}

void value_or_is_nullish_coalescing() {
    wd::document doc = document_of(wd::object{{"age", 30}, {"none", nullptr}, {"name", "Ana"}});
    BOOST_TEST_EQ(doc["absent"].value_or(18), 18);
    BOOST_TEST_EQ(doc["none"].value_or(18), 18);
    BOOST_TEST_EQ(doc["age"].value_or(18), 30);
    BOOST_TEST_EQ(doc["name"].value_or(18), 18);
    BOOST_TEST_EQ(doc["name"].value_or(std::string("x")), "Ana");
    BOOST_TEST_EQ(doc["a"]["b"].value_or(std::int64_t{7}), 7);
}

void erase_and_assign_from() {
    wd::document doc = document_of(wd::object{{"age", 30}, {"b", 0}, {"c", 3}});
    wd::value root = doc.root();
    BOOST_TEST(root.erase("age").value());
    BOOST_TEST(!root.erase("age").value());
    BOOST_TEST(doc["b"].erase("x").error() == wd::errc::type);
    BOOST_TEST(doc["absent"].erase("x").error() == wd::errc::type);

    const wd::document source = document_of(wd::object{{"a", 1}, {"b", 2}});
    BOOST_TEST(doc.root().assign_from(source.root()).has_value());
    BOOST_TEST(keys_of(doc.root()) == (std::vector<std::string>{"b", "c", "a"}));
    BOOST_TEST(doc == document_of(wd::object{{"b", 2}, {"c", 3}, {"a", 1}}));

    // An array's elements are its members "0" to "n-1"; null and undefined add nothing.
    const wd::document elements = document_of(wd::array{"x", wd::array{true}});
    BOOST_TEST(doc.root().assign_from(elements.root()).has_value());
    BOOST_TEST(keys_of(doc.root()) == (std::vector<std::string>{"0", "1", "b", "c", "a"}));
    BOOST_TEST(doc["1"][0].get<bool>().value());
    BOOST_TEST(doc.root().assign_from(document_of(nullptr).root()).has_value());
    BOOST_TEST(root.assign_from(source["absent"]).has_value());
    BOOST_TEST_EQ(doc.root().size(), 5U);
    BOOST_TEST(root.assign_from(source["a"]).error() == wd::errc::type);
    BOOST_TEST(doc["b"].assign_from(source.root()).error() == wd::errc::type);

    // A source within the same object is read before anything changes.
    BOOST_TEST(doc["o"].set(wd::object{{"k", 1}}).has_value());
    BOOST_TEST(doc["o"]["k"].assign_from(doc["o"]).error() == wd::errc::type);
    BOOST_TEST(doc["o"].assign_from(doc.root()).has_value());
    BOOST_TEST(keys_of(doc["o"]) == (std::vector<std::string>{"0", "1", "k", "b", "c", "a", "o"}));
    BOOST_TEST(doc["o"]["o"] == document_of(wd::object{{"k", 1}}).root());
}

void deep_copies_never_alias() {
    wd::document other = document_of(wd::object{{"user", wd::object{{"name", "Ana"}}}});
    wd::document doc = document_of(wd::object());
    BOOST_TEST(doc["copy"].set(other["user"]).has_value());
    BOOST_TEST(other["user"]["name"].set("Bia").has_value());
    BOOST_TEST(other["user"]["extra"].set(1).has_value());
    BOOST_TEST_EQ(doc["copy"]["name"].get<std::string_view>().value(), "Ana");
    BOOST_TEST(!doc["copy"].contains("extra"));

    BOOST_TEST(doc["b"].set(wd::array{1, wd::object{{"x", 2}}}).has_value());
    BOOST_TEST(doc["a"].set(doc["b"]).has_value());
    BOOST_TEST(doc["a"] == doc["b"]);
    BOOST_TEST(doc["b"][1]["x"].set(3).has_value());
    BOOST_TEST_EQ(doc["a"][1]["x"].get<std::int64_t>().value(), 2);

    // A document is a copy source too, and so is a value copied into its own descendant.
    BOOST_TEST(doc["whole"].set(other).has_value());
    BOOST_TEST(doc["whole"] == other.root());
    BOOST_TEST(doc["b"][1]["self"].set(doc["b"]).has_value());
    BOOST_TEST_EQ(doc["b"][1]["self"][1]["x"].get<std::int64_t>().value(), 3);
    BOOST_TEST(doc["b"][1]["self"][1]["self"].is_undefined());

    // A copy of what reads undefined erases, as `o.x = undefined` does.
    BOOST_TEST(doc["copy"].set(other["absent"]).has_value());
    BOOST_TEST(!doc.root().contains("copy"));
}

void handle_survives_growth() {
    wd::document doc = document_of(wd::object{{"before", "kept"}});
    auto h = doc["k"];
    auto before = doc["before"];
    if (!BOOST_TEST(h.set(wd::array()).has_value())) {
        return;
    }
    if (!BOOST_TEST(doc["k"][0].set(std::string(100, 'a')).has_value())) {
        return;
    }
    auto first = h[0];
    const std::uint64_t first_size = doc.image().size();
    constexpr std::uint64_t count = 20000;
    std::string text(100, 'x');
    for (std::uint64_t i = 1; i < count; ++i) {
        const std::string number = std::to_string(i);
        text.replace(0, number.size(), number);
        if (!BOOST_TEST(doc["k"][i].set(text).has_value())) {
            return;
        }
    }
    BOOST_TEST_GT(doc.image().size(), 64 * first_size);
    BOOST_TEST_EQ(h.length().value(), count);
    BOOST_TEST(h[count - 1].get<std::string_view>().value() == text);
    BOOST_TEST_EQ(first.get<std::string_view>().value(), std::string(100, 'a'));
    BOOST_TEST_EQ(before.get<std::string_view>().value(), "kept");

    // The handles still write, and grow it again.
    BOOST_TEST(h[count].set(wd::array{"last"}).has_value());
    BOOST_TEST(first.set(wd::object{{"z", std::string(5000, 'z')}}).has_value());
    BOOST_TEST(before.set(nullptr).has_value());
    BOOST_TEST_EQ(h.size(), count + 1);
    BOOST_TEST_EQ(doc["k"][0]["z"].get<std::string>().value(), std::string(5000, 'z'));
    BOOST_TEST_EQ(doc["k"][count][0].get<std::string_view>().value(), "last");
    BOOST_TEST(doc["before"].is_null());

    // A handle reads undefined once its node is erased.
    BOOST_TEST(doc["k"][0].set(nullptr).has_value());
    auto z = doc["k"][1];
    BOOST_TEST(doc["k"].set(1).has_value());
    BOOST_TEST(z.is_undefined());
    BOOST_TEST(z.set(1).error() == wd::errc::type);
}

void equality() {
    BOOST_TEST(document_of(wd::object{{"a", 1}, {"b", wd::array{dec("1.0"), "x"}}}) ==
               document_of(wd::object{{"b", wd::array{1, "x"}}, {"a", 1}}));
    BOOST_TEST(!(document_of(wd::object{{"a", 1}, {"b", wd::array{1, "x"}}}) ==
                 document_of(wd::object{{"a", 1}})));
    BOOST_TEST(!(document_of(wd::array{1, 2}) == document_of(wd::array{2, 1})));
    BOOST_TEST(!(document_of(wd::object{{"a", 1}}) == document_of(wd::object{{"b", 1}})));
    BOOST_TEST(document_of(1) == document_of(dec("1.0")));
    BOOST_TEST(document_of(dec("1e0")) == document_of(1));
    BOOST_TEST(!(document_of(1) == document_of("1")));
    BOOST_TEST(!(document_of(1) == document_of(true)));
    BOOST_TEST(!(document_of(nullptr) == document_of(wd::object())));
    BOOST_TEST(!(document_of(wd::object()) == document_of(wd::array())));

    using std::chrono::milliseconds;
    BOOST_TEST(document_of(wd::date_ms{milliseconds{5}}) ==
               document_of(wd::date_ms{milliseconds{5}}));
    BOOST_TEST(
        !(document_of(wd::date_ms{milliseconds{5}}) == document_of(wd::date_ms{milliseconds{6}})));
    const std::array<std::byte, 2> ab{std::byte{1}, std::byte{2}};
    const std::array<std::byte, 2> ba{std::byte{2}, std::byte{1}};
    BOOST_TEST(document_of(std::span<const std::byte>(ab)) ==
               document_of(std::span<const std::byte>(ab)));
    BOOST_TEST(!(document_of(std::span<const std::byte>(ab)) ==
                 document_of(std::span<const std::byte>(ba))));

    // Two undefined are equal; undefined is not null.
    const wd::document doc = document_of(wd::object{{"n", nullptr}});
    BOOST_TEST(doc["x"] == doc["y"]["z"]);
    BOOST_TEST(!(doc["x"] == doc["n"]));
}

void iteration() {
    wd::document doc = document_of(wd::object());
    BOOST_TEST(doc["b"].set(1).has_value());
    BOOST_TEST(doc["2"].set(2).has_value());
    BOOST_TEST(doc["a"].set(3).has_value());
    BOOST_TEST(doc["1"].set(4).has_value());

    std::vector<std::string> keys;
    std::vector<std::int64_t> values;
    for (const auto& [key, v] : doc.root().entries()) {
        keys.emplace_back(key);
        values.push_back(v.get<std::int64_t>().value());
    }
    BOOST_TEST(keys == (std::vector<std::string>{"1", "2", "b", "a"}));
    BOOST_TEST(values == (std::vector<std::int64_t>{4, 2, 1, 3}));
    BOOST_TEST(keys_of(doc.root()) == keys);
    values.clear();
    for (const auto& v : doc.root().values()) {
        values.push_back(v.get<std::int64_t>().value());
    }
    BOOST_TEST(values == (std::vector<std::int64_t>{4, 2, 1, 3}));
    // An object has no elements to iterate, and a scalar no members.
    BOOST_TEST(doc.root().begin() == doc.root().end());
    BOOST_TEST(doc["b"].keys().begin() == doc["b"].keys().end());
    BOOST_TEST(doc["absent"].entries().begin() == doc["absent"].entries().end());

    const wd::document arr = document_of(wd::array{1, "x", nullptr});
    std::vector<wd::kind> kinds;
    for (const auto& v : arr.root()) {
        kinds.push_back(v.kind());
    }
    BOOST_TEST(kinds ==
               (std::vector<wd::kind>{wd::kind::number, wd::kind::string, wd::kind::null}));
    const wd::const_value arr_root = arr.root();
    BOOST_TEST(arr_root.keys().begin() == arr_root.keys().end());
    static_assert(std::forward_iterator<decltype(arr.root().begin())>);
    static_assert(std::ranges::forward_range<decltype(arr.root().entries())>);
    auto it = arr.root().begin();
    const auto was = it++;
    BOOST_TEST((*was).get<std::int64_t>().value() == 1);
    BOOST_TEST_EQ((*it).get<std::string_view>().value(), "x");
    BOOST_TEST(std::ranges::distance(arr.root().begin(), arr.root().end()) == 3);
}

// Every buffer from the smallest that formats to the first that holds the copy: the copy either
// completes or fails with capacity, leaving the document's value and image as they were.
void copy_in_every_size() {
    wd::document source = document_of(wd::object{{"a", wd::array()}});
    for (std::uint64_t i = 0; i < 100; ++i) {
        const std::string text(i % 37, static_cast<char>('a' + (i % 26)));
        webcpp::test::require(BOOST_TEST(source["a"][i].set(text).has_value()));
    }
    std::size_t failed = 0;
    std::size_t formatted = 0;
    bool copied_once = false;
    for (std::size_t size = 64; size <= std::size_t{256} * 1024; size += 16) {
        const aligned_buffer buffer(size);
        auto made = wd::format(buffer.bytes());
        if (!made) {
            BOOST_TEST(made.error() == wd::errc::capacity);
            BOOST_TEST_EQ(formatted, 0U);
            continue;
        }
        ++formatted;
        auto& doc = *made;
        const std::vector<std::byte> before(doc.image().begin(), doc.image().begin() + 64);
        const auto copied = doc.root().set(source.root());
        if (copied) {
            BOOST_TEST(*doc.clone() == source);
            copied_once = true;
            break;
        }
        ++failed;
        BOOST_TEST(copied.error() == wd::errc::capacity);
        BOOST_TEST(doc.root().is_null());
        // The header, its next id included, is as it was; the heap's free blocks may differ.
        BOOST_TEST(std::ranges::equal(before, doc.image().first(64)));
        // A value that needs no block is stored still.
        auto root = doc.root();
        BOOST_TEST(root.set(true).has_value());
        BOOST_TEST(root.value_or(false));
    }
    BOOST_TEST(copied_once);
    BOOST_TEST_EQ(formatted, failed + 1);
    BOOST_TEST_GT(failed, 100U);
}

void a_moved_from_document() {
    wd::document doc = document_of(wd::object{{"a", 1}});
    wd::document taken = std::move(doc);
    BOOST_TEST_EQ(taken["a"].get<std::int64_t>().value(), 1);
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the state it leaves
    BOOST_TEST(doc.root().is_undefined());
    BOOST_TEST(doc["a"].is_undefined());
    const auto refused = doc.root().set(1);
    BOOST_TEST(refused.error() == wd::errc::type);
    BOOST_TEST(doc["a"].set(1).error() == wd::errc::type);
    BOOST_TEST(doc.image().empty());
    BOOST_TEST(!doc.clone().has_value());
    BOOST_TEST(doc.shrink_to_fit().error() == wd::errc::type);
    BOOST_TEST(doc.root() == taken["absent"]);
    BOOST_TEST(taken["copy"].set(doc).error() == wd::errc::type);
    BOOST_TEST(wd::detail::access::tree_of(doc) == nullptr);

    // Moving into it gives it a value again.
    doc = std::move(taken);
    BOOST_TEST_EQ(doc["a"].get<std::int64_t>().value(), 1);
    BOOST_TEST(taken.root().is_undefined());
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
}

void a_root_holding_undefined() {
    wd::document doc = document_of(wd::object{{"a", 1}});
    wd::value root = doc.root();
    BOOST_TEST(root.set(wd::undefined).has_value());
    BOOST_TEST(doc.root().is_undefined());
    BOOST_TEST(doc.root().kind() == wd::kind::undefined);
    BOOST_TEST(doc["a"].is_undefined());
    BOOST_TEST(doc["a"].set(1).error() == wd::errc::type);
    BOOST_TEST(doc.root().get<std::int64_t>().error() == wd::errc::type);
    BOOST_TEST(root.length().error() == wd::errc::type);
    BOOST_TEST_EQ(doc.root().size(), 0U);

    // Its value copied anywhere is undefined: a member erased, an element null.
    wd::document other = document_of(wd::object{{"x", 1}, {"y", wd::array{1}}});
    BOOST_TEST(other["x"].set(doc).has_value());
    BOOST_TEST(!other.root().contains("x"));
    BOOST_TEST(other["y"][0].set(doc.root()).has_value());
    BOOST_TEST(other["y"][0].is_null());
    wd::document empty = make_document();
    wd::value empty_root = empty.root();
    BOOST_TEST(empty_root.set(doc).has_value());
    BOOST_TEST(empty.root().is_undefined());
    BOOST_TEST(empty == doc);

    BOOST_TEST(root.set(5).has_value());
    BOOST_TEST_EQ(doc.root().get<std::int64_t>().value(), 5);
}

}  // namespace

int main() {
    a_new_document_holds_null();
    navigation_never_fails();
    set_creates_and_replaces();
    numbers_by_value();
    strings_dates_bytes();
    js_traits_strings();
    value_or_is_nullish_coalescing();
    erase_and_assign_from();
    deep_copies_never_alias();
    handle_survives_growth();
    equality();
    iteration();
    copy_in_every_size();
    a_moved_from_document();
    a_root_holding_undefined();
    return boost::report_errors();
}
