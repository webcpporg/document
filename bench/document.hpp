// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The benchmarks of webcpp::document: each operation of the page's section "Benchmarks" written as a
 user of the document and its handles writes it, through the core that returns its errors as
 values, and by hand where the document has no such method (map, filter, reduce, find, join).
*/

#ifndef WEBCPP_DOCUMENT_BENCH_DOCUMENT_HPP
#define WEBCPP_DOCUMENT_BENCH_DOCUMENT_HPP

#include "bench.hpp"

#include <webcpp/document.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::bench::document_bench {

namespace json = boost::json;
namespace wd = webcpp::document;

template <class Traits>
using document = wd::basic_document<Traits, wd::owned>;

template <class Traits>
using const_value = wd::basic_const_value<Traits, wd::owned>;

template <class Traits>
using value = wd::basic_value<Traits, wd::owned>;

/** Whether the traits hold text in UTF-16, as JavaScript does: `js_traits`. */
template <class Traits>
inline constexpr bool utf16 = std::same_as<typename Traits::string, wd::utf16>;

/** The unit a Statistics' length counts under the traits. */
template <class Traits>
constexpr std::string_view unit() noexcept {
    return utf16<Traits> ? "utf16" : "utf8";
}

/** The length of a string, in the code units of the traits' encoding. */
template <class Traits, class Storage>
std::uint64_t string_length(const wd::basic_const_value<Traits, Storage>& v) {
    if constexpr (utf16<Traits>) {
        return v.template value_or<std::u16string_view>({}).size();
    } else {
        return v.template value_or<std::string_view>({}).size();
    }
}

// NOLINTBEGIN(misc-no-recursion): a value is counted by counting its children, as nativejson's
// GenStat does.
/** Adds what `v` holds to `s`, as nativejson-benchmark's GenStat counts it. */
template <class Traits, class Storage>
void count(const wd::basic_const_value<Traits, Storage>& v, statistics& s) {
    switch (v.kind()) {
        case wd::kind::undefined:
        case wd::kind::date:
        case wd::kind::bytes: break;
        case wd::kind::null: ++s.nulls; break;
        case wd::kind::boolean: ++(v.value_or(false) ? s.trues : s.falses); break;
        case wd::kind::number: ++s.numbers; break;
        case wd::kind::string:
            ++s.strings;
            s.length += string_length(v);
            break;
        case wd::kind::array:
            for (const wd::basic_const_value<Traits, Storage> e : v) {
                count(e, s);
            }
            ++s.arrays;
            s.elements += v.size();
            break;
        case wd::kind::object:
            for (const auto& [key, child] : v.entries()) {
                s.length += key.size();
                count(child, s);
            }
            ++s.objects;
            s.members += v.size();
            s.strings += v.size();
            break;
    }
}

// NOLINTEND(misc-no-recursion)

/** The Statistics of a document, owned or opened in a buffer, as an answer; an error for no
 * document. */
template <class Traits, class Storage>
json::value statistics_of(const std::optional<wd::basic_document<Traits, Storage>>& d) {
    if (!d) {
        return "no document";
    }
    statistics s;
    count(d->root(), s);
    return s.json(unit<Traits>());
}

/** A document parsed from text; nothing when it does not parse. */
template <class Traits>
std::optional<document<Traits>> parsed(std::string_view text) {
    auto d = wd::parse<Traits>(text);
    if (!d.has_value()) {
        return std::nullopt;
    }
    return std::move(d).unsafe_value();
}

/** A new owned document holding an empty object or array; nothing when it cannot be made. */
template <class Traits, class Literal>
std::optional<document<Traits>> holding() {
    auto made = document<Traits>::create();
    if (!made.has_value() || !made->root().set(Literal{}).has_value()) {
        return std::nullopt;
    }
    return std::move(made).unsafe_value();
}

/** 1 when an operation succeeded, for the sink: every result reaches it. */
template <class T>
std::uint64_t succeeded(const wd::result<T>& r) noexcept {
    return r.has_value() ? 1 : 0;
}

/** The largest ring of coordinates of Canada's polygon, the first of the largest. */
template <class Traits>
value<Traits> largest_ring(document<Traits>& canada) {
    value<Traits> rings = canada["features"][std::uint64_t{0}]["geometry"]["coordinates"];
    std::uint64_t largest = 0;
    for (std::uint64_t i = 1; i < rings.size(); ++i) {
        if (rings[i].size() > rings[largest].size()) {
            largest = i;
        }
    }
    return rings[largest];
}

/**
 The datasets as the document holds them, read and built untimed, and handles into them, which the
 constructor makes: assigning a handle writes into the document, as `v = x` does in JavaScript.
*/
template <class Traits>
struct state {
    state(document<Traits> canada_document, document<Traits> citm_document,
          document<Traits> twitter_document)
        : canada(std::move(canada_document)),
          citm(std::move(citm_document)),
          twitter(std::move(twitter_document)),
          ring(largest_ring(canada)),
          statuses(twitter["statuses"]) {
        if constexpr (!utf16<Traits>) {
            find_name = std::as_const(statuses)
                            .at(-1)["user"]["screen_name"]
                            .template value_or<std::string_view>({});
        }
    }

    document<Traits> canada;
    document<Traits> citm;
    document<Traits> twitter;

    /** Canada's largest ring of coordinates. */
    value<Traits> ring;

    /** twitter.json's statuses. */
    value<Traits> statuses;

    /** The screen name of the last status, which find looks for. */
    std::string find_name;
};

/** `parse`: JSON text into a new document. */
template <class Traits>
benchmark parse(const std::string& name, const std::string& text) {
    const auto last = std::make_shared<std::optional<document<Traits>>>();
    return {
        .op = "parse",
        .dataset = name,
        .unit = "document",
        .run =
            [&text, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = parsed<Traits>(text);
                    sum += *last ? (*last)->root().size() : 0;
                }
                return sum;
            },
        .answer = [last] { return statistics_of(*last); },
    };
}

/** `stringify` and `prettify`: a document into compact text, or into text indented by 4. */
template <class Traits>
benchmark stringify(const std::string& name, const document<Traits>& d, bool indented) {
    const auto last = std::make_shared<std::string>();
    return {
        .op = indented ? "prettify" : "stringify",
        .dataset = name,
        .unit = "document",
        .run =
            [&d, last, indented](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = d.stringify(indented ? 4 : 0);
                    sum += last->size();
                }
                return sum;
            },
        .answer = [last] { return statistics_of(parsed<Traits>(*last)); },
    };
}

/** `statistics`: nativejson-benchmark's full traversal. */
template <class Traits>
benchmark traverse(const std::string& name, const document<Traits>& d) {
    const auto last = std::make_shared<statistics>();
    return {
        .op = "statistics",
        .dataset = name,
        .unit = "document",
        .run =
            [&d, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = statistics();
                    count(d.root(), *last);
                    sum += last->sum();
                }
                return sum;
            },
        .answer = [last] { return last->json(unit<Traits>()); },
    };
}

/** `deep_copy`: a document copied whole, by `clone`. */
template <class Traits>
benchmark deep_copy(const std::string& name, const document<Traits>& d) {
    const auto last = std::make_shared<std::optional<document<Traits>>>();
    return {
        .op = "deep_copy",
        .dataset = name,
        .unit = "document",
        .run =
            [&d, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    auto copy = d.clone();
                    *last = std::nullopt;
                    if (copy.has_value()) {
                        *last = std::move(copy).unsafe_value();
                        sum += (*last)->root().size();
                    }
                }
                return sum;
            },
        .answer = [last] { return statistics_of(*last); },
    };
}

/** Frees a buffer that an image was copied into, 16-aligned as an image must be. */
struct aligned_delete {
    void operator()(std::byte* p) const noexcept { ::operator delete(p, std::align_val_t{16}); }
};

/** The buffer of a received image. */
using image_buffer = std::unique_ptr<std::byte, aligned_delete>;

/** A buffer of `size` bytes, 16-aligned. */
inline image_buffer allocate_image(std::size_t size) {
    return image_buffer(static_cast<std::byte*>(::operator new(size, std::align_val_t{16})));
}

/** An image received: the buffer it was copied into, and the document opened in that buffer. */
template <class Traits>
struct received {
    image_buffer buffer;
    std::optional<wd::basic_document<Traits, wd::external>> document;
};

/**
 Receives an image: copies it into a buffer of its own, and opens it there in place, checking its
 seal as `check` says; the document is nothing when the image is refused.
*/
template <class Traits>
received<Traits> receive(std::span<const std::byte> image, wd::verify check) {
    received<Traits> r{allocate_image(image.size()), std::nullopt};
    std::copy(image.begin(), image.end(), r.buffer.get());
    auto opened = wd::open<Traits>(std::span<std::byte>(r.buffer.get(), image.size()), check);
    if (opened.has_value()) {
        r.document = std::move(opened).unsafe_value();
    }
    return r;
}

/** A copy of `d`, sealed, which the receiver checks against its seal; nothing when it cannot be
 * made. */
template <class Traits>
std::shared_ptr<document<Traits>> sealed_copy(const document<Traits>& d) {
    auto copy = d.clone();
    if (!copy.has_value()) {
        return nullptr;
    }
    auto sealed = std::make_shared<document<Traits>>(std::move(copy).unsafe_value());
    if (!sealed->seal().has_value()) {
        return nullptr;
    }
    return sealed;
}

/** `send_receive`: the image copied into the receiver's buffer, and opened there in place. */
template <class Traits>
benchmark send_receive(const std::string& name, const document<Traits>& d) {
    const auto last = std::make_shared<received<Traits>>();
    return {
        .op = "send_receive",
        .dataset = name,
        .unit = "document",
        .run =
            [&d, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    last->document.reset();
                    *last = receive<Traits>(d.image(), wd::verify::seal);
                    sum += last->document ? last->document->root().size() : 0;
                }
                return sum;
            },
        .answer = [last] { return statistics_of(last->document); },
    };
}

/** `send_receive_sealed`: a sealed image copied into the receiver's buffer, and opened there, its
 * seal checked. */
template <class Traits>
benchmark send_receive_sealed(const std::string& name,
                              const std::shared_ptr<document<Traits>>& sealed) {
    const auto last = std::make_shared<received<Traits>>();
    return {
        .op = "send_receive_sealed",
        .dataset = name,
        .unit = "document",
        .run =
            [sealed, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    last->document.reset();
                    *last = receive<Traits>(sealed->image(), wd::verify::seal);
                    sum += last->document ? last->document->root().size() : 0;
                }
                return sum;
            },
        .answer = [last] { return statistics_of(last->document); },
    };
}

/** simdjson's find_tweet: the text of the status of a given id. */
template <class Traits>
benchmark find_tweet(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<std::string_view>();
    return {
        .op = "find_tweet",
        .dataset = "twitter",
        .unit = "query",
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = {};
                    for (const const_value<Traits> tweet : std::as_const(st->statuses)) {
                        if (tweet["id"].template value_or<std::uint64_t>(0) == find_tweet_id) {
                            *last = tweet["text"].template value_or<std::string_view>({});
                            break;
                        }
                    }
                    sum += last->size();
                }
                return sum;
            },
        .answer = [last] { return json::value{{"text", *last}}; },
    };
}

/** simdjson's top_tweet: the most retweeted status of at most 60 retweets, the last of a tie. */
template <class Traits>
benchmark top_tweet(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<json::object>();
    return {
        .op = "top_tweet",
        .dataset = "twitter",
        .unit = "query",
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    std::int64_t best = -1;
                    const_value<Traits> top;
                    for (const const_value<Traits> tweet : std::as_const(st->statuses)) {
                        const auto retweets =
                            tweet["retweet_count"].template value_or<std::int64_t>(-1);
                        if (retweets <= top_tweet_max && retweets >= best) {
                            best = retweets;
                            top = tweet;
                        }
                    }
                    const auto text = top["text"].template value_or<std::string_view>({});
                    const auto screen =
                        top["user"]["screen_name"].template value_or<std::string_view>({});
                    *last = {{"retweet_count", best}, {"screen_name", screen}, {"text", text}};
                    sum += static_cast<std::uint64_t>(best) + text.size() + screen.size();
                }
                return sum;
            },
        .answer = [last] { return json::value(*last); },
    };
}

/** simdjson's distinct_user_id: the ids of the users who posted or were retweeted, sorted once. */
template <class Traits>
benchmark distinct_user_id(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<std::vector<std::uint64_t>>();
    return {
        .op = "distinct_user_id",
        .dataset = "twitter",
        .unit = "query",
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    std::vector<std::uint64_t> ids;
                    for (const const_value<Traits> tweet : std::as_const(st->statuses)) {
                        ids.push_back(tweet["user"]["id"].template value_or<std::uint64_t>(0));
                        if (tweet.contains("retweeted_status")) {
                            ids.push_back(tweet["retweeted_status"]["user"]["id"]
                                              .template value_or<std::uint64_t>(0));
                        }
                    }
                    std::ranges::sort(ids);
                    ids.erase(std::ranges::unique(ids).begin(), ids.end());
                    sum += ids.size();
                    *last = std::move(ids);
                }
                return sum;
            },
        .answer = [last] { return distinct_answer(*last); },
    };
}

/** The value of a dictionary's member as the dictionary benchmarks add it: a number, else 1. */
template <class Traits>
std::int64_t number_or_one(const const_value<Traits>& v) {
    return v.template value_or<std::int64_t>(1);
}

/** One dictionary's objects as the document holds them, and the copies a changing pass works on. */
template <class Traits>
struct dictionary {
    dictionary(const key_set& k, document<Traits> b, document<Traits> t)
        : keys(&k), base(std::move(b)), thinned(std::move(t)) {}

    const key_set* keys;
    document<Traits> base;
    document<Traits> thinned;
    std::vector<document<Traits>> copies;

    /** Gives each of `k` passes a copy of the object, untimed. */
    void copy(std::size_t k) {
        copies.clear();
        for (std::size_t i = 0; i < k; ++i) {
            auto made = base.clone();
            if (made.has_value()) {
                copies.push_back(std::move(made).unsafe_value());
            }
        }
    }

    /** The size of the last copy and the sum of its values. */
    [[nodiscard]] json::value summary() const {
        if (copies.empty()) {
            return "no copy";
        }
        const const_value<Traits> o = copies.back().root();
        std::int64_t total = 0;
        for (const const_value<Traits> v : o.values()) {
            total += number_or_one(v);
        }
        return {{"size", o.size()}, {"sum", total}};
    }

    /** The size of the last copy and its first and last keys, in its order. */
    [[nodiscard]] json::value ends() const {
        if (copies.empty()) {
            return "no copy";
        }
        const const_value<Traits> o = copies.back().root();
        std::string_view first;
        std::string_view last;
        for (const std::string_view key : o.keys()) {
            first = first.empty() ? key : first;
            last = key;
        }
        return {{"size", o.size()}, {"first", first}, {"last", last}};
    }
};

/** Looks every key of the lookups' order up, adding the values of those found. */
template <class Traits>
benchmark lookups(const std::shared_ptr<dictionary<Traits>>& d, bool after_delete) {
    const auto last = std::make_shared<std::pair<std::uint64_t, std::int64_t>>();
    return {
        .op = after_delete ? "lookup_after_delete" : "lookup_hit",
        .dataset = d->keys->name,
        .unit = "lookup",
        .ops = static_cast<double>(d->keys->lookups.size()),
        .run =
            [d, last, after_delete](std::size_t k) {
                const const_value<Traits> o = (after_delete ? d->thinned : d->base).root();
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = {};
                    for (const std::string& key : d->keys->lookups) {
                        const const_value<Traits> v = o[key];
                        if (!v.is_undefined()) {
                            ++last->first;
                            last->second += number_or_one(v);
                        }
                    }
                    sum += last->first;
                }
                return sum;
            },
        .answer = [last] { return found_answer(*last); },
    };
}

/** Looks up keys that are not there, as `key in o` does. */
template <class Traits>
benchmark lookup_miss(const std::shared_ptr<dictionary<Traits>>& d) {
    const auto last = std::make_shared<std::uint64_t>();
    return {
        .op = "lookup_miss",
        .dataset = d->keys->name,
        .unit = "lookup",
        .ops = static_cast<double>(d->keys->misses.size()),
        .run =
            [d, last](std::size_t k) {
                const const_value<Traits> o = d->base.root();
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = 0;
                    for (const std::string& key : d->keys->misses) {
                        *last += o.contains(key) ? 1 : 0;
                    }
                    sum += *last + 1;
                }
                return sum;
            },
        .answer = [last] { return json::value{{"found", *last}}; },
    };
}

/** Inserts every key into an empty object, its index as its value, as `o[key] = i` does. */
template <class Traits>
benchmark insert(const std::shared_ptr<dictionary<Traits>>& d) {
    return {
        .op = "insert",
        .dataset = d->keys->name,
        .unit = "insert",
        .ops = static_cast<double>(d->keys->keys.size()),
        .prepare =
            [d](std::size_t k) {
                d->copies.clear();
                for (std::size_t i = 0; i < k; ++i) {
                    if (auto made = holding<Traits, wd::object>()) {
                        d->copies.push_back(std::move(*made));
                    }
                }
            },
        .chunk = chunk_of(d->keys->keys.size()),
        .run =
            [d](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k && i < d->copies.size(); ++i) {
                    value<Traits> o = d->copies[i].root();
                    for (std::size_t j = 0; j < d->keys->keys.size(); ++j) {
                        sum += succeeded(o[d->keys->keys[j]].set(static_cast<std::int64_t>(j)));
                    }
                }
                return sum;
            },
        .answer = [d] { return d->ends(); },
    };
}

/** Assigns every member twice its index, as `o[key] = 2 * i` does. */
template <class Traits>
benchmark update(const std::shared_ptr<dictionary<Traits>>& d, std::size_t members) {
    return {
        .op = "update",
        .dataset = d->keys->name,
        .unit = "update",
        .ops = static_cast<double>(d->keys->keys.size()),
        .prepare = [d](std::size_t k) { d->copy(k); },
        .chunk = chunk_of(members),
        .run =
            [d](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k && i < d->copies.size(); ++i) {
                    value<Traits> o = d->copies[i].root();
                    for (std::size_t j = 0; j < d->keys->keys.size(); ++j) {
                        const auto twice = 2 * static_cast<std::int64_t>(j);
                        sum += succeeded(o[d->keys->keys[j]].set(twice));
                    }
                }
                return sum;
            },
        .answer = [d] { return d->summary(); },
    };
}

/** Deletes the keys of `deletes`, as `delete o[key]` does. */
template <class Traits>
benchmark erase(const std::shared_ptr<dictionary<Traits>>& d, std::size_t members) {
    return {
        .op = "delete",
        .dataset = d->keys->name,
        .unit = "delete",
        .ops = static_cast<double>(d->keys->deletes.size()),
        .prepare = [d](std::size_t k) { d->copy(k); },
        .chunk = chunk_of(members),
        .run =
            [d](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k && i < d->copies.size(); ++i) {
                    value<Traits> o = d->copies[i].root();
                    for (const std::string& key : d->keys->deletes) {
                        sum += succeeded(o.erase(key));
                    }
                }
                return sum;
            },
        .answer = [d] { return d->ends(); },
    };
}

/** Reads every member's key and value, in the object's order. */
template <class Traits>
benchmark iterate(const std::shared_ptr<dictionary<Traits>>& d) {
    const auto last = std::make_shared<std::pair<std::uint64_t, std::int64_t>>();
    return {
        .op = "iterate",
        .dataset = d->keys->name,
        .unit = "member",
        .ops = static_cast<double>(d->keys->keys.size()),
        .run =
            [d, last](std::size_t k) {
                const const_value<Traits> o = d->base.root();
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = {};
                    for (const auto& [key, v] : o.entries()) {
                        ++last->first;
                        last->second += static_cast<std::int64_t>(key.size()) + number_or_one(v);
                    }
                    sum += last->first;
                }
                return sum;
            },
        .answer = [last] { return iterate_answer(*last); },
    };
}

/** The dictionary of one key set: its object built, and a copy with `deletes` deleted. */
template <class Traits>
std::shared_ptr<dictionary<Traits>> make_dictionary(const key_set& ks, const state<Traits>& st) {
    auto base = holding<Traits, wd::object>();
    if (!base) {
        return nullptr;
    }
    if (ks.name == "citm.events") {
        if (!base->root().set(std::as_const(st.citm)["events"]).has_value()) {
            return nullptr;
        }
    } else {
        value<Traits> o = base->root();
        for (std::size_t j = 0; j < ks.keys.size(); ++j) {
            if (!o[ks.keys[j]].set(static_cast<std::int64_t>(j)).has_value()) {
                return nullptr;
            }
        }
    }
    auto thinned = base->clone();
    if (!thinned.has_value()) {
        return nullptr;
    }
    for (const std::string& key : ks.deletes) {
        if (!thinned->root().erase(key).has_value()) {
            return nullptr;
        }
    }
    return std::make_shared<dictionary<Traits>>(ks, std::move(*base),
                                                std::move(thinned).unsafe_value());
}

/** The dictionary benchmarks of one object: lookups, insert, update, delete, iteration. */
template <class Traits>
void add_dictionary(std::vector<benchmark>& out, const std::shared_ptr<dictionary<Traits>>& d) {
    const bool synthetic = d->keys->name != "citm.events";
    // A copy of citm's events holds about 2,048 values, a synthetic object one per member.
    const std::size_t members = synthetic ? d->keys->keys.size() : 2048;
    out.push_back(lookups(d, false));
    out.push_back(lookup_miss(d));
    if (synthetic) {
        out.push_back(insert(d));
    }
    out.push_back(update(d, members));
    out.push_back(erase(d, members));
    out.push_back(lookups(d, true));
    out.push_back(iterate(d));
}

/** push and pop at the end of an array, `push_pop_count` of each in a pass. */
template <class Traits>
benchmark push_pop(const std::shared_ptr<state<Traits>>& st, bool ring) {
    const auto last = std::make_shared<std::pair<std::uint64_t, std::uint64_t>>();
    return {
        .op = "push_pop",
        .dataset = ring ? "canada.ring" : "twitter.statuses",
        .unit = "operation",
        .ops = 2.0 * push_pop_count,
        .run =
            [st, last, ring](std::size_t k) {
                value<Traits>& a = ring ? st->ring : st->statuses;
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    for (std::size_t j = 0; j < push_pop_count; ++j) {
                        sum += succeeded(a.push(static_cast<std::int64_t>(j)));
                    }
                    last->first = a.size();
                    for (std::size_t j = 0; j < push_pop_count; ++j) {
                        sum += succeeded(a.pop());
                    }
                    last->second = a.size();
                }
                return sum;
            },
        .answer = [last] { return json::value{{"pushed", last->first}, {"popped", last->second}}; },
    };
}

/** splice in the middle of the ring, one element in, then out. */
template <class Traits>
benchmark splice(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<std::pair<std::uint64_t, std::uint64_t>>();
    return {
        .op = "splice",
        .dataset = "canada.ring",
        .unit = "operation",
        .ops = 2,
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    const auto middle = static_cast<std::int64_t>(st->ring.size() / 2);
                    sum += succeeded(st->ring.splice(middle, 0, std::int64_t{0}));
                    last->first = st->ring.size();
                    sum += succeeded(st->ring.splice(middle, 1));
                    last->second = st->ring.size();
                }
                return sum;
            },
        .answer =
            [last] { return json::value{{"inserted", last->first}, {"removed", last->second}}; },
    };
}

/** The length of the array a document holds, as an answer. */
template <class Traits>
json::value length_of(const std::optional<document<Traits>>& d) {
    if (!d) {
        return "no document";
    }
    return {{"length", d->root().size()}};
}

/** map: the ring's longitudes, by hand into a new document. */
template <class Traits>
benchmark map(const std::shared_ptr<state<Traits>>& st) {
    using number = Traits::number;
    const auto last = std::make_shared<std::optional<document<Traits>>>();
    return {
        .op = "map",
        .dataset = "canada.ring",
        .unit = "element",
        .ops = static_cast<double>(st->ring.size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = holding<Traits, wd::array>();
                    if (!*last) {
                        return sum;
                    }
                    value<Traits> o = (*last)->root();
                    for (const const_value<Traits> point : std::as_const(st->ring)) {
                        sum += succeeded(o.push(point[std::uint64_t{0}]));
                    }
                }
                return sum;
            },
        .answer =
            [last] {
                if (!*last) {
                    return json::value("no document");
                }
                const const_value<Traits> o = std::as_const(**last).root();
                std::uint64_t negatives = 0;
                for (const const_value<Traits> x : o) {
                    negatives += x.template value_or<number>(number{0}) < number{0} ? 1 : 0;
                }
                return json::value{{"length", o.size()}, {"negatives", negatives}};
            },
    };
}

/** filter: the ring's points west of 100 degrees, copied by hand into a new document. */
template <class Traits>
benchmark filter(const std::shared_ptr<state<Traits>>& st) {
    using number = Traits::number;
    const auto last = std::make_shared<std::optional<document<Traits>>>();
    return {
        .op = "filter",
        .dataset = "canada.ring",
        .unit = "element",
        .ops = static_cast<double>(st->ring.size()),
        .run =
            [st, last](std::size_t k) {
                const number bound{-100};
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = holding<Traits, wd::array>();
                    if (!*last) {
                        return sum;
                    }
                    value<Traits> o = (*last)->root();
                    for (const const_value<Traits> point : std::as_const(st->ring)) {
                        if (point[std::uint64_t{0}].template value_or<number>(bound) < bound) {
                            sum += succeeded(o.push(point));
                        }
                    }
                }
                return sum;
            },
        .answer = [last] { return length_of(*last); },
    };
}

/** reduce: the statuses' retweets added up, by hand. */
template <class Traits>
benchmark reduce(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<std::int64_t>();
    return {
        .op = "reduce",
        .dataset = "twitter.statuses",
        .unit = "element",
        .ops = static_cast<double>(st->statuses.size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = 0;
                    for (const const_value<Traits> tweet : std::as_const(st->statuses)) {
                        *last += tweet["retweet_count"].template value_or<std::int64_t>(0);
                    }
                    sum += static_cast<std::uint64_t>(*last);
                }
                return sum;
            },
        .answer = [last] { return json::value{{"sum", *last}}; },
    };
}

/** find: the first status of the last status's user, by hand. */
template <class Traits>
benchmark find(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<std::pair<std::uint64_t, std::string_view>>();
    return {
        .op = "find",
        .dataset = "twitter.statuses",
        .unit = "element",
        .ops = static_cast<double>(st->statuses.size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = {st->statuses.size(), {}};
                    std::uint64_t index = 0;
                    for (const const_value<Traits> tweet : std::as_const(st->statuses)) {
                        const auto name =
                            tweet["user"]["screen_name"].template value_or<std::string_view>({});
                        if (name == st->find_name) {
                            const auto id = tweet["id_str"].template value_or<std::string_view>({});
                            *last = {index, id};
                            break;
                        }
                        ++index;
                    }
                    sum += last->first;
                }
                return sum;
            },
        .answer = [last] { return json::value{{"index", last->first}, {"id_str", last->second}}; },
    };
}

/** slice: the middle half of the ring, copied into a new document by `slice`. */
template <class Traits>
benchmark slice(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<std::optional<document<Traits>>>();
    const auto from = static_cast<std::int64_t>(st->ring.size() / 4);
    const auto to = static_cast<std::int64_t>(st->ring.size() * 3 / 4);
    return {
        .op = "slice",
        .dataset = "canada.ring",
        .unit = "element",
        .ops = static_cast<double>(to - from),
        .run =
            [st, last, from, to](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    auto part = std::as_const(st->ring).slice(from, to);
                    *last = std::nullopt;
                    if (part.has_value()) {
                        *last = std::move(part).unsafe_value();
                        sum += (*last)->root().size();
                    }
                }
                return sum;
            },
        .answer = [last] { return length_of(*last); },
    };
}

/** join: the statuses' id_str joined by commas, by hand. */
template <class Traits>
benchmark join(const std::shared_ptr<state<Traits>>& st) {
    const auto last = std::make_shared<std::string>();
    return {
        .op = "join",
        .dataset = "twitter.statuses",
        .unit = "element",
        .ops = static_cast<double>(st->statuses.size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    std::string joined;
                    for (const const_value<Traits> tweet : std::as_const(st->statuses)) {
                        if (!joined.empty()) {
                            joined += ',';
                        }
                        joined += tweet["id_str"].template value_or<std::string_view>({});
                    }
                    sum += joined.size();
                    *last = std::move(joined);
                }
                return sum;
            },
        .answer = [last] { return json::value{{"length", last->size()}, {"fnv", fnv(*last)}}; },
    };
}

/** The datasets read, Canada's largest ring and the user find looks for found, untimed. */
template <class Traits>
std::shared_ptr<state<Traits>> load(const texts& t) {
    auto canada = parsed<Traits>(t.canada);
    auto citm = parsed<Traits>(t.citm);
    auto twitter = parsed<Traits>(t.twitter);
    if (!canada || !citm || !twitter) {
        return nullptr;
    }
    return std::make_shared<state<Traits>>(std::move(*canada), std::move(*citm),
                                           std::move(*twitter));
}

/** The parse and stringify benchmarks of the three files, and when `whole` the others too. */
template <class Traits>
void add_documents(std::vector<benchmark>& out, const texts& t, const state<Traits>& st,
                   bool whole) {
    const auto files = std::to_array<std::pair<std::string, const std::string*>>({
        {"canada", &t.canada},
        {"citm", &t.citm},
        {"twitter", &t.twitter},
    });
    const auto documents =
        std::to_array<const document<Traits>*>({&st.canada, &st.citm, &st.twitter});
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& [name, text] = files.at(i);
        const document<Traits>& d = *documents.at(i);
        out.push_back(parse<Traits>(name, *text));
        out.push_back(stringify<Traits>(name, d, false));
        if (whole) {
            out.push_back(stringify<Traits>(name, d, true));
            out.push_back(traverse<Traits>(name, d));
            out.push_back(deep_copy<Traits>(name, d));
            out.push_back(send_receive<Traits>(name, d));
            if (const std::shared_ptr<document<Traits>> sealed = sealed_copy<Traits>(d)) {
                out.push_back(send_receive_sealed<Traits>(name, sealed));
            }
        }
    }
}

/** Every array benchmark: push and pop, splice, and the array methods. */
template <class Traits>
void add_arrays(std::vector<benchmark>& out, const std::shared_ptr<state<Traits>>& st) {
    out.push_back(push_pop(st, false));
    out.push_back(push_pop(st, true));
    out.push_back(splice(st));
    out.push_back(map(st));
    out.push_back(filter(st));
    out.push_back(reduce(st));
    out.push_back(find(st));
    out.push_back(slice(st));
    out.push_back(join(st));
}

/**
 Every benchmark of the document under `Traits` over the texts and the dictionaries' keys; only
 parse and stringify for `js_traits`, whose rows the page shows beside the default traits'.
*/
template <class Traits>
std::vector<benchmark> benchmarks(const texts& t, std::span<const key_set> dictionaries) {
    const std::shared_ptr<state<Traits>> st = load<Traits>(t);
    std::vector<benchmark> out;
    if (!st) {
        return out;
    }
    add_documents<Traits>(out, t, *st, !utf16<Traits>);
    if constexpr (!utf16<Traits>) {
        out.push_back(find_tweet(st));
        out.push_back(top_tweet(st));
        out.push_back(distinct_user_id(st));
        for (const key_set& ks : dictionaries) {
            const auto d = make_dictionary(ks, *st);
            if (!d) {
                return {};
            }
            add_dictionary(out, d);
        }
        add_arrays(out, st);
    }
    // Each benchmark keeps the state that its references point into alive.
    for (benchmark& b : out) {
        b.run = [run = std::move(b.run), st](std::size_t k) { return run(k); };
    }
    return out;
}

}  // namespace webcpp::bench::document_bench

#endif  // WEBCPP_DOCUMENT_BENCH_DOCUMENT_HPP
