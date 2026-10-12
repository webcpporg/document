// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Selectors deliver values as they complete: on takes them from the document, observe keeps
// them; a value inside another is delivered first; a scalar when its token ends; a callback's
// failure, or with exceptions its exception, fails the parser; a callback reads and cannot write;
// a million elements under on stay within 64 KB of image; the deliveries are the same however the
// text is split; a malformed selector is errc::type; and a path is written as a selector of its
// place, which parses back.

#include <webcpp/document/parser.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>

#include "parse_corpus.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <stdexcept>
#endif

namespace wd = webcpp::document;

namespace {

// One delivery: the selector that matched, the path, and the value as stringify writes it.
struct received {
    std::string selector;
    std::string path;
    std::string value;

    friend bool operator==(const received&, const received&) = default;
};

// A callback that records each delivery of `selector` in `log`.
auto recorder(std::vector<received>& log, std::string selector) {
    return [&log, selector = std::move(selector)](const wd::const_value& v, wd::path_view where) {
        log.push_back({.selector = selector, .path = where.to_string(), .value = wd::stringify(v)});
    };
}

// The text a finished parser's document holds.
std::string finished(wd::parser& p) {
    auto made = p.finish();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return wd::stringify(*made);
}

void delivery() {
    constexpr std::string_view text = R"({"meta":{"n":2},"items":[{"id":1},{"id":2}]})";
    {
        std::vector<received> log;
        wd::parser p;
        BOOST_TEST(p.on("$.items[*]", recorder(log, "items")).has_value());
        BOOST_TEST(p.write(text).has_value());
        BOOST_TEST_EQ(finished(p), R"({"meta":{"n":2},"items":[]})");
        if (BOOST_TEST_EQ(log.size(), 2U)) {
            BOOST_TEST_EQ(log[0].path, "$['items'][0]");
            BOOST_TEST_EQ(log[0].value, R"({"id":1})");
            BOOST_TEST_EQ(log[1].path, "$['items'][1]");
            BOOST_TEST_EQ(log[1].value, R"({"id":2})");
        }
    }
    {
        std::vector<received> log;
        wd::parser p;
        BOOST_TEST(p.observe("$.meta", recorder(log, "meta")).has_value());
        BOOST_TEST(p.write(text).has_value());
        BOOST_TEST_EQ(finished(p), text);
        if (BOOST_TEST_EQ(log.size(), 1U)) {
            BOOST_TEST_EQ(log[0].path, "$['meta']");
            BOOST_TEST_EQ(log[0].value, R"({"n":2})");
        }
    }
    {
        std::vector<received> log;
        wd::parser p;
        BOOST_TEST(p.on("$.*", recorder(log, "any")).has_value());
        BOOST_TEST(p.write(text).has_value());
        BOOST_TEST_EQ(finished(p), "{}");
        if (BOOST_TEST_EQ(log.size(), 2U)) {
            BOOST_TEST_EQ(log[0].path, "$['meta']");
            BOOST_TEST_EQ(log[1].path, "$['items']");
            BOOST_TEST_EQ(log[1].value, R"([{"id":1},{"id":2}])");
        }
    }
}

// The root completes last; taken by on, the document holds undefined.
void the_root() {
    std::vector<received> log;
    wd::parser p;
    BOOST_TEST(p.on("$", recorder(log, "root")).has_value());
    BOOST_TEST(p.write("[1,2]").has_value());
    auto made = p.finish();
    if (BOOST_TEST(made.has_value())) {
        BOOST_TEST(made->root().is_undefined());
        BOOST_TEST_EQ(wd::stringify(*made), "");
    }
    if (BOOST_TEST_EQ(log.size(), 1U)) {
        BOOST_TEST_EQ(log[0].path, "$");
        BOOST_TEST_EQ(log[0].value, "[1,2]");
    }

    // A number at the top completes only when the text ends.
    log.clear();
    p.reset();
    BOOST_TEST(p.write("12").has_value());
    BOOST_TEST(log.empty());
    BOOST_TEST(p.write("3").has_value());
    BOOST_TEST(log.empty());
    (void)p.finish();
    if (BOOST_TEST_EQ(log.size(), 1U)) {
        BOOST_TEST_EQ(log[0].value, "123");
    }
}

void nested_first() {
    constexpr std::string_view text = R"({"a":{"b":1}})";
    {
        std::vector<received> log;
        wd::parser p;
        BOOST_TEST(p.observe("$.a", recorder(log, "a")).has_value());
        BOOST_TEST(p.observe("$.a.b", recorder(log, "b")).has_value());
        BOOST_TEST(p.write(text).has_value());
        BOOST_TEST_EQ(finished(p), text);
        if (BOOST_TEST_EQ(log.size(), 2U)) {
            BOOST_TEST_EQ(log[0].selector, "b");
            BOOST_TEST_EQ(log[0].value, "1");
            BOOST_TEST_EQ(log[1].selector, "a");
            BOOST_TEST_EQ(log[1].value, R"({"b":1})");
        }
    }
    {
        std::vector<received> log;
        wd::parser p;
        BOOST_TEST(p.observe("$.a", recorder(log, "a")).has_value());
        BOOST_TEST(p.on("$.a.b", recorder(log, "b")).has_value());
        BOOST_TEST(p.write(text).has_value());
        BOOST_TEST_EQ(finished(p), R"({"a":{}})");
        if (BOOST_TEST_EQ(log.size(), 2U)) {
            BOOST_TEST_EQ(log[0].value, "1");
            BOOST_TEST_EQ(log[1].value, "{}");
        }
    }
    {
        // Two selectors matching one value: both receive it, in the order they were added, and
        // an on among them takes it.
        std::vector<received> log;
        wd::parser p;
        BOOST_TEST(p.on("$.a.b", recorder(log, "on")).has_value());
        BOOST_TEST(p.observe("$.*.*", recorder(log, "observe")).has_value());
        BOOST_TEST(p.write(text).has_value());
        BOOST_TEST_EQ(finished(p), R"({"a":{}})");
        if (BOOST_TEST_EQ(log.size(), 2U)) {
            BOOST_TEST_EQ(log[0].selector, "on");
            BOOST_TEST_EQ(log[1].selector, "observe");
            BOOST_TEST_EQ(log[1].value, "1");
        }
    }
}

void scalars_when_their_token_ends() {
    std::vector<received> log;
    wd::parser p;
    BOOST_TEST(p.on("$[*]", recorder(log, "each")).has_value());
    BOOST_TEST(p.write(R"([1,"x",true,null)").has_value());
    if (BOOST_TEST_EQ(log.size(), 4U)) {
        BOOST_TEST_EQ(log[0].value, "1");
        BOOST_TEST_EQ(log[1].value, R"("x")");
        BOOST_TEST_EQ(log[2].value, "true");
        BOOST_TEST_EQ(log[3].value, "null");
        BOOST_TEST_EQ(log[3].path, "$[3]");
    }
    BOOST_TEST(p.write("]").has_value());
    BOOST_TEST_EQ(finished(p), "[]");
}

// A callback's error fails the parser: write returns it, unchanged, then every call until reset.
void a_failing_callback_fails_the_parser() {
    std::size_t calls = 0;
    wd::parser p;
    BOOST_TEST(
        p.observe("$[*]", [&calls](const wd::const_value& v, wd::path_view) -> wd::result<void> {
             ++calls;
             if (v.value_or(std::int64_t{0}) == 2) {
                 return wd::errc::range;
             }
             return {};
         }).has_value());
    BOOST_TEST(p.write("[1,2,3").error() == wd::errc::range);
    BOOST_TEST_EQ(calls, 2U);
    BOOST_TEST(p.write("]").error() == wd::errc::range);
    BOOST_TEST(p.finish().error() == wd::errc::range);
    BOOST_TEST_EQ(calls, 2U);
    p.reset();
    BOOST_TEST(p.write("[5]").has_value());
    BOOST_TEST_EQ(finished(p), "[5]");
    BOOST_TEST_EQ(calls, 3U);

    // An error of another category is passed on as it is.
    const boost::system::error_code mine =
        boost::system::errc::make_error_code(boost::system::errc::operation_canceled);
    wd::parser q;
    BOOST_TEST(q.on("$", [mine](const wd::const_value&, wd::path_view) -> wd::result<void> {
                    return mine;
                }).has_value());
    BOOST_TEST(q.write("7").has_value());
    BOOST_TEST(q.finish().error() == mine);
}

// A callback that is a function, given by its name: one that returns nothing, and one that refuses
// every value.
void ignore_value(const wd::const_value& /*v*/, wd::path_view /*where*/) {}

wd::result<void> refuse_value(const wd::const_value& /*v*/, wd::path_view /*where*/) {
    return wd::errc::type;
}

// A function is a callback, by its name or its address, as a lambda is.
void a_function_is_a_callback() {
    wd::parser p;
    BOOST_TEST(p.observe("$[*]", ignore_value).has_value());
    BOOST_TEST(p.on("$[1]", refuse_value).has_value());
    BOOST_TEST(p.write("[1,").has_value());
    BOOST_TEST(p.write("2]").error() == wd::errc::type);

    wd::parser q;
    BOOST_TEST(q.on("$", &ignore_value).has_value());
    BOOST_TEST(q.write("[3]").has_value());
    BOOST_TEST_EQ(finished(q), "");
}

// The callback's parameter is a read-only handle, which has no set.
void a_callback_cannot_change_the_document() {
    wd::parser p;
    BOOST_TEST(p.observe("$", [](const auto& v, wd::path_view) {
                    static_assert(
                        std::is_same_v<std::remove_cvref_t<decltype(v)>, wd::const_value>);
                    static_assert(!requires { v.set(1); });
                }).has_value());
    BOOST_TEST(p.write("1").has_value());
    BOOST_TEST(p.finish().has_value());
}

// A million elements written in 4 KB chunks generated on the fly, each taken by on, leave an
// image of at most 64 KB.
void a_million_elements_in_bounded_memory() {
#ifdef __wasm__
    // A million elements take 162 s on wasip2 and 79 s on emscripten in a debug build (measured
    // natively: 107 s with clang 21, 41 s with gcc 15); a quarter of them keeps a wasm lane within
    // a minute, and the image's bound is the same claim.
    constexpr std::int64_t count = 250'000;
#else
    constexpr std::int64_t count = 1'000'000;
#endif
    std::int64_t delivered = 0;
    std::int64_t out_of_order = 0;
    wd::parser p;
    BOOST_TEST(p.on("$[*]", [&](const wd::const_value& v, wd::path_view where) {
                    if (v["i"].value_or(std::int64_t{-1}) != delivered ||
                        where.index(0) != static_cast<std::uint64_t>(delivered)) {
                        ++out_of_order;
                    }
                    ++delivered;
                }).has_value());
    std::string pending = "[";
    bool ok = true;
    const auto flush = [&](std::size_t keep) {
        while (ok && pending.size() > keep) {
            const std::size_t n = std::min<std::size_t>(4096, pending.size());
            ok = BOOST_TEST(p.write(std::string_view(pending).substr(0, n)).has_value());
            pending.erase(0, n);
        }
    };
    for (std::int64_t i = 0; ok && i < count; ++i) {
        pending += i == 0 ? R"({"i":)" : R"(,{"i":)";
        pending += std::to_string(i);
        pending += R"(,"s":"abcdefghijklmnopqrstuvwxyz"})";
        flush(4095);
    }
    pending += ']';
    flush(0);
    auto made = p.finish();
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    BOOST_TEST_EQ(delivered, count);
    BOOST_TEST_EQ(out_of_order, 0);
    BOOST_TEST_EQ(wd::stringify(*made), "[]");
    BOOST_TEST_LE(made->image().size(), 65536U);
}

// The deliveries of selectors of every kind, on and observe, while a parser reads `chunks`, and the
// document it finishes with.
std::vector<received> deliveries(const std::vector<std::string_view>& chunks) {
    std::vector<received> log;
    wd::parser p;
    webcpp::test::require(BOOST_TEST(p.observe("$", recorder(log, "$")).has_value()));
    webcpp::test::require(BOOST_TEST(p.observe("$.*", recorder(log, "$.*")).has_value()));
    webcpp::test::require(BOOST_TEST(p.on("$[*]", recorder(log, "$[*]")).has_value()));
    webcpp::test::require(BOOST_TEST(p.observe("$.*[*]", recorder(log, "$.*[*]")).has_value()));
    webcpp::test::require(BOOST_TEST(p.on("$.items[*].id", recorder(log, "id")).has_value()));
    for (const std::string_view chunk : chunks) {
        webcpp::test::require(BOOST_TEST(p.write(chunk).has_value()));
    }
    auto made = p.finish();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    log.push_back({.selector = "finish", .path = "", .value = wd::stringify(*made)});
    return log;
}

// The deliveries of a text are the same however it is split.
void selectors_and_splits() {
    for (const std::string& text : webcpp::test::parse_corpus()) {
        const std::string_view view(text);
        const std::vector<received> whole = deliveries({view});
        BOOST_TEST(!whole.empty());
        for (std::size_t p = 0; p <= text.size(); ++p) {
            BOOST_TEST(deliveries({view.substr(0, p), view.substr(p)}) == whole);
        }
        std::vector<std::string_view> bytes;
        bytes.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            bytes.push_back(view.substr(i, 1));
        }
        BOOST_TEST(deliveries(bytes) == whole);
    }
}

void a_malformed_selector() {
    wd::parser p;
    std::size_t calls = 0;
    const auto count = [&calls](const wd::const_value&, wd::path_view) { ++calls; };
    for (const std::string_view bad : {"a", "$.", "$[", "$[-1]", "$['a", "$..a", "$[01]", ""}) {
        BOOST_TEST(p.on(bad, count).error() == wd::errc::type);
        BOOST_TEST(p.observe(bad, count).error() == wd::errc::type);
    }
    BOOST_TEST(p.write("[1]").has_value());
    BOOST_TEST_EQ(finished(p), "[1]");
    BOOST_TEST_EQ(calls, 0U);
}

// A path is written as a selector of its place, a key's quote and backslash escaped and every other
// byte as it is, which parses back and matches that place; its steps are the text's, which on does
// not renumber.
void paths_as_jsonpath() {
    std::vector<std::string> paths;
    wd::parser p;
    BOOST_TEST(p.on("$.*[*].*", [&](const wd::const_value&, wd::path_view where) {
                    paths.push_back(where.to_string());
                    BOOST_TEST_EQ(where.size(), 3U);
                    BOOST_TEST(!where.is_index(0));
                    BOOST_TEST(where.is_index(1));
                    BOOST_TEST(!where.is_index(2));
                }).has_value());
    BOOST_TEST(p.write(R"({"it's":[{"a\\b":1},{"é":2,"":3}]})").has_value());
    BOOST_TEST_EQ(finished(p), R"({"it's":[{},{}]})");
    if (BOOST_TEST_EQ(paths.size(), 3U)) {
        BOOST_TEST_EQ(paths[0], R"($['it\'s'][0]['a\\b'])");
        BOOST_TEST_EQ(paths[1], "$['it\\'s'][1]['\xc3\xa9']");
        BOOST_TEST_EQ(paths[2], R"($['it\'s'][1][''])");
    }
    for (const std::string& path : paths) {
        BOOST_TEST(wd::detail::selector::parse(path).has_value());
    }
    const wd::path_view root;
    BOOST_TEST_EQ(root.size(), 0U);
    BOOST_TEST_EQ(root.to_string(), "$");

    std::vector<std::uint64_t> indices;
    std::vector<std::string> keys;
    wd::parser q;
    BOOST_TEST(q.on("$.list[*]", [&](const wd::const_value&, wd::path_view where) {
                    keys.emplace_back(where.key(0));
                    indices.push_back(where.index(1));
                }).has_value());
    BOOST_TEST(q.write(R"({"list":["a","b","c"]})").has_value());
    (void)q.finish();
    BOOST_TEST((indices == std::vector<std::uint64_t>{0, 1, 2}));
    BOOST_TEST((keys == std::vector<std::string>{"list", "list", "list"}));
}

// A path with a key of a newline, a quote, a backslash and U+0001 is written with the quote and
// the backslash escaped and the rest as it is, and parses back as a selector of that key.
void a_path_parses_back() {
    std::vector<std::string> paths;
    wd::parser p;
    const auto subscribed = p.observe("$.*", [&paths](const wd::const_value&, wd::path_view where) {
        paths.push_back(where.to_string());
    });
    BOOST_TEST(subscribed.has_value());
    BOOST_TEST(p.write(R"({"a\n'\\\u0001":1})").has_value());
    (void)p.finish();
    if (!BOOST_TEST_EQ(paths.size(), 1U)) {
        return;
    }
    BOOST_TEST_EQ(paths[0], "$['a\n\\'\\\\\x01']");
    const auto parsed = wd::detail::selector::parse(paths[0]);
    if (BOOST_TEST(parsed.has_value()) && BOOST_TEST_EQ(parsed->steps().size(), 1U)) {
        BOOST_TEST_EQ(parsed->steps()[0].key, "a\n'\\\x01");
    }
}

// Selectors are kept across reset, for the next text.
void selectors_survive_reset() {
    std::size_t calls = 0;
    wd::parser p;
    BOOST_TEST(
        p.on("$[*]", [&calls](const wd::const_value&, wd::path_view) { ++calls; }).has_value());
    BOOST_TEST(p.write("[1,2]").has_value());
    (void)p.finish();
    p.reset();
    BOOST_TEST(p.write("[3]").has_value());
    BOOST_TEST_EQ(finished(p), "[]");
    BOOST_TEST_EQ(calls, 3U);
}

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
// An exception a callback throws propagates from write, and leaves the parser failed: every later
// call throws it again, until reset.
void a_throwing_callback() {
    wd::parser p;
    BOOST_TEST(p.on("$[*]", [](const wd::const_value& v, wd::path_view) {
                    if (v.value_or(std::int64_t{0}) == 2) {
                        throw std::runtime_error("two");
                    }
                }).has_value());
    BOOST_TEST(p.write("[1,").has_value());
    BOOST_TEST_THROWS((void)p.write("2,3]"), std::runtime_error);
    BOOST_TEST_THROWS((void)p.write("]"), std::runtime_error);
    BOOST_TEST_THROWS((void)p.finish(), std::runtime_error);
    p.reset();
    BOOST_TEST(p.write("[1]").has_value());
    BOOST_TEST_EQ(finished(p), "[]");
}

// An exception thrown at the end of the text, where the root completes, propagates from finish.
void a_callback_throwing_at_the_end() {
    wd::parser p;
    BOOST_TEST(p.observe("$", [](const wd::const_value&, wd::path_view) {
                    throw std::runtime_error("at the end");
                }).has_value());
    BOOST_TEST(p.write("1").has_value());
    BOOST_TEST_THROWS((void)p.finish(), std::runtime_error);
}
#endif

}  // namespace

int main() {
    delivery();
    the_root();
    nested_first();
    scalars_when_their_token_ends();
    a_failing_callback_fails_the_parser();
    a_function_is_a_callback();
    a_callback_cannot_change_the_document();
    a_million_elements_in_bounded_memory();
    selectors_and_splits();
    a_malformed_selector();
    paths_as_jsonpath();
    a_path_parses_back();
    selectors_survive_reset();
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    a_throwing_callback();
    a_callback_throwing_at_the_end();
#endif
    return boost::report_errors();
}
