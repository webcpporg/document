// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// quote writes a string as JSON.stringify quotes it, the bytes Node 26 writes held here as
// literals: every escape, C1's controls and other text past ASCII, a lone surrogate of UTF-16, long
// strings; it appends, refuses the text a strict decoder (Node's TextDecoder, fatal) refuses and
// leaves the output unchanged then, and allocates through the output alone, a std::pmr::string on
// a resource of its own, which a replaced operator new proves.

#include <webcpp/document/quote.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

namespace {

// Whether operator new counts, and what it counted: global and mutable, since operator new is.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
bool counting = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::uint64_t news = 0;

}  // namespace

// operator new replaced, to count the allocations of the program's own heap while `counting`. The
// standard library names its parameters with reserved names, which a program cannot use.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    if (counting) {
        ++news;
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        // A test built without exceptions has no std::bad_alloc to throw.
        std::abort();
    }
    return memory;
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

// NOLINTEND(readability-inconsistent-declaration-parameter-name)

namespace {

/** A memory resource that counts what it hands out, from `upstream`. */
class counting_resource final : public std::pmr::memory_resource {
public:
    /** A resource over `upstream`, which outlives it. */
    explicit counting_resource(std::pmr::memory_resource* upstream) noexcept
        : upstream_(upstream) {}

    /** The allocations it made. */
    [[nodiscard]] std::uint64_t allocations() const noexcept { return allocations_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        return upstream_->allocate(bytes, alignment);
    }

    void do_deallocate(void* memory, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(memory, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::uint64_t allocations_ = 0;
};

// UTF-8 text quoted into a std::string, the result checked.
std::string literal_of(std::string_view utf8) {
    std::string out;
    webcpp::test::require(BOOST_TEST(wd::quote(utf8, out).has_value()));
    return out;
}

// UTF-16 text quoted into a std::string.
std::string literal_of(std::u16string_view utf16) {
    std::string out;
    wd::quote(utf16, out);
    return out;
}

// UTF-16 code units, lone surrogates among them.
std::u16string units(std::initializer_list<char16_t> list) {
    return {list};
}

// x"\<BS><FF><LF><CR><TAB><U+0001><U+001F>/é<U+2028>😀, in UTF-8, as stringify_test holds it.
constexpr std::string_view plain_text =
    "x\"\\\b\f\n\r\t\x01\x1f/"
    "\xc3\xa9"
    "\xe2\x80\xa8"
    "\xf0\x9f\x98\x80";

// What JSON.stringify writes of it.
constexpr std::string_view quoted_text = R"("x\"\\\b\f\n\r\t\u0001\u001f/)"
                                         "\xc3\xa9"
                                         "\xe2\x80\xa8"
                                         "\xf0\x9f\x98\x80\"";

void quotes_as_json_stringify() {
    BOOST_TEST_EQ(literal_of(plain_text), quoted_text);
    BOOST_TEST_EQ(literal_of(""), R"("")");
    // Every unit below U+0020, then U+007F, which is not escaped.
    std::string controls;
    for (char c = 0; c < 0x20; ++c) {
        controls.push_back(c);
    }
    controls.push_back('\x7f');
    const std::string expected =
        R"("\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f)"
        R"(\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c)"
        R"(\u001d\u001e\u001f)"
        "\x7f\"";
    BOOST_TEST_EQ(literal_of(controls), expected);
    // U+0080 to U+009F, C1's controls, which JSON.stringify writes as they are: C2 80 to C2 9F in
    // UTF-8, valid though a byte 80 alone is refused.
    std::string c1;
    for (std::uint32_t unit = 0x80; unit < 0xa0; ++unit) {
        c1 += '\xc2';
        c1 += static_cast<char>(unit);
    }
    BOOST_TEST_EQ(literal_of(c1), '"' + c1 + '"');
    // Long strings, past any buffer the writer keeps: one with no escape, one of escapes alone.
    const std::string long_text(10000, 'z');
    BOOST_TEST_EQ(literal_of(long_text), '"' + long_text + '"');
    std::string escaped_quotes = "\"";
    for (std::size_t i = 0; i < 10000; ++i) {
        escaped_quotes += R"(\")";
    }
    escaped_quotes += '"';
    BOOST_TEST_EQ(literal_of(std::string(10000, '"')), escaped_quotes);
}

void a_lone_surrogate_of_utf16() {
    // The cases stringify_test pins for a document of js_traits, which holds UTF-16.
    // Plain literals: MSVC 14.3 does not read a raw string in these macros' arguments as one.
    BOOST_TEST_EQ(literal_of(units({0xD800})), "\"\\ud800\"");
    BOOST_TEST_EQ(literal_of(units({0xDC00})), "\"\\udc00\"");
    BOOST_TEST_EQ(literal_of(units({0xDC00, 0xD800})), "\"\\udc00\\ud800\"");
    BOOST_TEST_EQ(literal_of(units({u'a', 0xDBFF, u'b'})), "\"a\\udbffb\"");
    BOOST_TEST_EQ(literal_of(units({0xD83D, 0xDE00})), "\"\xf0\x9f\x98\x80\"");
    BOOST_TEST_EQ(literal_of(units({0xD83D, 0xDE00, 0xD83D})), "\"\xf0\x9f\x98\x80\\ud83d\"");
    BOOST_TEST_EQ(literal_of(units({0x7F, 0xE9, 0x20AC, 0xFFFF})),
                  "\"\x7f\xc3\xa9\xe2\x82\xac\xef\xbf\xbf\"");
    BOOST_TEST_EQ(literal_of(units({})), R"("")");
    // A group of units repeated past the writer's buffer of 256 bytes, and what JSON.stringify
    // writes of one group.
    constexpr std::string_view group = R"(\"\ud800)"
                                       "\xc3\xa9"
                                       "\xf0\x9f\x98\x80"
                                       R"(\n)";
    std::u16string many;
    std::string expected = "\"";
    for (std::size_t i = 0; i < 300; ++i) {
        many += units({u'"', 0xD800, 0xE9, 0xD83D, 0xDE00, u'\n'});
        expected += group;
    }
    expected += '"';
    BOOST_TEST_EQ(literal_of(many), expected);
}

void appends() {
    std::string out = "[";
    BOOST_TEST(wd::quote("a", out).has_value());
    out += ',';
    wd::quote(u"b", out);
    out += ']';
    BOOST_TEST_EQ(out, R"(["a","b"])");
}

void refuses_what_is_not_utf8() {
    for (const std::string_view text : {
             // A stray continuation byte, a byte UTF-8 never holds, a surrogate encoded, an
             // overlong form, a sequence cut short, and one past U+10FFFF.
             "\x80",
             "a\xff",
             "\xed\xa0\x80",
             "\xc0\xaf",
             "\xe2\x82",
             "\xf4\x90\x80\x80",
             // Lead bytes past F4, which UTF-8 never holds, before three continuation bytes, and
             // F8 before four, the five-byte form of RFC 2279.
             "\xf5\x80\x80\x80",
             "\xf8\x88\x80\x80",
             "\xf8\x88\x80\x80\x80",
             "\xfe\x80\x80\x80",
             "\xff\x80\x80\x80",
             // Overlong forms of three and four bytes: U+0000 as E0 80 80 and as F0 80 80 80.
             "\xe0\x80\x80",
             "\xf0\x80\x80\x80",
             // A sequence of four bytes cut short, at the end of the text and before ASCII: the
             // last one's 61 is an "a".
             "\xf0\x9f\x98",
             "\xf0\x9f",
             "\xf0",
             "\xf0\x9f\x98\x61",
         }) {
        std::string out = "kept";
        const wd::result<void> refused = wd::quote(text, out);
        BOOST_TEST(refused.has_error() && refused.error() == wd::errc::type);
        BOOST_TEST_EQ(out, "kept");
    }
}

void allocates_through_the_output_alone() {
    // The arena's memory, taken from operator new before it counts; a monotonic arena never
    // reuses what a growing string frees, so it holds every size the string grows through.
    std::vector<std::byte> storage(std::size_t{1} << 20U);
    std::pmr::monotonic_buffer_resource arena(storage.data(), storage.size(),
                                              std::pmr::null_memory_resource());
    counting_resource resource(&arena);
    std::pmr::string out(&resource);
    constexpr std::size_t lines = 10000;
    constexpr std::size_t triples = 1000;
    const std::string long_text(lines, '\n');
    std::u16string long_units;
    for (std::size_t i = 0; i < triples; ++i) {
        long_units += units({u'a', 0xD800, 0xE9});
    }
    news = 0;
    counting = true;
    const bool utf8 = wd::quote(long_text, out).has_value();
    const std::size_t utf8_size = out.size();
    wd::quote(std::u16string_view(long_units), out);
    counting = false;
    BOOST_TEST(utf8);
    BOOST_TEST_EQ(news, 0U);
    BOOST_TEST_GT(resource.allocations(), 0U);
    // Each line feed becomes \n, and each 'a', lone surrogate and é a, \ud800 and its 2 bytes.
    BOOST_TEST_EQ(utf8_size, 2 + (lines * 2));
    BOOST_TEST_EQ(out.size(), utf8_size + 2 + (triples * (1 + 6 + 2)));
}

}  // namespace

int main() {
    quotes_as_json_stringify();
    a_lone_surrogate_of_utf16();
    appends();
    refuses_what_is_not_utf8();
    allocates_through_the_output_alone();
    return boost::report_errors();
}
