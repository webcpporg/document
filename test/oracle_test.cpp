// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Node as the oracle, from C++: every case of fixtures/cases is parsed, changed by its operations
// through the document's core API and written, under default_traits (unless the case is js_only)
// and under js_traits, and each text and each operation's return must equal, byte for byte, what
// Node wrote in fixtures/expected (test/oracle/oracle.mjs, whose header documents both formats).
// The fixtures' directory is WEBCPP_TEST_DOCUMENT_FIXTURES, which test/Jamfile defines. The first
// missing expected file is named, so a case Node has not run fails instead of vanishing.

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include "require.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace wd = webcpp::document;
namespace json = boost::json;

namespace {

// The most values one push, unshift or splice carries that the runner passes to the variadic
// call; the cases carry at most 4.
constexpr std::size_t max_values = 8;

/** A case and what Node wrote for it, both read with Boost.JSON. */
struct fixture {
    /** The case's file name, `arrays-push.json`. */
    std::string name;

    /** The case: `text`, `indent`, `js_only` and `operations`. */
    json::object the_case;

    /** Node's output: `text` and `returns`. */
    json::object expected;
};

/** Every case of a directory, with Node's output for each that has one. */
struct loaded {
    /** The cases that have an expected file, in the order of their names. */
    std::vector<fixture> fixtures;

    /** The first expected file that is missing, by the cases' order; empty when none is. */
    std::string first_missing;

    /** How many expected files are missing. */
    std::size_t missing = 0;

    /** A file that could not be read or is not a JSON object, one line each. */
    std::vector<std::string> unreadable;
};

/** The bytes of a file, or nothing when it cannot be opened. */
std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/** A file's JSON object, or nothing, with the reason in `why`. */
std::optional<json::object> read_object(const std::filesystem::path& path, std::string& why) {
    const std::optional<std::string> text = read_file(path);
    if (!text.has_value()) {
        why = path.string() + ": cannot be read";
        return std::nullopt;
    }
    boost::system::error_code error;
    json::value parsed = json::parse(*text, error);
    if (error) {
        why = path.string() + ": " + error.message();
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        why = path.string() + ": not a JSON object";
        return std::nullopt;
    }
    return std::move(parsed.get_object());
}

/** Reads every case file of `cases`, and its namesake under `expected`, sorted by name. */
loaded load(const std::filesystem::path& cases, const std::filesystem::path& expected) {
    loaded found;
    std::vector<std::string> names;
    std::error_code listing;
    std::filesystem::directory_iterator entry(cases, listing);
    if (listing) {
        found.unreadable.push_back(cases.string() + ": " + listing.message());
        return found;
    }
    for (; entry != std::filesystem::directory_iterator(); entry.increment(listing)) {
        if (listing) {
            found.unreadable.push_back(cases.string() + ": " + listing.message());
            return found;
        }
        if (entry->path().extension() == ".json") {
            names.push_back(entry->path().filename().string());
        }
    }
    std::ranges::sort(names);
    for (const std::string& name : names) {
        std::string why;
        std::optional<json::object> the_case = read_object(cases / name, why);
        if (!the_case.has_value()) {
            found.unreadable.push_back(why);
            continue;
        }
        if (!read_file(expected / name).has_value()) {
            if (found.missing++ == 0) {
                found.first_missing = (expected / name).string();
            }
            continue;
        }
        std::optional<json::object> output = read_object(expected / name, why);
        if (!output.has_value()) {
            found.unreadable.push_back(why);
            continue;
        }
        found.fixtures.push_back(
            {.name = name, .the_case = std::move(*the_case), .expected = std::move(*output)});
    }
    return found;
}

/** The directory of cases/ and expected/. */
std::filesystem::path fixtures_root() {
    return {WEBCPP_TEST_DOCUMENT_FIXTURES};
}

/** An integer of a case, as Boost.JSON read it; nothing for any other value. */
std::optional<std::int64_t> integer_of(const json::value& v) {
    if (const std::int64_t* i = v.if_int64()) {
        return *i;
    }
    if (const std::uint64_t* u = v.if_uint64(); u != nullptr && std::in_range<std::int64_t>(*u)) {
        return static_cast<std::int64_t>(*u);
    }
    return std::nullopt;
}

template <class Traits>
using owned_document = wd::basic_document<Traits, wd::owned>;

template <class Traits>
using handle = wd::basic_value<Traits, wd::owned>;

/** The traits' name in a message. */
template <class Traits>
constexpr std::string_view traits_name() {
    return std::same_as<Traits, wd::js_traits> ? "js_traits" : "default_traits";
}

/**
 What an operation gave: its return as compact JSON text, or nothing where Node writes `null`, or
 the reason it could not run.
*/
struct outcome {
    /** The return's text; nothing for `null`, and for `undefined`, which stringifies to "". */
    std::optional<std::string> returned;

    /** Why the operation did not run; empty when it did. */
    std::string failure;
};

outcome failed(std::string why) {
    return {.returned = std::nullopt, .failure = std::move(why)};
}

outcome returning(std::string text) {
    if (text.empty()) {
        return {};
    }
    return {.returned = std::move(text), .failure = {}};
}

/** A JSON text of an operation, read by the document under the traits being tested. */
template <class Traits>
wd::result<owned_document<Traits>> value_of(const json::value& text) {
    const json::string* s = text.if_string();
    if (s == nullptr) {
        return wd::errc::type;
    }
    return wd::parse<Traits>(std::string_view(*s));
}

/** Every value of a list of JSON texts, or the reason one cannot be read. */
template <class Traits>
std::optional<std::string> values_of(const json::value& list,
                                     std::vector<owned_document<Traits>>& out) {
    const json::array* texts = list.if_array();
    if (texts == nullptr) {
        return std::string("the values are not a list");
    }
    if (texts->size() > max_values) {
        return std::to_string(texts->size()) + " values, more than the runner passes";
    }
    for (const json::value& text : *texts) {
        auto v = value_of<Traits>(text);
        if (!v.has_value()) {
            return json::serialize(text) + " does not parse: " + v.error().message();
        }
        out.push_back(std::move(v.unsafe_value()));
    }
    return std::nullopt;
}

/**
 Calls `call` with every document of `values` as its arguments, as `t.push(...values)` spreads
 them, so that the variadic method runs once with all of them.

 Tip: the count is known at run time and the arguments at compile time, so each count up to
 max_values is one instantiation; values_of refuses more.
*/
template <std::size_t N = 0, class Traits, class Call>
auto spread(const std::vector<owned_document<Traits>>& values, Call& call) -> decltype(call()) {
    if constexpr (N < max_values) {
        if (values.size() != N) {
            return spread<N + 1>(values, call);
        }
    }
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return call(values[I]...);
    }(std::make_index_sequence<N>{});
}

/** The handle at a path of keys and indices from the root, as JavaScript's `t` is found. */
template <class Traits>
std::optional<handle<Traits>> at_path(owned_document<Traits>& doc, const json::value& path) {
    const json::array* steps = path.if_array();
    if (steps == nullptr) {
        return std::nullopt;
    }
    std::optional<handle<Traits>> at(std::in_place, doc.root());
    for (const json::value& step : *steps) {
        // A handle is not assignable without exceptions, and with them assignment writes: each
        // step makes a new one.
        if (const json::string* key = step.if_string()) {
            handle<Traits> next = (*at)[std::string_view(*key)];
            at.emplace(std::move(next));
        } else if (const std::optional<std::int64_t> index = integer_of(step);
                   index.has_value() && *index >= 0) {
            handle<Traits> next = (*at)[static_cast<std::uint64_t>(*index)];
            at.emplace(std::move(next));
        } else {
            return std::nullopt;
        }
    }
    return at;
}

/** A method's result as Node writes its return: compact text, or a failure. */
template <class T>
outcome written(const wd::result<T>& r, std::string_view operation) {
    if (!r.has_value()) {
        return failed(std::string(operation) + " failed: " + r.error().message());
    }
    if constexpr (std::same_as<T, void>) {
        return {};
    } else if constexpr (std::same_as<T, bool>) {
        return returning(*r ? "true" : "false");
    } else if constexpr (std::same_as<T, std::uint64_t>) {
        return returning(std::to_string(*r));
    } else {
        return returning(wd::stringify(*r));
    }
}

/** How many entries an operation has, its name and path included; 0 for no operation. */
std::size_t arity(std::string_view name) {
    if (name == "pop" || name == "shift" || name == "reverse") {
        return 2;
    }
    if (name == "set" || name == "erase" || name == "push" || name == "unshift" ||
        name == "set_length" || name == "assign_from") {
        return 3;
    }
    return name == "splice" ? 5 : 0;
}

/** Applies an operation that takes one argument or none. */
template <class Traits>
outcome apply_one(handle<Traits>& t, std::string_view name, const json::array& op) {
    if (name == "set" || name == "assign_from") {
        auto v = value_of<Traits>(op[2]);
        if (!v.has_value()) {
            return failed(json::serialize(op[2]) + " does not parse: " + v.error().message());
        }
        if (name == "set") {
            return written(t.set(v.unsafe_value()), name);
        }
        return written(t.assign_from(std::as_const(v.unsafe_value()).root()), name);
    }
    if (name == "erase") {
        const json::string* key = op[2].if_string();
        if (key == nullptr) {
            return failed("erase: the key is not a string");
        }
        return written(t.erase(std::string_view(*key)), name);
    }
    if (name == "set_length") {
        const std::optional<std::int64_t> n = integer_of(op[2]);
        if (!n.has_value() || *n < 0) {
            return failed("set_length: the length is not a non-negative integer");
        }
        return written(t.set_length(static_cast<std::uint64_t>(*n)), name);
    }
    if (name == "pop") {
        return written(t.pop(), name);
    }
    if (name == "shift") {
        return written(t.shift(), name);
    }
    return written(t.reverse(), name);
}

/** Applies push, unshift or splice, with every value it carries. */
template <class Traits>
outcome apply_spread(handle<Traits>& t, std::string_view name, const json::array& op) {
    const bool splice = name == "splice";
    std::vector<owned_document<Traits>> values;
    if (const std::optional<std::string> why = values_of<Traits>(op[splice ? 4 : 2], values)) {
        return failed(std::string(name) + ": " + *why);
    }
    if (name == "push") {
        auto call = [&](const auto&... v) { return t.push(v...); };
        return written(spread(values, call), name);
    }
    if (name == "unshift") {
        auto call = [&](const auto&... v) { return t.unshift(v...); };
        return written(spread(values, call), name);
    }
    const std::optional<std::int64_t> start = integer_of(op[2]);
    const std::optional<std::int64_t> count = integer_of(op[3]);
    if (!start.has_value() || !count.has_value()) {
        return failed("splice: the start or the count is not an integer");
    }
    auto call = [&](const auto&... v) { return t.splice(*start, *count, v...); };
    return written(spread(values, call), name);
}

/** Applies one operation of a case to the document, as oracle.mjs applies it to JavaScript's. */
template <class Traits>
outcome apply(owned_document<Traits>& doc, const json::array& op) {
    const json::string* name = op.empty() ? nullptr : op[0].if_string();
    if (name == nullptr || op.size() != arity(*name)) {
        return failed("not an operation, or not of its operation's length");
    }
    std::optional<handle<Traits>> t = at_path(doc, op[1]);
    if (!t.has_value()) {
        return failed(std::string(*name) + ": the path is not a list of keys and indices");
    }
    if (*name == "push" || *name == "unshift" || *name == "splice") {
        return apply_spread(*t, *name, op);
    }
    return apply_one(*t, *name, op);
}

/** The first offset at which two texts differ, for a message. */
std::size_t first_difference(std::string_view a, std::string_view b) {
    return static_cast<std::size_t>(std::ranges::mismatch(a, b).in1 - a.begin());
}

void difference(const fixture& f, std::string_view traits, std::string_view what,
                std::string_view node, std::string_view ours) {
    const std::string message =
        f.name + " under " + std::string(traits) + ", " + std::string(what) + ", from byte " +
        std::to_string(first_difference(node, ours)) + ":\n  Node:     " + std::string(node) +
        "\n  document: " + std::string(ours);
    BOOST_ERROR(message.c_str());
}

/** What a case and Node's output for it hold, read. */
struct case_parts {
    /** The case's JSON text. */
    std::string_view text;

    /** The case's operations. */
    const json::array* operations = nullptr;

    /** JSON.stringify's third argument. */
    std::uint32_t indent = 0;

    /** The text Node wrote. */
    std::string_view node_text;

    /** What each operation returned in Node, as text or null. */
    const json::array* node_returns = nullptr;
};

/** A case's parts, or nothing when the case or its expected file is not as oracle.mjs writes. */
std::optional<case_parts> parts_of(const fixture& f) {
    const json::value* text = f.the_case.if_contains("text");
    const json::value* operations = f.the_case.if_contains("operations");
    const json::value* indent = f.the_case.if_contains("indent");
    const json::value* node_text = f.expected.if_contains("text");
    const json::value* node_returns = f.expected.if_contains("returns");
    if (text == nullptr || !text->is_string() || operations == nullptr || !operations->is_array() ||
        indent == nullptr || node_text == nullptr || !node_text->is_string() ||
        node_returns == nullptr || !node_returns->is_array()) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> spaces = integer_of(*indent);
    if (!spaces.has_value() || !std::in_range<std::uint32_t>(*spaces) ||
        node_returns->get_array().size() != operations->get_array().size()) {
        return std::nullopt;
    }
    return case_parts{
        .text = text->get_string(),
        .operations = &operations->get_array(),
        .indent = static_cast<std::uint32_t>(*spaces),
        .node_text = node_text->get_string(),
        .node_returns = &node_returns->get_array(),
    };
}

/** Compares one operation's return with Node's, byte for byte, and reports a difference. */
void compare_return(const fixture& f, std::string_view traits, const std::string& which,
                    const json::value& node, const std::optional<std::string>& ours) {
    const json::string* node_return = node.if_string();
    if (!node.is_null() && node_return == nullptr) {
        BOOST_ERROR((f.name + ": " + which + ": Node's return is neither text nor null").c_str());
        return;
    }
    const std::string_view expected =
        node_return != nullptr ? std::string_view(*node_return) : std::string_view("null");
    const std::string_view got = ours.has_value() ? std::string_view(*ours) : "null";
    if (expected != got) {
        difference(f, traits, which + "'s return", expected, got);
    }
}

/** Runs one case under the traits and reports every difference from Node's output. */
template <class Traits>
void run(const fixture& f) {
    constexpr std::string_view traits = traits_name<Traits>();
    const std::optional<case_parts> parts = parts_of(f);
    if (!parts.has_value()) {
        BOOST_ERROR((f.name + ": the case or its expected file is malformed").c_str());
        return;
    }
    auto parsed = wd::parse<Traits>(parts->text);
    if (!parsed.has_value()) {
        BOOST_ERROR((f.name + " under " + std::string(traits) +
                     ": the text does not parse: " + parsed.error().message())
                        .c_str());
        return;
    }
    owned_document<Traits> doc = std::move(parsed.unsafe_value());
    for (std::size_t i = 0; i < parts->operations->size(); ++i) {
        const json::value& operation = (*parts->operations)[i];
        const std::string which =
            "operation " + std::to_string(i) + " " + json::serialize(operation);
        const json::array* op = operation.if_array();
        const outcome got = op != nullptr ? apply(doc, *op) : failed("not an operation");
        if (!got.failure.empty()) {
            BOOST_ERROR(
                (f.name + " under " + std::string(traits) + ", " + which + ": " + got.failure)
                    .c_str());
            return;
        }
        compare_return(f, traits, which, (*parts->node_returns)[i], got.returned);
    }
    const std::string ours = wd::stringify(doc, parts->indent);
    if (parts->node_text != ours) {
        difference(f, traits, "the text", parts->node_text, ours);
    }
}

bool js_only(const fixture& f) {
    const json::value* flag = f.the_case.if_contains("js_only");
    return flag != nullptr && flag->is_bool() && flag->get_bool();
}

void every_case_matches_node_under_both_traits() {
    const std::filesystem::path root = fixtures_root();
    const loaded found = load(root / "cases", root / "expected");
    for (const std::string& why : found.unreadable) {
        BOOST_ERROR(why.c_str());
    }
    if (found.missing != 0) {
        const std::string message = "no expected file " + found.first_missing + " (" +
                                    std::to_string(found.missing) +
                                    " missing); run b2 libs/document/test/oracle//update-expected";
        BOOST_ERROR(message.c_str());
    }
    if (!BOOST_TEST(!found.fixtures.empty())) {
        BOOST_ERROR(("no case under " + (root / "cases").string()).c_str());
        return;
    }
    std::size_t under_default = 0;
    std::size_t under_js = 0;
    for (const fixture& f : found.fixtures) {
        if (!js_only(f)) {
            run<wd::default_traits>(f);
            ++under_default;
        }
        run<wd::js_traits>(f);
        ++under_js;
    }
    std::cout << "oracle: " << under_default << " cases under default_traits, " << under_js
              << " under js_traits\n";
}

void the_first_missing_expected_file_is_named() {
    // A directory that holds no expected file: every case's is missing, and the first is named.
    const std::filesystem::path root = fixtures_root();
    const loaded found = load(root / "cases", root / "no-expected-files");
    if (!BOOST_TEST(found.missing > 0)) {
        return;
    }
    BOOST_TEST(found.fixtures.empty());
    std::vector<std::string> names;
    std::error_code listing;
    for (std::filesystem::directory_iterator entry(root / "cases", listing), end;
         !listing && entry != end; entry.increment(listing)) {
        names.push_back(entry->path().filename().string());
    }
    webcpp::test::require(BOOST_TEST(!names.empty()));
    BOOST_TEST_EQ(found.missing, names.size());
    BOOST_TEST_EQ(found.first_missing,
                  (root / "no-expected-files" / *std::ranges::min_element(names)).string());
}

}  // namespace

int main() {
    every_case_matches_node_under_both_traits();
    the_first_missing_expected_file_is_named();
    return boost::report_errors();
}
