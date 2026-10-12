// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// parse reads JSON text as JSON.parse does: what it accepts, written back as Node 26 writes it;
// what it refuses, as malformed text with the offset where it was found, or as a number past the
// traits; the limits of depth, bytes and image, exact, and the default image limit of 64 MiB; an
// object of 100,000 keys and a 64 MB string; the revivers of dates and bytes; a parse into external
// buffers of every size. Built with WEBCPP_TEST_DOCUMENT_SUGAR, and so with exceptions, it also
// checks parse_or_throw.

#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/json/error.hpp>

#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

namespace {

// What parse reads from `text`, as stringify writes it, or "error " and the error's message.
template <class Traits = wd::default_traits>
std::string reparse(std::string_view text, const wd::parse_options& o = {}) {
    const auto parsed = wd::parse<Traits>(text, o);
    if (!parsed.has_value()) {
        return "error " + parsed.error().message();
    }
    return wd::stringify(*parsed);
}

// The error parse returns for `text`, or no error.
template <class Traits = wd::default_traits>
boost::system::error_code refusal(std::string_view text, const wd::parse_options& o = {}) {
    const auto parsed = wd::parse<Traits>(text, o);
    return parsed.has_value() ? boost::system::error_code{} : parsed.error();
}

wd::document parsed(std::string_view text, const wd::parse_options& o = {}) {
    auto made = wd::parse(text, o);
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
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

// A text and what JSON.stringify writes of it.
struct row {
    constexpr row(std::string_view t, std::string_view w) noexcept : text(t), written(w) {}

    std::string_view text;
    std::string_view written;
};

// The rows JSON.parse accepts, each written as JSON.stringify(JSON.parse(text)) writes it in Node
// 26; the same under js_traits but where a double rounds.
void accepted_as_json_parse_accepts() {
    constexpr std::array rows{
        row{" [1 , 2] ", "[1,2]"},
        row{R"({"b":1,"2":2,"a":3,"1":4})", R"({"1":4,"2":2,"b":1,"a":3})"},
        row{R"({"a":1,"b":2,"a":3})", R"({"a":3,"b":2})"},
        row{R"({"a":[1],"a":{"x":1}})", R"({"a":{"x":1}})"},
        row{R"("é😀")", "\"\xc3\xa9\xf0\x9f\x98\x80\""},
        row{"-0", "0"},
        row{"1.0", "1"},
        row{R"(" ")", "\"\xe2\x80\xa8\""},
        row{" \t\n\r1 \t\n\r", "1"},
        row{R"(["\"\\\/\b\f\n\r\t\u0001\u001f"])", R"(["\"\\/\b\f\n\r\t\u0001\u001f"])"},
        row{R"({"a\u0000b":"\u0000"})", R"({"a\u0000b":"\u0000"})"},
        row{R"({"":{"":[]},"x":{}})", R"({"":{"":[]},"x":{}})"},
        row{R"({"01":1,"-0":2,"1e2":3,"4294967295":4,"4294967294":5,"":6,"0":7})",
            R"({"0":7,"4294967294":5,"01":1,"-0":2,"1e2":3,"4294967295":4,"":6})"},
    };
    for (const row& r : rows) {
        BOOST_TEST_EQ(reparse(r.text), r.written);
        BOOST_TEST_EQ(reparse<wd::js_traits>(r.text), r.written);
    }
    BOOST_TEST(parsed("1.0").root().is_integer());
    BOOST_TEST(parsed("1.5").root().is_number());
    BOOST_TEST(!parsed("1.5").root().is_integer());
}

// Numbers at the edges of their text, stored by value and written as JavaScript writes them, with
// the default traits; JavaScript's doubles with js_traits.
void numbers_at_their_edges() {
    constexpr std::array rows{
        row{"-0", "0"},
        row{"0e0", "0"},
        row{"1E400", "1e+400"},
        row{"1e-400", "1e-400"},
        row{"0.0000001", "1e-7"},
        row{"1e21", "1e+21"},
        row{"1.000000000000000000000000000000000000000", "1"},
        row{"9223372036854775807", "9223372036854775807"},
        row{"9223372036854775808", "9223372036854775808"},
        row{"-9223372036854775809", "-9223372036854775809"},
        row{"[1.5e300,-2.5e-300,12.5e3]", "[1.5e+300,-2.5e-300,12500]"},
    };
    for (const row& r : rows) {
        BOOST_TEST_EQ(reparse(r.text), r.written);
    }
    constexpr std::array js_rows{
        row{"9223372036854775808", "9223372036854776000"},
        row{"123456789012345678", "123456789012345680"},
        row{"1e-400", "0"},
        row{"0.1", "0.1"},
    };
    for (const row& r : js_rows) {
        BOOST_TEST_EQ(reparse<wd::js_traits>(r.text), r.written);
    }
    // JSON.parse reads 1e400 as Infinity, which JSON.stringify writes null, and js_traits does too.
    BOOST_TEST_EQ(reparse<wd::js_traits>("[1e400,-1e400,1e999]"), "[null,null,null]");
    const auto infinite = wd::parse<wd::js_traits>("[1e400,-1e400]");
    if (BOOST_TEST(infinite.has_value())) {
        BOOST_TEST((*infinite)[0].is_number());
        BOOST_TEST((*infinite)[0].get<double>() == std::numeric_limits<double>::infinity());
        BOOST_TEST((*infinite)[1].get<double>() == -std::numeric_limits<double>::infinity());
    }
    BOOST_TEST(parsed("9223372036854775807").root().is_integer());
    BOOST_TEST_EQ(parsed("-9223372036854775808").root().get<std::int64_t>().value(), INT64_MIN);
}

// Every text JSON.parse refuses is errc::syntax, a lone surrogate included: the documented limit.
void refused_as_malformed() {
    constexpr std::array<std::string_view, 21> texts{
        "",
        "   ",
        "[1,]",
        R"({"a":1,})",
        "// c\n1",
        "/* c */ 1",
        "[1] 2",
        "01",
        "+1",
        ".5",
        "1.",
        "NaN",
        "'a'",
        R"("\ud800")",
        R"("\udc00")",
        R"("\x41")",
        "\"a\nb\"",
        "\xff",
        "[\"\xc0\xaf\"]",
        "\f1",
        "[1",
    };
    for (const std::string_view text : texts) {
        BOOST_TEST(refusal(text) == wd::errc::syntax);
        BOOST_TEST(refusal<wd::js_traits>(text) == wd::errc::syntax);
    }
}

// A syntax error's offset is where it was found, and its cause Boost.JSON's code.
void a_syntax_error_has_its_offset() {
    wd::parser p;
    BOOST_TEST(!p.cause());
    BOOST_TEST(p.write("[1,]").error() == wd::errc::syntax);
    BOOST_TEST_EQ(p.offset(), 3U);
    BOOST_TEST(p.cause());
    BOOST_TEST(p.cause() != wd::errc::syntax);

    wd::parser q;
    BOOST_TEST(q.write("[1,2").has_value());
    BOOST_TEST_EQ(q.offset(), 4U);
    BOOST_TEST(q.finish().error() == wd::errc::syntax);
    BOOST_TEST_EQ(q.offset(), 4U);
    BOOST_TEST(q.cause());

    // A second text after the first is extra data, where it begins.
    wd::parser r;
    BOOST_TEST(r.write("1 2").error() == wd::errc::syntax);
    BOOST_TEST_EQ(r.offset(), 2U);
    BOOST_TEST(r.cause() == boost::json::error::extra_data);
    BOOST_TEST(refusal("1 2") == wd::errc::syntax);
    BOOST_TEST(refusal<wd::js_traits>(R"({"a":1}{"b":2})") == wd::errc::syntax);
}

// A number past the default traits is errc::range, where JavaScript rounds or overflows; with
// js_traits it is the double JavaScript reads, Infinity past a double's range.
void numbers_past_the_traits() {
    BOOST_TEST(refusal("1e999999999999") == wd::errc::range);
    BOOST_TEST(refusal("12345678901234567890123456789012345") == wd::errc::range);
    BOOST_TEST(refusal("[1,123456789012345678901234567890123456789]") == wd::errc::range);
    BOOST_TEST_EQ(reparse<wd::js_traits>("12345678901234567890123456789012345"),
                  "1.234567890123457e+34");
    BOOST_TEST_EQ(reparse<wd::js_traits>("[1e999999999999,-1e999999999999]"), "[null,null]");
    BOOST_TEST_EQ(reparse("1234567890123456789012345678901234"),
                  "1.234567890123456789012345678901234e+33");
}

std::string nested(std::size_t depth) {
    return std::string(depth, '[') + "1" + std::string(depth, ']');
}

// Depth, bytes and the image: each limit is exact, and past it is errc::capacity.
void limits() {
    wd::parse_options four;
    four.max_depth = 4;
    BOOST_TEST_EQ(reparse("[[[[1]]]]", four), "[[[[1]]]]");
    BOOST_TEST(refusal("[[[[[1]]]]]", four) == wd::errc::capacity);
    BOOST_TEST(refusal(R"({"a":{"b":{"c":{"d":{}}}}})", four) == wd::errc::capacity);
    BOOST_TEST(refusal(nested(256)).failed() == false);
    BOOST_TEST(refusal(nested(257)) == wd::errc::capacity);

    wd::parse_options ten;
    ten.max_bytes = 10;
    BOOST_TEST_EQ(reparse("[1,2,3,45]", ten), "[1,2,3,45]");
    BOOST_TEST(refusal("[1,2,3,456]", ten) == wd::errc::capacity);

    wd::parse_options small;
    small.max_document_bytes = 8192;
    std::string big = "[";
    for (std::size_t i = 0; i < 1000; ++i) {
        big += i == 0 ? "\"0123456789\"" : ",\"0123456789\"";
    }
    big += ']';
    BOOST_TEST(refusal(big, small) == wd::errc::capacity);
    BOOST_TEST(!refusal(big).failed());
}

// An object of 100,000 distinct keys parses, and each key is found with its value.
void an_object_of_100000_keys() {
    constexpr std::int64_t count = 100'000;
    std::string text = "{";
    for (std::int64_t i = 0; i < count; ++i) {
        text += i == 0 ? "\"k" : ",\"k";
        text += std::to_string(i);
        text += "\":";
        text += std::to_string(i);
    }
    text += '}';
    const wd::document doc = parsed(text);
    BOOST_TEST_EQ(doc.root().size(), static_cast<std::uint64_t>(count));
    std::int64_t found = 0;
    for (std::int64_t i = 0; i < count; ++i) {
        const std::string key = "k" + std::to_string(i);
        if (doc[key].value_or(std::int64_t{-1}) == i) {
            ++found;
        }
    }
    BOOST_TEST_EQ(found, count);
}

// The text of an array of `count` zeros, `[0,0,...]`, whose image takes 192 bytes a zero.
std::string zeros(std::size_t count) {
    std::string text = "[";
    for (std::size_t i = 0; i < count; ++i) {
        text += i == 0 ? "0" : ",0";
    }
    text += ']';
    return text;
}

// The default image limit is 64 MiB: 400,000 zeros, 800 KB of text whose image would take 77 MB,
// are errc::capacity unless max_document_bytes is raised.
void the_default_image_limit_is_64_mib() {
    const std::string text = zeros(400'000);
    BOOST_TEST(refusal(text) == wd::errc::capacity);
    wd::parse_options raised;
    raised.max_document_bytes = std::uint64_t{128} << 20U;
    BOOST_TEST_EQ(parsed(text, raised).root().size(), 400'000U);
}

// A string of 64 MB parses whole under an image limit above it; under one below it, it is
// errc::capacity.
void a_64_mb_string() {
    constexpr std::size_t size = std::size_t{64} << 20U;
    std::string text(size + 2, 'x');
    text.front() = '"';
    text.back() = '"';
    {
        wd::parse_options large;
        large.max_document_bytes = std::uint64_t{256} << 20U;
        const wd::document doc = parsed(text, large);
        const auto read = doc.root().value_or(std::string_view{});
        BOOST_TEST_EQ(read.size(), size);
    }
    wd::parse_options small;
    small.max_document_bytes = std::uint64_t{32} << 20U;
    BOOST_TEST(refusal(text, small) == wd::errc::capacity);
}

// The revivers: a date's exact text becomes a date, base64 bytes, and under javascript_bytes an
// object of the shape JSON.stringify gives a Uint8Array; anything else stays as it was.
void revivers() {
    wd::parse_options dates;
    dates.revive_dates = true;
    const wd::document d = parsed(R"(["2026-10-12T03:04:05.678Z","2026-10-12"])", dates);
    BOOST_TEST(d[0].is_date());
    BOOST_TEST(d[1].is_string());
    BOOST_TEST_EQ(wd::stringify(d), R"(["2026-10-12T03:04:05.678Z","2026-10-12"])");
    BOOST_TEST(parsed(R"(["2026-10-12T03:04:05.678Z"])")[0].is_string());

    wd::parse_options bytes;
    bytes.revive_bytes = true;
    const wd::document b = parsed(R"(["Zm9v","Zm9v!"])", bytes);
    BOOST_TEST(b[0].is_bytes());
    const auto foo = b[0].get<std::vector<std::byte>>();
    if (BOOST_TEST(foo.has_value())) {
        BOOST_TEST_EQ(foo->size(), 3U);
        BOOST_TEST(foo->at(0) == std::byte{'f'});
        BOOST_TEST(foo->at(2) == std::byte{'o'});
    }
    BOOST_TEST(b[1].is_string());
    BOOST_TEST(parsed(R"(["Zm9v"])")[0].is_string());

    auto js = wd::parse<wd::js_traits>(
        R"({"u":{"0":102,"1":111,"2":111},"gap":{"0":1,"2":2},"big":{"0":256},"s":"Zm9v"})", bytes);
    if (!BOOST_TEST(js.has_value())) {
        return;
    }
    BOOST_TEST((*js)["u"].is_bytes());
    BOOST_TEST((*js)["gap"].is_object());
    BOOST_TEST((*js)["big"].is_object());
    BOOST_TEST((*js)["s"].is_string());
    BOOST_TEST_EQ(
        wd::stringify(*js),
        R"({"u":{"0":102,"1":111,"2":111},"gap":{"0":1,"2":2},"big":{"0":256},"s":"Zm9v"})");
}

// A parse into a buffer of every size, from 1 KB to the size that holds the document, succeeds
// and equals the owned parse, or returns errc::capacity.
void parse_into_every_size() {
    std::string text = R"({"a":[)";
    for (std::size_t i = 0; i < 100; ++i) {
        text += i == 0 ? "\"" : ",\"";
        text += "item number " + std::to_string(i);
        text += '"';
    }
    text += "]}";
    const wd::document owned = parsed(text);
    bool fitted = false;
    std::size_t refused = 0;
    for (std::size_t size = 1024; !fitted && size <= (std::size_t{1} << 20U); size += 16) {
        const aligned_buffer buffer(size);
        auto made = wd::parse_into(buffer.bytes(), text);
        if (!made.has_value()) {
            BOOST_TEST(made.error() == wd::errc::capacity);
            ++refused;
            continue;
        }
        fitted = true;
        auto copied = made->clone();
        BOOST_TEST(copied.has_value() && *copied == owned);
        BOOST_TEST_EQ(wd::stringify(*made), wd::stringify(owned));
        BOOST_TEST(made->image().data() == buffer.bytes().data());
    }
    BOOST_TEST(fitted);
    BOOST_TEST(refused > 0U);
}

#ifdef WEBCPP_TEST_DOCUMENT_SUGAR
// parse_or_throw throws the exception of each errc, a syntax error with its offset.
void parse_or_throw_throws() {
    try {
        (void)wd::parse_or_throw("[1,]");
        BOOST_ERROR("[1,] was accepted");
    } catch (const wd::syntax_error& e) {
        BOOST_TEST_EQ(e.offset(), 3U);
        BOOST_TEST(e.code() == wd::errc::syntax);
    }
    try {
        (void)wd::parse_or_throw("1 2");
        BOOST_ERROR("1 2 was accepted");
    } catch (const wd::syntax_error& e) {
        BOOST_TEST_EQ(e.offset(), 2U);
    }
    BOOST_TEST_THROWS((void)wd::parse_or_throw("12345678901234567890123456789012345"),
                      wd::range_error);
    wd::parse_options shallow;
    shallow.max_depth = 1;
    BOOST_TEST_THROWS((void)wd::parse_or_throw("[[1]]", shallow), wd::capacity_error);
    const wd::document doc = wd::parse_or_throw(R"({"a":[1,2]})");
    BOOST_TEST_EQ(wd::stringify(doc), R"({"a":[1,2]})");
    const auto js = wd::parse_or_throw<wd::js_traits>("123456789012345678");
    BOOST_TEST_EQ(wd::stringify(js), "123456789012345680");
}
#endif

}  // namespace

int main() {
    accepted_as_json_parse_accepts();
    numbers_at_their_edges();
    refused_as_malformed();
    a_syntax_error_has_its_offset();
    numbers_past_the_traits();
    limits();
    an_object_of_100000_keys();
    the_default_image_limit_is_64_mib();
    a_64_mb_string();
    revivers();
    parse_into_every_size();
#ifdef WEBCPP_TEST_DOCUMENT_SUGAR
    parse_or_throw_throws();
#endif
    return boost::report_errors();
}
