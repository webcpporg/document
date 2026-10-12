// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The benchmarks of Boost.JSON: each operation of the page's section "Benchmarks" written as a user
 of `boost::json::value` writes it, and, where Boost.JSON has no such operation, as close to it as
 its interface goes (a pretty printer, splice, the array methods), which the table marks.
*/

#ifndef WEBCPP_DOCUMENT_BENCH_BOOST_JSON_HPP
#define WEBCPP_DOCUMENT_BENCH_BOOST_JSON_HPP

#include "bench.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::bench::boost_json {

namespace json = boost::json;

// NOLINTBEGIN(misc-no-recursion): a value is counted by counting its children, as nativejson's
// GenStat does.
/** Adds what `v` holds to `s`, as nativejson-benchmark's GenStat counts it. */
inline void count(const json::value& v, statistics& s) {
    switch (v.kind()) {
        case json::kind::null: ++s.nulls; break;
        case json::kind::bool_: ++(v.get_bool() ? s.trues : s.falses); break;
        case json::kind::int64:
        case json::kind::uint64:
        case json::kind::double_: ++s.numbers; break;
        case json::kind::string:
            ++s.strings;
            s.length += v.get_string().size();
            break;
        case json::kind::array:
            for (const json::value& e : v.get_array()) {
                count(e, s);
            }
            ++s.arrays;
            s.elements += v.get_array().size();
            break;
        case json::kind::object:
            for (const json::key_value_pair& m : v.get_object()) {
                s.length += m.key().size();
                count(m.value(), s);
            }
            ++s.objects;
            s.members += v.get_object().size();
            s.strings += v.get_object().size();
            break;
    }
}

// NOLINTEND(misc-no-recursion)

/** The Statistics of `v`, as an answer. */
inline json::value statistics_of(const json::value& v) {
    statistics s;
    count(v, s);
    return s.json("utf8");
}

/** A value parsed from text; null when it does not parse. */
inline json::value parsed(std::string_view text) {
    boost::system::error_code ec;
    json::value v = json::parse(text, ec);
    return ec ? json::value() : std::move(v);
}

/** Appends what a serializer reset to one value writes. */
inline void drain(json::serializer& sr, std::string& out) {
    std::array<char, 256> buffer{};
    while (!sr.done()) {
        out.append(sr.read(buffer.data(), buffer.size()));
    }
}

/** A line break, then the indentation of `level`, four spaces a level. */
inline void indent(std::string& out, std::size_t level) {
    out += '\n';
    out.append(level * 4, ' ');
}

// NOLINTBEGIN(misc-no-recursion): a value is written by writing its children.
/**
 Writes `v` as `JSON.stringify(v, null, 4)` lays it out, each scalar and key by Boost.JSON's own
 serializer: Boost.JSON has no pretty printer, and its example pretty.cpp writes one this way.
*/
inline void pretty(const json::value& v, json::serializer& sr, std::string& out,
                   std::size_t depth) {
    if (const json::object* o = v.if_object(); o != nullptr && !o->empty()) {
        out += '{';
        for (const auto* m = o->begin(); m != o->end(); ++m) {
            out += m == o->begin() ? "" : ",";
            indent(out, depth + 1);
            sr.reset(m->key());
            drain(sr, out);
            out += ": ";
            pretty(m->value(), sr, out, depth + 1);
        }
        indent(out, depth);
        out += '}';
        return;
    }
    if (const json::array* a = v.if_array(); a != nullptr && !a->empty()) {
        out += '[';
        for (const auto* e = a->begin(); e != a->end(); ++e) {
            out += e == a->begin() ? "" : ",";
            indent(out, depth + 1);
            pretty(*e, sr, out, depth + 1);
        }
        indent(out, depth);
        out += ']';
        return;
    }
    sr.reset(&v);
    drain(sr, out);
}

// NOLINTEND(misc-no-recursion)

/** The value of a dictionary's member as the dictionary benchmarks add it: a number, else 1. */
inline std::int64_t number_or_one(const json::value& v) {
    return v.is_int64() ? v.get_int64() : 1;
}

/** The datasets as Boost.JSON holds them, read and built untimed. */
struct state {
    json::value canada;
    json::value citm;
    json::value twitter;

    /** Canada's largest ring of coordinates. */
    json::array* ring = nullptr;

    /** twitter.json's statuses. */
    json::array* statuses = nullptr;

    /** The screen name of the last status, which find looks for. */
    std::string find_name;
};

/** `parse`: JSON text into a value. */
inline benchmark parse(const std::string& name, const std::string& text) {
    const auto last = std::make_shared<json::value>();
    return {
        .op = "parse",
        .dataset = name,
        .unit = "document",
        .run =
            [&text, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = parsed(text);
                    sum += last->is_object() ? last->get_object().size() : 0;
                }
                return sum;
            },
        .answer = [last] { return statistics_of(*last); },
    };
}

/** `stringify` and `prettify`: a value into compact text, or into text indented by 4. */
inline benchmark stringify(const std::string& name, const json::value& v, bool indented) {
    const auto last = std::make_shared<std::string>();
    return {
        .op = indented ? "prettify" : "stringify",
        .dataset = name,
        .unit = "document",
        .run =
            [&v, last, indented](std::size_t k) {
                std::uint64_t sum = 0;
                json::serializer sr;
                for (std::size_t i = 0; i < k; ++i) {
                    std::string text;
                    if (indented) {
                        pretty(v, sr, text, 0);
                    } else {
                        text = json::serialize(v);
                    }
                    sum += text.size();
                    *last = std::move(text);
                }
                return sum;
            },
        .answer = [last] { return statistics_of(parsed(*last)); },
    };
}

/** `statistics`: nativejson-benchmark's full traversal. */
inline benchmark traverse(const std::string& name, const json::value& v) {
    const auto last = std::make_shared<statistics>();
    return {
        .op = "statistics",
        .dataset = name,
        .unit = "document",
        .run =
            [&v, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = statistics();
                    count(v, *last);
                    sum += last->sum();
                }
                return sum;
            },
        .answer = [last] { return last->json("utf8"); },
    };
}

/** `deep_copy`: a value copied whole, by its copy constructor. */
inline benchmark deep_copy(const std::string& name, const json::value& v) {
    const auto last = std::make_shared<json::value>();
    return {
        .op = "deep_copy",
        .dataset = name,
        .unit = "document",
        .run =
            [&v, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = json::value(v);
                    sum += last->is_object() ? last->get_object().size() : 0;
                }
                return sum;
            },
        .answer = [last] { return statistics_of(*last); },
    };
}

/** `send_receive`: a value serialized to text, which the receiver parses. */
inline benchmark send_receive(const std::string& name, const json::value& v) {
    const auto last = std::make_shared<json::value>();
    return {
        .op = "send_receive",
        .dataset = name,
        .unit = "document",
        .run =
            [&v, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = parsed(json::serialize(v));
                    sum += last->is_object() ? last->get_object().size() : 0;
                }
                return sum;
            },
        .answer = [last] { return statistics_of(*last); },
    };
}

/** simdjson's find_tweet: the text of the status of a given id. */
inline benchmark find_tweet(const std::shared_ptr<state>& st) {
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
                    for (const json::value& tweet : *st->statuses) {
                        if (tweet.at("id") == find_tweet_id) {
                            *last = tweet.at("text").as_string();
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
inline benchmark top_tweet(const std::shared_ptr<state>& st) {
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
                    const json::value* top = &st->statuses->front();
                    for (const json::value& tweet : *st->statuses) {
                        const std::int64_t retweets = tweet.at("retweet_count").as_int64();
                        if (retweets <= top_tweet_max && retweets >= best) {
                            best = retweets;
                            top = &tweet;
                        }
                    }
                    const json::string& text = top->at("text").as_string();
                    const json::string& screen = top->at("user").at("screen_name").as_string();
                    *last = {{"retweet_count", best}, {"screen_name", screen}, {"text", text}};
                    sum += static_cast<std::uint64_t>(best) + text.size() + screen.size();
                }
                return sum;
            },
        .answer = [last] { return json::value(*last); },
    };
}

/** simdjson's distinct_user_id: the ids of the users who posted or were retweeted, sorted once. */
inline benchmark distinct_user_id(const std::shared_ptr<state>& st) {
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
                    for (const json::value& tweet : *st->statuses) {
                        ids.push_back(tweet.at("user").at("id").to_number<std::uint64_t>());
                        if (const json::value* retweeted =
                                tweet.as_object().if_contains("retweeted_status")) {
                            ids.push_back(
                                retweeted->at("user").at("id").to_number<std::uint64_t>());
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

/** One dictionary's objects as Boost.JSON holds them, and the copies a changing pass works on. */
struct dictionary {
    explicit dictionary(const key_set& k) : keys(&k) {}

    const key_set* keys;
    json::object base;
    json::object thinned;
    std::vector<json::object> copies;

    /** Gives each of `k` passes a copy of the object, untimed. */
    void copy(std::size_t k) { copies.assign(k, base); }

    /** The size of the last copy and the sum of its values. */
    [[nodiscard]] json::value summary() const {
        std::int64_t total = 0;
        for (const json::key_value_pair& m : copies.back()) {
            total += number_or_one(m.value());
        }
        return {{"size", copies.back().size()}, {"sum", total}};
    }

    /** The size of the last copy and its first and last keys, in its order. */
    [[nodiscard]] json::value ends() const {
        const json::object& o = copies.back();
        return {
            {"size", o.size()},
            {"first", o.begin()->key()},
            {"last", std::prev(o.end())->key()},
        };
    }
};

/** Looks every key of the lookups' order up, adding the values of those found. */
inline benchmark lookups(const std::shared_ptr<dictionary>& d, bool after_delete) {
    const auto last = std::make_shared<std::pair<std::uint64_t, std::int64_t>>();
    return {
        .op = after_delete ? "lookup_after_delete" : "lookup_hit",
        .dataset = d->keys->name,
        .unit = "lookup",
        .ops = static_cast<double>(d->keys->lookups.size()),
        .run =
            [d, last, after_delete](std::size_t k) {
                const json::object& o = after_delete ? d->thinned : d->base;
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = {};
                    for (const std::string& key : d->keys->lookups) {
                        if (const json::value* v = o.if_contains(key)) {
                            ++last->first;
                            last->second += number_or_one(*v);
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
inline benchmark lookup_miss(const std::shared_ptr<dictionary>& d) {
    const auto last = std::make_shared<std::uint64_t>();
    return {
        .op = "lookup_miss",
        .dataset = d->keys->name,
        .unit = "lookup",
        .ops = static_cast<double>(d->keys->misses.size()),
        .run =
            [d, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = 0;
                    for (const std::string& key : d->keys->misses) {
                        *last += d->base.contains(key) ? 1 : 0;
                    }
                    sum += *last + 1;
                }
                return sum;
            },
        .answer = [last] { return json::value{{"found", *last}}; },
    };
}

/** Inserts every key into an empty object, its index as its value, as `o[key] = i` does. */
inline benchmark insert(const std::shared_ptr<dictionary>& d) {
    return {
        .op = "insert",
        .dataset = d->keys->name,
        .unit = "insert",
        .ops = static_cast<double>(d->keys->keys.size()),
        .prepare = [d](std::size_t k) { d->copies.assign(k, json::object()); },
        .chunk = chunk_of(d->keys->keys.size()),
        .run =
            [d](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    json::object& o = d->copies[i];
                    for (std::size_t j = 0; j < d->keys->keys.size(); ++j) {
                        o.insert_or_assign(d->keys->keys[j], static_cast<std::int64_t>(j));
                    }
                    sum += o.size();
                }
                return sum;
            },
        .answer = [d] { return d->ends(); },
    };
}

/** Assigns every member twice its index, as `o[key] = 2 * i` does. */
inline benchmark update(const std::shared_ptr<dictionary>& d, std::size_t members) {
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
                for (std::size_t i = 0; i < k; ++i) {
                    json::object& o = d->copies[i];
                    for (std::size_t j = 0; j < d->keys->keys.size(); ++j) {
                        o.insert_or_assign(d->keys->keys[j], 2 * static_cast<std::int64_t>(j));
                    }
                    sum += o.size();
                }
                return sum;
            },
        .answer = [d] { return d->summary(); },
    };
}

/** Deletes the keys of `deletes`, keeping the order of the rest, as `delete o[key]` does. */
inline benchmark erase(const std::shared_ptr<dictionary>& d, std::size_t members) {
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
                for (std::size_t i = 0; i < k; ++i) {
                    for (const std::string& key : d->keys->deletes) {
                        // stable_erase keeps the order of the members left; erase would not.
                        sum += d->copies[i].stable_erase(key);
                    }
                }
                return sum;
            },
        .answer = [d] { return d->ends(); },
    };
}

/** Reads every member's key and value, in the object's order. */
inline benchmark iterate(const std::shared_ptr<dictionary>& d) {
    const auto last = std::make_shared<std::pair<std::uint64_t, std::int64_t>>();
    return {
        .op = "iterate",
        .dataset = d->keys->name,
        .unit = "member",
        .ops = static_cast<double>(d->keys->keys.size()),
        .run =
            [d, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = {};
                    for (const json::key_value_pair& m : d->base) {
                        ++last->first;
                        last->second +=
                            static_cast<std::int64_t>(m.key().size()) + number_or_one(m.value());
                    }
                    sum += last->first;
                }
                return sum;
            },
        .answer = [last] { return iterate_answer(*last); },
    };
}

/** The dictionary benchmarks of one object: lookups, insert, update, delete, iteration. */
inline void add_dictionary(std::vector<benchmark>& out, const std::shared_ptr<dictionary>& d,
                           bool synthetic) {
    // A copy of citm's events holds about 2,048 values, a synthetic object one per member.
    const std::size_t members = synthetic ? d->keys->keys.size() : 2048;
    d->thinned = d->base;
    for (const std::string& key : d->keys->deletes) {
        d->thinned.stable_erase(key);
    }
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
inline benchmark push_pop(const std::string& dataset, json::array* a) {
    const auto last = std::make_shared<std::pair<std::size_t, std::size_t>>();
    return {
        .op = "push_pop",
        .dataset = dataset,
        .unit = "operation",
        .ops = 2.0 * push_pop_count,
        .run =
            [a, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    for (std::size_t j = 0; j < push_pop_count; ++j) {
                        a->push_back(static_cast<std::int64_t>(j));
                    }
                    last->first = a->size();
                    for (std::size_t j = 0; j < push_pop_count; ++j) {
                        a->pop_back();
                    }
                    last->second = a->size();
                    sum += last->first + last->second;
                }
                return sum;
            },
        .answer = [last] { return json::value{{"pushed", last->first}, {"popped", last->second}}; },
    };
}

/** splice in the middle of the ring, one element in and out: insert and erase, its halves. */
inline benchmark splice(const std::shared_ptr<state>& st) {
    const auto last = std::make_shared<std::pair<std::size_t, std::size_t>>();
    return {
        .op = "splice",
        .dataset = "canada.ring",
        .unit = "operation",
        .ops = 2,
        .run =
            [st, last](std::size_t k) {
                json::array& ring = *st->ring;
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    const auto middle = static_cast<std::ptrdiff_t>(ring.size() / 2);
                    ring.insert(ring.begin() + middle, std::int64_t{0});
                    last->first = ring.size();
                    ring.erase(ring.begin() + middle);
                    last->second = ring.size();
                    sum += last->first + last->second;
                }
                return sum;
            },
        .answer =
            [last] { return json::value{{"inserted", last->first}, {"removed", last->second}}; },
    };
}

/** map: the ring's longitudes, by hand into a new array. */
inline benchmark map(const std::shared_ptr<state>& st) {
    const auto last = std::make_shared<json::array>();
    return {
        .op = "map",
        .dataset = "canada.ring",
        .unit = "element",
        .ops = static_cast<double>(st->ring->size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    json::array mapped;
                    mapped.reserve(st->ring->size());
                    for (const json::value& point : *st->ring) {
                        mapped.push_back(point.as_array()[0]);
                    }
                    *last = std::move(mapped);
                    sum += last->size();
                }
                return sum;
            },
        .answer =
            [last] {
                std::uint64_t negatives = 0;
                for (const json::value& x : *last) {
                    negatives += x.to_number<double>() < 0 ? 1 : 0;
                }
                return json::value{{"length", last->size()}, {"negatives", negatives}};
            },
    };
}

/** filter: the ring's points west of 100 degrees, copied by hand into a new array. */
inline benchmark filter(const std::shared_ptr<state>& st) {
    const auto last = std::make_shared<json::array>();
    return {
        .op = "filter",
        .dataset = "canada.ring",
        .unit = "element",
        .ops = static_cast<double>(st->ring->size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    json::array kept;
                    for (const json::value& point : *st->ring) {
                        if (point.as_array()[0].to_number<double>() < -100) {
                            kept.push_back(point);
                        }
                    }
                    *last = std::move(kept);
                    sum += last->size();
                }
                return sum;
            },
        .answer = [last] { return json::value{{"length", last->size()}}; },
    };
}

/** reduce: the statuses' retweets added up, by hand. */
inline benchmark reduce(const std::shared_ptr<state>& st) {
    const auto last = std::make_shared<std::int64_t>();
    return {
        .op = "reduce",
        .dataset = "twitter.statuses",
        .unit = "element",
        .ops = static_cast<double>(st->statuses->size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = 0;
                    for (const json::value& tweet : *st->statuses) {
                        *last += tweet.at("retweet_count").as_int64();
                    }
                    sum += static_cast<std::uint64_t>(*last);
                }
                return sum;
            },
        .answer = [last] { return json::value{{"sum", *last}}; },
    };
}

/** find: the first status of the last status's user, by hand. */
inline benchmark find(const std::shared_ptr<state>& st) {
    const auto last = std::make_shared<std::pair<std::size_t, std::string_view>>();
    return {
        .op = "find",
        .dataset = "twitter.statuses",
        .unit = "element",
        .ops = static_cast<double>(st->statuses->size()),
        .run =
            [st, last](std::size_t k) {
                const json::array& statuses = *st->statuses;
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = {statuses.size(), {}};
                    for (std::size_t j = 0; j < statuses.size(); ++j) {
                        const json::value& user = statuses[j].at("user");
                        if (user.at("screen_name").as_string() == st->find_name) {
                            *last = {j, statuses[j].at("id_str").as_string()};
                            break;
                        }
                    }
                    sum += last->first;
                }
                return sum;
            },
        .answer = [last] { return json::value{{"index", last->first}, {"id_str", last->second}}; },
    };
}

/** slice: the middle half of the ring, copied by the array's range constructor. */
inline benchmark slice(const std::shared_ptr<state>& st) {
    const auto last = std::make_shared<json::array>();
    const std::size_t from = st->ring->size() / 4;
    const std::size_t to = st->ring->size() * 3 / 4;
    return {
        .op = "slice",
        .dataset = "canada.ring",
        .unit = "element",
        .ops = static_cast<double>(to - from),
        .run =
            [st, last, from, to](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    *last = json::array(st->ring->begin() + static_cast<std::ptrdiff_t>(from),
                                        st->ring->begin() + static_cast<std::ptrdiff_t>(to));
                    sum += last->size();
                }
                return sum;
            },
        .answer = [last] { return json::value{{"length", last->size()}}; },
    };
}

/** join: the statuses' id_str joined by commas, by hand. */
inline benchmark join(const std::shared_ptr<state>& st) {
    const auto last = std::make_shared<std::string>();
    return {
        .op = "join",
        .dataset = "twitter.statuses",
        .unit = "element",
        .ops = static_cast<double>(st->statuses->size()),
        .run =
            [st, last](std::size_t k) {
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < k; ++i) {
                    std::string joined;
                    for (const json::value& tweet : *st->statuses) {
                        if (!joined.empty()) {
                            joined += ',';
                        }
                        joined += tweet.at("id_str").as_string();
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
inline std::shared_ptr<state> load(const texts& t) {
    const auto st = std::make_shared<state>();
    st->canada = parsed(t.canada);
    st->citm = parsed(t.citm);
    st->twitter = parsed(t.twitter);
    json::value& geometry = st->canada.at("features").as_array()[0].at("geometry");
    json::array& rings = geometry.at("coordinates").as_array();
    st->ring = &rings[0].as_array();
    for (json::value& ring : rings) {
        if (ring.as_array().size() > st->ring->size()) {
            st->ring = &ring.as_array();
        }
    }
    st->statuses = &st->twitter.at("statuses").as_array();
    const json::value& last_user = st->statuses->back().at("user");
    st->find_name = last_user.at("screen_name").as_string();
    return st;
}

/** Every benchmark of Boost.JSON over the texts and the dictionaries' keys. */
inline std::vector<benchmark> benchmarks(const texts& t, std::span<const key_set> dictionaries) {
    const std::shared_ptr<state> st = load(t);
    std::vector<benchmark> out;
    const auto files = std::to_array<std::pair<std::string, const std::string*>>({
        {"canada", &t.canada},
        {"citm", &t.citm},
        {"twitter", &t.twitter},
    });
    const auto values = std::to_array<const json::value*>({&st->canada, &st->citm, &st->twitter});
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& [name, text] = files.at(i);
        out.push_back(parse(name, *text));
        out.push_back(stringify(name, *values.at(i), false));
        out.push_back(stringify(name, *values.at(i), true));
        out.push_back(traverse(name, *values.at(i)));
        out.push_back(deep_copy(name, *values.at(i)));
        out.push_back(send_receive(name, *values.at(i)));
    }
    out.push_back(find_tweet(st));
    out.push_back(top_tweet(st));
    out.push_back(distinct_user_id(st));
    for (const key_set& ks : dictionaries) {
        const auto d = std::make_shared<dictionary>(ks);
        const bool synthetic = ks.name != "citm.events";
        if (synthetic) {
            for (std::size_t j = 0; j < ks.keys.size(); ++j) {
                d->base.insert_or_assign(ks.keys[j], static_cast<std::int64_t>(j));
            }
        } else {
            d->base = st->citm.at("events").as_object();
        }
        add_dictionary(out, d, synthetic);
    }
    out.push_back(push_pop("twitter.statuses", st->statuses));
    out.push_back(push_pop("canada.ring", st->ring));
    out.push_back(splice(st));
    out.push_back(map(st));
    out.push_back(filter(st));
    out.push_back(reduce(st));
    out.push_back(find(st));
    out.push_back(slice(st));
    out.push_back(join(st));
    // Each benchmark keeps the state that its references point into alive.
    for (benchmark& b : out) {
        b.run = [run = std::move(b.run), st](std::size_t k) { return run(k); };
    }
    return out;
}

}  // namespace webcpp::bench::boost_json

#endif  // WEBCPP_DOCUMENT_BENCH_BOOST_JSON_HPP
