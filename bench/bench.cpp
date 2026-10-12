// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The benchmarks of Boost.JSON and webcpp::document, one program for every target.
//
//   bench [--data <dir>] [--impl <name>]... [--smoke] [--verbose] [--quick]
//
// <dir> holds nativejson-benchmark's canada.json, citm_catalog.json and twitter.json; without
// --data, the data directory of the checkout WEBCPP_TEST_NATIVEJSON_BENCHMARK names
// (/webcpp/third_party//nativejson-benchmark). The implementations are boost.json, document and
// document.js_traits, every one unless --impl names some.
//
// Measuring, the default, writes one JSON line of what the program was built with, then one per
// benchmark: its first pass, its median and its answer, which run.mjs gathers with JavaScript's.
// --smoke, or a build with WEBCPP_BENCH_SMOKE defined, runs one pass of each benchmark of each
// implementation, then fails, naming each one, when two implementations answer differently: the
// check the library's tests run on every target.
// --verbose names each benchmark of the smoke pass with the time its pass took; --quick measures
// with 3 samples of 1 ms, to check the pipeline rather than to measure.

#include "bench.hpp"
#include "boost_json.hpp"
#include "document.hpp"

#include <webcpp/document.hpp>

#include <boost/json.hpp>
#include <boost/version.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten/version.h>
#endif

#include <array>
#include <cstddef>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace bench = webcpp::bench;
namespace json = boost::json;
namespace wd = webcpp::document;

/** The implementations, in the order they run. */
constexpr std::array<std::string_view, 3> implementations = {
    "boost.json",
    "document",
    "document.js_traits",
};

/** What the command line asks. */
struct options {
    std::string data;
    std::vector<std::string> implementations;
    bool smoke = false;
    bool verbose = false;
    bool quick = false;
};

/** The options of the command line; nothing, after naming the fault, when it is malformed. */
std::optional<options> read_options(std::span<char*> args) {
    options o;
#ifdef WEBCPP_TEST_NATIVEJSON_BENCHMARK
    o.data = WEBCPP_TEST_NATIVEJSON_BENCHMARK "/data";
#endif
#ifdef WEBCPP_BENCH_SMOKE
    o.smoke = true;
#endif
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--smoke") {
            o.smoke = true;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--quick") {
            o.quick = true;
        } else if ((arg == "--data" || arg == "--impl") && i + 1 < args.size()) {
            const std::string_view value = args[++i];
            if (arg == "--data") {
                o.data = value;
            } else {
                o.implementations.emplace_back(value);
            }
        } else {
            std::cerr << "bench: unknown or incomplete argument " << arg
                      << "\nusage: bench [--data <dir>] [--impl <name>]... [--smoke] [--verbose] "
                         "[--quick]\n";
            return std::nullopt;
        }
    }
    if (o.data.empty()) {
        std::cerr << "bench: no --data, and no WEBCPP_TEST_NATIVEJSON_BENCHMARK built in\n";
        return std::nullopt;
    }
    if (o.implementations.empty()) {
        o.implementations.assign(implementations.begin(), implementations.end());
    }
    return o;
}

/** The three files of nativejson-benchmark, read untimed; nothing, after naming one, when not. */
std::optional<bench::texts> read_texts(const std::string& data) {
    bench::texts t;
    const auto files = std::to_array<std::pair<const char*, std::string*>>({
        {"canada.json", &t.canada},
        {"citm_catalog.json", &t.citm},
        {"twitter.json", &t.twitter},
    });
    for (const auto& [name, text] : files) {
        auto read = bench::read_file(data + "/" + name);
        if (!read) {
            std::cerr << "bench: cannot read " << data << "/" << name << "\n";
            return std::nullopt;
        }
        *text = std::move(*read);
    }
    return t;
}

/**
 The dictionaries' keys: the synthetic objects, then citm_catalog.json's events. The smoke pass
 leaves out the objects of 100,000 members, which run the same code as those of 1,000 and would
 take most of its time in a debug build.
*/
std::vector<bench::key_set> dictionaries(const bench::texts& t, bool smoke) {
    std::vector<bench::key_set> out;
    for (const std::size_t count : bench::synthetic_sizes) {
        if (smoke && count > 1000) {
            continue;
        }
        for (const std::size_t length : bench::synthetic_lengths) {
            out.push_back(bench::synthetic(count, length));
        }
    }
    std::vector<std::string> keys;
    const json::value citm = bench::boost_json::parsed(t.citm);
    if (const json::value* events =
            citm.is_object() ? citm.get_object().if_contains("events") : nullptr;
        events != nullptr && events->is_object()) {
        for (const json::key_value_pair& m : events->get_object()) {
            keys.emplace_back(m.key());
        }
    }
    out.push_back(bench::citm_events(std::move(keys)));
    return out;
}

/** The benchmarks of one implementation. */
std::vector<bench::benchmark> benchmarks_of(std::string_view name, const bench::texts& t,
                                            std::span<const bench::key_set> keys) {
    if (name == "boost.json") {
        return bench::boost_json::benchmarks(t, keys);
    }
    if (name == "document") {
        return bench::document_bench::benchmarks<wd::default_traits>(t, keys);
    }
    if (name == "document.js_traits") {
        return bench::document_bench::benchmarks<wd::js_traits>(t, keys);
    }
    return {};
}

/** What the program was built with and for: its compiler, Boost, and its target. */
json::object built_with() {
    json::object meta;
#ifdef __apple_build_version__
    meta["compiler"] = "Apple clang " __clang_version__;
#elif defined(__clang_version__)
    meta["compiler"] = "clang " __clang_version__;
#elif defined(__GNUC__)
    meta["compiler"] = "gcc " __VERSION__;
#else
    meta["compiler"] = "unknown";
#endif
    meta["boost"] = BOOST_LIB_VERSION;
#ifdef __EMSCRIPTEN__
    meta["target"] = "emscripten";
    meta["emscripten"] = std::to_string(__EMSCRIPTEN_MAJOR__) + "." +
                         std::to_string(__EMSCRIPTEN_MINOR__) + "." +
                         std::to_string(__EMSCRIPTEN_TINY__);
#elif defined(__wasi__)
    meta["target"] = "wasi";
#else
    meta["target"] = "native";
#endif
    return meta;
}

/** One pass of every benchmark of every implementation; 1 when two answer differently. */
int smoke(const options& o, const bench::texts& t, std::span<const bench::key_set> keys) {
    std::map<std::pair<std::string, std::string>, std::vector<std::pair<std::string, json::value>>>
        answers;
    for (const std::string& name : o.implementations) {
        const std::vector<bench::benchmark> all = benchmarks_of(name, t, keys);
        if (all.empty()) {
            std::cerr << "bench: " << name << " has no benchmarks: an unknown name, or data "
                      << "it cannot read\n";
            return 1;
        }
        for (const bench::benchmark& b : all) {
            auto [first, answer] = bench::first_pass(b);
            if (o.verbose) {
                std::cout << name << " " << b.op << " " << b.dataset << " " << first * b.ops / 1e6
                          << " ms\n";
            }
            answers[{b.op, b.dataset}].emplace_back(name, std::move(answer));
        }
    }
    std::size_t disagreements = 0;
    for (const auto& [row, given] : answers) {
        for (const auto& [name, answer] : given) {
            if (!bench::agree(answer, given.front().second)) {
                ++disagreements;
                std::cout << "bench: " << row.first << " on " << row.second << ": " << name
                          << " answers " << json::serialize(answer) << ", " << given.front().first
                          << " " << json::serialize(given.front().second) << "\n";
            }
        }
    }
    std::cout << "bench: " << answers.size() << " benchmarks of " << o.implementations.size()
              << " implementations, " << disagreements << " disagreements\n";
    return disagreements == 0 ? 0 : 1;
}

/** Measures every benchmark of every implementation, one JSON line each. */
int measure(const options& o, const bench::texts& t, std::span<const bench::key_set> keys) {
    // Each line is flushed, so that the runner shows the progress of a long run.
    std::cout << json::serialize(json::object{{"meta", built_with()}}) << '\n' << std::flush;
    for (const std::string& name : o.implementations) {
        const std::vector<bench::benchmark> all = benchmarks_of(name, t, keys);
        if (all.empty()) {
            std::cerr << "bench: " << name << " has no benchmarks\n";
            return 1;
        }
        for (const bench::benchmark& b : all) {
            auto [first, answer] = bench::first_pass(b);
            const bench::measurement m =
                bench::measure(b, first, o.quick ? bench::quick_method : bench::full_method);
            const json::object line{
                {"impl", name},
                {"op", b.op},
                {"dataset", b.dataset},
                {"unit", b.unit},
                {"ops", b.ops},
                {"first_ns", m.first_ns},
                {"median_ns", m.median_ns},
                {"batch", m.batch},
                {"samples", m.samples},
                {"answer", std::move(answer)},
            };
            std::cout << json::serialize(line) << '\n' << std::flush;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::optional<options> o = read_options(std::span(argv, static_cast<std::size_t>(argc)));
    if (!o) {
        return 2;
    }
    const std::optional<bench::texts> t = read_texts(o->data);
    if (!t) {
        return 2;
    }
    const std::vector<bench::key_set> keys = dictionaries(*t, o->smoke);
    return o->smoke ? smoke(*o, *t, keys) : measure(*o, *t, keys);
}
