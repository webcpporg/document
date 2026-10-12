// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The benchmarks' harness, shared by every C++ implementation: a benchmark, its measurement after
 the methodology of the page's section "Benchmarks", the data every implementation reads, and the
 comparison of their answers. bench.mjs is its twin in JavaScript, and generates the same data.
*/

#ifndef WEBCPP_DOCUMENT_BENCH_BENCH_HPP
#define WEBCPP_DOCUMENT_BENCH_BENCH_HPP

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::bench {

/**
 One benchmark of one implementation: an operation on a dataset, run in passes.

 `prepare(k)` makes, untimed, what `k` passes consume (fresh copies, for an operation that changes
 its input); `run(k)` runs `k` passes, timed, and returns a checksum of their results, which the
 harness writes to a volatile sink so that no pass is optimised away; `answer()` describes the
 result of the last pass, which every implementation must give alike.
*/
struct benchmark {
    /** The operation, as the results name it: `parse`, `lookup_hit`, ... */
    std::string op{};

    /** The dataset: `canada`, `citm.events`, `s1000.k15`, ... */
    std::string dataset{};

    /** What one operation of a pass is: `document`, `lookup`, `element`, ... */
    std::string unit{};

    /** How many operations one pass makes. */
    double ops = 1;

    /** Makes the inputs of `k` passes, untimed; empty when the passes share one input. */
    std::function<void(std::size_t)> prepare{};

    /**
     The most passes one `prepare` makes the inputs of, so that the copies of a sample fit in
     memory: a sample of more passes is prepared and timed in parts, its time their sum.
    */
    std::size_t chunk = SIZE_MAX;

    /** Runs `k` passes and returns a checksum of their results. */
    std::function<std::uint64_t(std::size_t)> run{};

    /** The result of the last pass, as every implementation must give it. */
    std::function<boost::json::value()> answer{};
};

/** What the harness measured of one benchmark, in nanoseconds per operation. */
struct measurement {
    /** The first pass, before any warm-up: the cold cost. */
    double first_ns = 0;

    /** The median of the samples, each of `batch` passes and at least 10 ms long. */
    double median_ns = 0;

    /** The passes of one sample. */
    std::size_t batch = 1;

    /** The samples the median is taken over. */
    std::size_t samples = 0;
};

/** How a benchmark is sampled. */
struct method {
    /** The shortest sample, in nanoseconds. */
    double min_sample_ns = 0;

    /** The samples the median is taken over. */
    std::size_t samples = 0;
};

/** The methodology: samples of at least 10 ms, which a browser's clock reads well, 10 of them. */
inline constexpr method full_method{.min_sample_ns = 10e6, .samples = 10};

/** What --quick takes, to check the pipeline rather than to measure: 3 samples of 1 ms. */
inline constexpr method quick_method{.min_sample_ns = 1e6, .samples = 3};

/** The nanoseconds since an arbitrary start, from the steady clock. */
inline double now_ns() {
    return std::chrono::duration<double, std::nano>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/**
 Runs `k` passes of `b`, timed, in parts of at most `b.chunk` passes, each prepared untimed, its
 checksum written to `sink`: the passes' results reach a volatile object, which the compiler must
 write, so it cannot drop the work that computes them.
*/
inline double timed(const benchmark& b, std::size_t k, volatile std::uint64_t& sink) {
    double total = 0;
    for (std::size_t done = 0; done < k;) {
        const std::size_t part = std::min(k - done, b.chunk);
        if (b.prepare) {
            b.prepare(part);
        }
        const double start = now_ns();
        sink = b.run(part);
        total += now_ns() - start;
        done += part;
    }
    return total;
}

/** The passes of a prepared benchmark one part holds: about 65,536 members' worth of copies. */
inline std::size_t chunk_of(std::size_t members) noexcept {
    return std::max<std::size_t>(1, 65536 / std::max<std::size_t>(1, members));
}

/** The first pass of `b`, timed, in nanoseconds per operation, and its answer. */
inline std::pair<double, boost::json::value> first_pass(const benchmark& b) {
    volatile std::uint64_t sink = 0;
    const double first = timed(b, 1, sink);
    return {first / b.ops, b.answer()};
}

/**
 Measures `b` after its first pass: warms it up while it finds the passes a sample needs to last
 `how.min_sample_ns`, runs one more sample of that many passes, discarded, then takes the median
 of `how.samples` samples.
*/
inline measurement measure(const benchmark& b, double first_ns, const method& how) {
    volatile std::uint64_t sink = 0;
    measurement m;
    m.first_ns = first_ns;
    std::size_t k = 1;
    double t = timed(b, k, sink);
    while (t < how.min_sample_ns) {
        // Ten times the passes while a sample is too short for the clock to read it well (a
        // browser's reads 5 us, and may read 0): then twice at least, and enough for a sample with
        // a fifth more, from what k took.
        const double wanted = how.min_sample_ns * 1.2 / std::max(t, 1.0) * static_cast<double>(k);
        k = t < how.min_sample_ns / 100 ? k * 10
                                        : std::max(k * 2, static_cast<std::size_t>(wanted));
        t = timed(b, k, sink);
    }
    timed(b, k, sink);
    std::vector<double> samples;
    samples.reserve(how.samples);
    for (std::size_t i = 0; i < how.samples; ++i) {
        samples.push_back(timed(b, k, sink) / static_cast<double>(k) / b.ops);
    }
    std::ranges::sort(samples);
    m.median_ns = (samples[(how.samples - 1) / 2] + samples[how.samples / 2]) / 2;
    m.batch = k;
    m.samples = how.samples;
    return m;
}

/**
 Whether two answers agree: equal, but for two Statistics of different encodings, whose string
 lengths count UTF-8 bytes in one and UTF-16 code units in the other, and are not compared.
*/
inline bool agree(const boost::json::value& a, const boost::json::value& b) {
    const boost::json::object* x = a.if_object();
    const boost::json::object* y = b.if_object();
    if (x == nullptr || y == nullptr || !x->contains("unit") || !y->contains("unit") ||
        x->at("unit") == y->at("unit")) {
        return a == b;
    }
    boost::json::object p = *x;
    boost::json::object q = *y;
    for (boost::json::object* o : {&p, &q}) {
        o->erase("unit");
        o->erase("length");
    }
    return p == q;
}

/** nativejson-benchmark's Statistics (src/test.h): what a full traversal counts. */
struct statistics {
    std::uint64_t objects = 0;
    std::uint64_t arrays = 0;
    std::uint64_t numbers = 0;
    std::uint64_t strings = 0;
    std::uint64_t trues = 0;
    std::uint64_t falses = 0;
    std::uint64_t nulls = 0;
    std::uint64_t members = 0;
    std::uint64_t elements = 0;

    /** The length of every string and key, in the unit of the implementation's encoding. */
    std::uint64_t length = 0;

    /** A checksum of the counts, for the sink. */
    [[nodiscard]] std::uint64_t sum() const noexcept {
        return objects + arrays + numbers + strings + trues + falses + nulls + members + elements +
               length;
    }

    /** The counts as an answer, with the unit of `length`: `utf8` or `utf16`. */
    [[nodiscard]] boost::json::value json(std::string_view unit) const {
        return boost::json::object{
            {"objects", objects},   {"arrays", arrays}, {"numbers", numbers}, {"strings", strings},
            {"trues", trues},       {"falses", falses}, {"nulls", nulls},     {"members", members},
            {"elements", elements}, {"length", length}, {"unit", unit},
        };
    }
};

/**
 Mulberry32, a 32-bit generator with a fixed seed, so that every implementation, the JavaScript one
 included (bench.mjs), generates the same keys in the same order.
*/
class mulberry32 {
public:
    explicit mulberry32(std::uint32_t seed) noexcept : state_(seed) {}

    std::uint32_t operator()() noexcept {
        state_ += 0x6D2B79F5U;
        std::uint32_t z = state_;
        z = (z ^ (z >> 15U)) * (z | 1U);
        z ^= z + ((z ^ (z >> 7U)) * (z | 61U));
        return z ^ (z >> 14U);
    }

private:
    std::uint32_t state_;
};

/** A key of `length` characters, a letter then letters and digits: never an array index. */
inline std::string make_key(mulberry32& rng, std::size_t length) {
    constexpr std::string_view letters = "abcdefghijklmnopqrstuvwxyz";
    constexpr std::string_view alphanumerics = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string key(1, letters[rng() % letters.size()]);
    while (key.size() < length) {
        key += alphanumerics[rng() % alphanumerics.size()];
    }
    return key;
}

/** Shuffles `v` with Fisher and Yates' algorithm, from `seed`. */
template <class T>
void shuffle(std::vector<T>& v, std::uint32_t seed) {
    mulberry32 rng(seed);
    for (std::size_t i = v.size(); i > 1; --i) {
        std::swap(v[i - 1], v[rng() % i]);
    }
}

/**
 The keys of one dictionary dataset: those inserted, in order, each with its index as its value;
 the same keys shuffled, the order of the lookups; keys that are not there; and the keys every
 other one of which is deleted.
*/
struct key_set {
    /** The dataset's name: `s1000.k15`, or `citm.events`. */
    std::string name;

    /** The keys in insertion order (the object's, for citm.events, sorted). */
    std::vector<std::string> keys;

    /** The keys in the lookups' order. */
    std::vector<std::string> lookups;

    /** Keys that are not members. */
    std::vector<std::string> misses;

    /** The keys deleted: every other one of the lookups' order, at most `max_deletes`. */
    std::vector<std::string> deletes;
};

/**
 The most members one pass of delete deletes: deleting half of 100,000 members with Boost.JSON's
 stable_erase, which moves every member after the one it erases, takes 25 s a pass.
*/
inline constexpr std::size_t max_deletes = 1000;

/** Fills `s.deletes` from its lookups' order. */
inline void choose_deletes(key_set& s) {
    for (std::size_t i = 0; i < s.lookups.size() && s.deletes.size() < max_deletes; i += 2) {
        s.deletes.push_back(s.lookups[i]);
    }
}

/**
 The synthetic objects of Tessil's benchmark: `count` members whose keys are `length` characters
 long, generated from a seed of their own.
*/
inline key_set synthetic(std::size_t count, std::size_t length) {
    const auto seed = static_cast<std::uint32_t>((count * 100) + length);
    key_set s;
    s.name = "s" + std::to_string(count) + ".k" + std::to_string(length);
    mulberry32 hits(seed);
    mulberry32 misses(seed + 1);
    for (std::size_t i = 0; i < count; ++i) {
        s.keys.push_back(make_key(hits, length));
        s.misses.push_back(make_key(misses, length));
    }
    s.lookups = s.keys;
    shuffle(s.lookups, seed + 2);
    choose_deletes(s);
    return s;
}

/**
 The keys of citm_catalog.json's widest object, `events` (184 members, each an integer-like key),
 sorted so that every implementation gets them in one order whatever its own, and nine-digit keys
 starting with 9 that are not among them.
*/
inline key_set citm_events(std::vector<std::string> keys) {
    key_set s;
    s.name = "citm.events";
    std::ranges::sort(keys);
    s.keys = std::move(keys);
    s.lookups = s.keys;
    shuffle(s.lookups, 8);
    mulberry32 rng(7);
    for (std::size_t i = 0; i < s.keys.size(); ++i) {
        std::string key = "9";
        while (key.size() < 9) {
            key += static_cast<char>('0' + (rng() % 10));
        }
        s.misses.push_back(std::move(key));
    }
    choose_deletes(s);
    return s;
}

/** The synthetic objects' sizes: 10 to 100,000 members. */
inline constexpr std::array<std::size_t, 3> synthetic_sizes = {10, 1000, 100000};

/** The synthetic objects' key lengths: Tessil's 15 and 50 characters. */
inline constexpr std::array<std::size_t, 2> synthetic_lengths = {15, 50};

/** The texts of nativejson-benchmark's three files, read untimed. */
struct texts {
    std::string canada;
    std::string citm;
    std::string twitter;
};

/** The whole content of a file; nothing when it cannot be read. */
inline std::optional<std::string> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/**
 The id the query find_tweet looks for: simdjson's tweet (benchmark/find_tweet/find_tweet.h), whose
 id is 505874901689851904 there. nativejson-benchmark's twitter.json writes the ids as JavaScript
 printed them once read as doubles, and holds it as 505874901689851900: the same double, but another
 integer.
*/
inline constexpr std::uint64_t find_tweet_id = 505874901689851900ULL;

/** The largest retweet count the query top_tweet keeps, simdjson's. */
inline constexpr std::int64_t top_tweet_max = 60;

/** The pushes, then the pops, of one pass of push_pop. */
inline constexpr std::size_t push_pop_count = 100;

/** The answer of a lookup pass: the keys found, and the sum of their values. */
inline boost::json::value found_answer(const std::pair<std::uint64_t, std::int64_t>& found) {
    return {{"found", found.first}, {"sum", found.second}};
}

/** The answer of an iteration pass: the members read, and the sum of their keys and values. */
inline boost::json::value iterate_answer(const std::pair<std::uint64_t, std::int64_t>& read) {
    return {{"count", read.first}, {"sum", read.second}};
}

/** The answer of distinct_user_id: how many distinct ids, and their sum. */
inline boost::json::value distinct_answer(const std::vector<std::uint64_t>& ids) {
    std::uint64_t total = 0;
    for (const std::uint64_t id : ids) {
        total += id;
    }
    return {{"count", ids.size()}, {"sum", total}};
}

/** A 32-bit FNV-1a step over `text`, for a checksum of strings. */
inline std::uint64_t fnv(std::string_view text) noexcept {
    std::uint32_t h = 2166136261U;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return h;
}

}  // namespace webcpp::bench

#endif  // WEBCPP_DOCUMENT_BENCH_BENCH_HPP
