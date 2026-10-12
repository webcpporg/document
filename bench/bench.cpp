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

#include <algorithm>
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

/**
 A row the smoke requires: the implementation `impl` must answer it on every transmission file,
 under the name `answered_as` in the answers of the row `op`.
*/
struct requirement {
    std::string_view op;
    std::string_view impl;
    std::string_view answered_as;
};

/**
 The transmission rows: Boost.JSON and document send and receive each file; document's sealed send
 and receive answers the same row, as "document sealed", so that its answer is checked too.
*/
constexpr auto transmission = std::to_array<requirement>({
    {.op = "send_receive", .impl = "boost.json", .answered_as = "boost.json"},
    {.op = "send_receive", .impl = "document", .answered_as = "document"},
    {.op = "send_receive", .impl = "document", .answered_as = "document sealed"},
});

/** The files the transmission rows name, as their benchmarks name them. */
constexpr std::array<std::string_view, 3> transmission_files = {"canada", "citm", "twitter"};

/** What the command line asks. */
struct options {
    std::string data;
    std::vector<std::string> implementations;
    bool smoke = false;
    bool verbose = false;
    bool quick = false;

    /** The directory --wire writes the files' sizes to; empty when the run measures or checks. */
    std::string wire;
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
        } else if ((arg == "--data" || arg == "--impl" || arg == "--wire") && i + 1 < args.size()) {
            const std::string_view value = args[++i];
            if (arg == "--data") {
                o.data = value;
            } else if (arg == "--wire") {
                o.wire = value;
            } else {
                o.implementations.emplace_back(value);
            }
        } else {
            std::cerr << "bench: unknown or incomplete argument " << arg
                      << "\nusage: bench [--data <dir>] [--impl <name>]... [--smoke] [--verbose] "
                         "[--quick] [--wire <dir>]\n";
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

/** The answers of the benchmarks, by row: the operation and the dataset, each with the names and
 the answers of the implementations that gave them. */
using answers_by_row =
    std::map<std::pair<std::string, std::string>, std::vector<std::pair<std::string, json::value>>>;

/** Runs one pass of every benchmark of the implementation `name` and records each answer under
 its row; false, after naming the implementation, when it has no benchmarks. */
bool collect(std::string_view name, const options& o, const bench::texts& t,
             std::span<const bench::key_set> keys, answers_by_row& answers) {
    const std::vector<bench::benchmark> all = benchmarks_of(name, t, keys);
    if (all.empty()) {
        std::cerr << "bench: " << name << " has no benchmarks: an unknown name, or data "
                  << "it cannot read\n";
        return false;
    }
    for (const bench::benchmark& b : all) {
        auto [first, answer] = bench::first_pass(b);
        if (o.verbose) {
            std::cout << name << " " << b.op << " " << b.dataset << " " << first * b.ops / 1e6
                      << " ms\n";
        }
        // A sealed send and receive answers the row of the plain one, as "document sealed".
        const bool sealed = b.op == "send_receive_sealed";
        answers[{sealed ? "send_receive" : b.op, b.dataset}].emplace_back(
            sealed ? std::string(name) + " sealed" : std::string(name), std::move(answer));
    }
    return true;
}

/** Names each answer that differs from the first answer of its row; returns how many do. */
std::size_t report_disagreements(const answers_by_row& answers) {
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
    return disagreements;
}

/** Whether the row of the requirement `r` on `file` holds the answer `r` names. */
bool answered(const answers_by_row& answers, const requirement& r, std::string_view file) {
    const auto row = answers.find({std::string(r.op), std::string(file)});
    return row != answers.end() && std::ranges::any_of(row->second, [&](const auto& given) {
               return given.first == r.answered_as;
           });
}

/** Names each transmission row that an implementation of the run did not answer; returns how
 many were missing. */
std::size_t report_missing(const options& o, const answers_by_row& answers) {
    std::size_t missing = 0;
    for (const requirement& r : transmission) {
        if (std::ranges::find(o.implementations, r.impl) == o.implementations.end()) {
            continue;
        }
        for (const std::string_view file : transmission_files) {
            if (!answered(answers, r, file)) {
                ++missing;
                std::cout << "bench: " << r.op << " on " << file << ": " << r.answered_as
                          << " did not answer\n";
            }
        }
    }
    return missing;
}

/** One pass of every benchmark of every implementation; 1 when two answer differently. */
int smoke(const options& o, const bench::texts& t, std::span<const bench::key_set> keys) {
    answers_by_row answers;
    for (const std::string& name : o.implementations) {
        if (!collect(name, o, t, keys, answers)) {
            return 1;
        }
    }
    const std::size_t disagreements = report_disagreements(answers);
    // A row that is missing is a gap in the page, not a disagreement: the smoke names it too.
    const std::size_t missing = report_missing(o, answers);
    std::cout << "bench: " << answers.size() << " benchmarks of " << o.implementations.size()
              << " implementations, " << disagreements << " disagreements, " << missing
              << " missing\n";
    return disagreements == 0 && missing == 0 ? 0 : 1;
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

/** Writes `bytes` to the file `path`, whole; false when it cannot be written. */
bool write_file(const std::string& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

/**
 Writes what each file weighs on the wire, for the page: `<dir>/<name>.boost.json`, the text
 Boost.JSON serializes the file's value to, and `<dir>/<name>.image`, the image of its document
 after shrink_to_fit. bench/wire.mjs sizes them, and compresses them and the JSON.stringify text
 too. Not timed.
*/
int wire(const options& o, const bench::texts& t) {
    const auto files = std::to_array<std::pair<const char*, const std::string*>>({
        {"canada", &t.canada},
        {"citm", &t.citm},
        {"twitter", &t.twitter},
    });
    for (const auto& [name, text] : files) {
        auto parsed = wd::parse<wd::default_traits>(*text);
        if (!parsed.has_value()) {
            std::cerr << "bench: " << name << " does not parse as a document\n";
            return 1;
        }
        wd::basic_document<wd::default_traits, wd::owned> document =
            std::move(parsed).unsafe_value();
        if (!document.shrink_to_fit().has_value()) {
            std::cerr << "bench: " << name << " cannot be shrunk\n";
            return 1;
        }
        const std::string json_text = json::serialize(bench::boost_json::parsed(*text));
        const std::span<const std::byte> image = document.image();
        const std::string_view image_bytes(reinterpret_cast<const char*>(image.data()),
                                           image.size());
        if (!write_file(o.wire + "/" + name + ".boost.json", json_text) ||
            !write_file(o.wire + "/" + name + ".image", image_bytes)) {
            std::cerr << "bench: cannot write the sizes of " << name << " to " << o.wire << "\n";
            return 1;
        }
    }
    return 0;
}

}  // namespace

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

namespace {

namespace dbench = webcpp::bench::document_bench;

/**
 The state of the Worker benchmarks' transfers in this module: the document sent, and the image
 received into the buffer and opened in place. The JavaScript side calls the exports below one at a
 time, as it runs the passes.
*/
struct transmission_state {
    std::optional<wd::basic_document<wd::default_traits, wd::owned>> sent;
    dbench::image_buffer buffer;
    std::optional<wd::basic_document<wd::default_traits, wd::external>> opened;
    std::string statistics;
};

transmission_state& transmission_of() {
    static transmission_state state;
    return state;
}

}  // namespace

extern "C" {

/** Parses one of the three files (0 canada, 1 citm, 2 twitter) and keeps it to send; its image's
 * size, or 0. */
EMSCRIPTEN_KEEPALIVE std::size_t webcpp_bench_send(int file) {
    constexpr std::array<const char*, 3> names = {"canada.json", "citm_catalog.json",
                                                  "twitter.json"};
    if (file < 0 || file >= static_cast<int>(names.size())) {
        return 0;
    }
    const std::optional<std::string> text = bench::read_file(std::string("/data/") + names[file]);
    if (!text) {
        return 0;
    }
    auto parsed = wd::parse<wd::default_traits>(*text);
    transmission_state& state = transmission_of();
    state.sent.reset();
    if (!parsed.has_value()) {
        return 0;
    }
    state.sent = std::move(parsed).unsafe_value();
    return state.sent->image().size();
}

/** The image of the document sent, valid until the next send. */
EMSCRIPTEN_KEEPALIVE const std::byte* webcpp_bench_image() {
    const transmission_state& state = transmission_of();
    return state.sent ? state.sent->image().data() : nullptr;
}

/** A 16-aligned buffer of `size` bytes for a received image; the image opened before is released.
 */
EMSCRIPTEN_KEEPALIVE std::byte* webcpp_bench_receive(std::size_t size) {
    transmission_state& state = transmission_of();
    state.opened.reset();
    state.buffer = dbench::allocate_image(size);
    return state.buffer.get();
}

/** Opens the image received, in place and with its seal checked: 1 when it opens, 0 when it is
 * refused. */
EMSCRIPTEN_KEEPALIVE int webcpp_bench_open(std::size_t size) {
    transmission_state& state = transmission_of();
    state.opened.reset();
    if (!state.buffer) {
        return 0;
    }
    auto opened = wd::open<wd::default_traits>(std::span<std::byte>(state.buffer.get(), size));
    if (!opened.has_value()) {
        return 0;
    }
    state.opened = std::move(opened).unsafe_value();
    return 1;
}

/** The Statistics of the image opened, as JSON text, valid until the next call. */
EMSCRIPTEN_KEEPALIVE const char* webcpp_bench_statistics() {
    transmission_state& state = transmission_of();
    state.statistics = json::serialize(dbench::statistics_of<wd::default_traits>(state.opened));
    return state.statistics.c_str();
}

}  // extern "C"
#endif

int main(int argc, char** argv) {
    const std::optional<options> o = read_options(std::span(argv, static_cast<std::size_t>(argc)));
    if (!o) {
        return 2;
    }
    const std::optional<bench::texts> t = read_texts(o->data);
    if (!t) {
        return 2;
    }
    if (!o->wire.empty()) {
        return wire(*o, *t);
    }
    const std::vector<bench::key_set> keys = dictionaries(*t, o->smoke);
    return o->smoke ? smoke(*o, *t, keys) : measure(*o, *t, keys);
}
