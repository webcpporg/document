// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The containers as a C++ program uses them: an object is a sorted map and an array a vector, both
// views that the standard algorithms and ranges read. Each line it prints is the same on every
// target, native or in a browser.

// tag::program[]
// tag::includes[]
#include <webcpp/document.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>

namespace wd = webcpp::document;
// end::includes[]

namespace {

// tag::helpers[]
/** Prints the message of a failed result, and yields the exit code of main. */
template <class Result>
int fail(const Result& r) {
    std::cout << "error: " << r.error().message() << '\n';
    return 1;
}

/** Prints an operation and the document after it, as JSON. */
void step(std::string_view what, const wd::document& doc) {
    std::cout << what << " -> " << doc.stringify() << '\n';
}

// end::helpers[]

/** Inserts, finds and erases the members of an object, printing each step. */
int object_section() {
    // tag::object[]
    // An object is a sorted map: insert and try_emplace answer the member and whether it was
    // inserted, and insert_or_assign assigns to it as JavaScript's o[key] = v does.
    wd::result<wd::document> made = wd::parse("{}");
    if (made.has_error()) {
        return fail(made);
    }
    wd::document& doc = *made;
    wd::result<wd::object_ref> as_object = doc.root().as_object();
    if (as_object.has_error()) {
        return fail(as_object);
    }
    const wd::object_ref obj = *as_object;

    const auto name = obj.insert("name", "Ada");
    if (name.has_error()) {
        return fail(name);
    }
    std::cout << "insert name: " << name->second << '\n';

    // A key that is there keeps its value.
    const auto again = obj.insert("name", "Grace");
    if (again.has_error()) {
        return fail(again);
    }
    std::cout << "insert name again: " << again->second
              << ", name = " << doc["name"].value_or(std::string_view("nobody")) << '\n';

    const auto assigned = obj.insert_or_assign("name", "Augusta Ada");
    if (assigned.has_error()) {
        return fail(assigned);
    }
    std::cout << "insert_or_assign name: " << assigned->second
              << ", name = " << doc["name"].value_or(std::string_view("nobody")) << '\n';

    const auto born = obj.try_emplace("born", 1815);
    if (born.has_error()) {
        return fail(born);
    }
    std::cout << "try_emplace born: " << born->second << '\n';

    // find answers the member's iterator, or the end; contains answers whether the key is there.
    const wd::object_ref::iterator found = obj.find("born");
    std::cout << "find born: " << found->second.value_or(std::int64_t{0}) << '\n';
    const wd::object_ref::iterator missing = obj.find("nope");
    std::cout << "contains born: " << obj.contains("born")
              << ", find nope is end: " << (missing == obj.end()) << '\n';

    const auto erased = obj.erase("born");
    if (erased.has_error()) {
        return fail(erased);
    }
    std::cout << "erase born: " << *erased << ", contains born: " << obj.contains("born") << '\n';

    // A new key comes last, and so does a key erased and inserted again: the members iterate in
    // JavaScript's order, which is the order they were added in.
    const auto langs = obj.insert("langs", wd::array{"Analytical", "Note G"});
    if (langs.has_error()) {
        return fail(langs);
    }
    const auto known = obj.insert("known", true);
    if (known.has_error()) {
        return fail(known);
    }
    const auto born_again = obj.insert("born", 1815);
    if (born_again.has_error()) {
        return fail(born_again);
    }
    for (const auto& [key, handle] : obj) {
        std::cout << key << " = " << handle.stringify() << '\n';
    }
    // end::object[]
    return 0;
}

/** Pushes, inserts, erases and resizes an array, and reads its ends and its elements. */
int array_section() {
    // tag::array[]
    // An array is a vector: push_back, emplace_back, insert, erase and resize change it as
    // std::vector does, and at checks the index.
    wd::result<wd::document> list_made = wd::parse("[1,2,3]");
    if (list_made.has_error()) {
        return fail(list_made);
    }
    wd::document& list_doc = *list_made;
    wd::result<wd::array_ref> as_array = list_doc.root().as_array();
    if (as_array.has_error()) {
        return fail(as_array);
    }
    const wd::array_ref list = *as_array;

    const auto pushed = list.push_back(4);
    if (pushed.has_error()) {
        return fail(pushed);
    }
    step("push_back(4)", list_doc);

    const auto emplaced = list.emplace_back(5);
    if (emplaced.has_error()) {
        return fail(emplaced);
    }
    step("emplace_back(5)", list_doc);

    const auto placed = list.insert(std::ranges::next(list.begin(), 1), 9);
    if (placed.has_error()) {
        return fail(placed);
    }
    step("insert(begin + 1, 9)", list_doc);

    // erase answers the iterator of the element after the one it removed.
    const wd::array_ref::iterator after = list.erase(std::ranges::next(list.begin(), 1));
    step("erase(begin + 1)", list_doc);
    const wd::value next_element = *after;
    std::cout << "erase returns the element after: " << next_element.value_or(std::int64_t{0})
              << '\n';

    const auto shrunk = list.resize(3);
    if (shrunk.has_error()) {
        return fail(shrunk);
    }
    step("resize(3)", list_doc);
    std::cout << "front " << list.front().value_or(std::int64_t{0}) << ", back "
              << list.back().value_or(std::int64_t{0}) << '\n';

    const wd::result<wd::value> third = list.at(2);
    if (third.has_error()) {
        return fail(third);
    }
    std::cout << "at(2) = " << third->value_or(std::int64_t{0}) << '\n';
    // An index past the end is an error value, not an undefined element.
    const wd::result<wd::value> beyond = list.at(10);
    std::cout << "at(10) is errc::range: "
              << (beyond.has_error() && beyond.error() == wd::errc::range) << '\n';

    const auto grown = list.resize(5);
    if (grown.has_error()) {
        return fail(grown);
    }
    step("resize(5)", list_doc);
    // end::array[]
    return 0;
}

/** Counts, filters and finds the members of an object and of an array. */
int ranges_section() {
    // tag::ranges[]
    // An object and an array are ranges: the standard algorithms and the views over them read
    // their members, as they read a vector.
    wd::result<wd::document> shop_made = wd::document::create();
    if (shop_made.has_error()) {
        return fail(shop_made);
    }
    wd::document& shop = *shop_made;
    const auto shop_set = shop.root().set(wd::object{
        {"prices", wd::array{3, 8, 12, 5, 20}},
        {"stock", wd::object{{"apple", 4}, {"pear", 0}, {"plum", 7}}},
    });
    if (shop_set.has_error()) {
        return fail(shop_set);
    }
    wd::result<wd::array_ref> as_prices = shop["prices"].as_array();
    if (as_prices.has_error()) {
        return fail(as_prices);
    }
    const wd::array_ref prices = *as_prices;
    wd::result<wd::object_ref> as_stock = shop["stock"].as_object();
    if (as_stock.has_error()) {
        return fail(as_stock);
    }
    const wd::object_ref stock = *as_stock;

    const auto above_five = [](const wd::value& v) { return v.value_or(std::int64_t{0}) > 5; };
    const auto doubled_value = [](const wd::value& v) { return 2 * v.value_or(std::int64_t{0}); };
    std::cout << "prices above 5: " << std::ranges::count_if(prices, above_five) << '\n';
    std::cout << "prices above 5, doubled:";
    // The view is built and iterated in one statement: a filter view has no const iteration.
    for (const std::int64_t twice :
         prices | std::views::filter(above_five) | std::views::transform(doubled_value)) {
        std::cout << ' ' << twice;
    }
    std::cout << '\n';

    const auto empty = std::ranges::find_if(
        stock, [](const auto& member) { return member.second.value_or(std::int64_t{0}) == 0; });
    std::cout << "first out of stock: " << empty->first << '\n';
    // end::ranges[]
    return 0;
}

/** Prints a catalog in JavaScript's order and in key order, and the ranges of its keys. */
int by_key_section() {
    // tag::by_key[]
    // by_key is the same object in key order, by UTF-16 code units, with bounds for ranges of keys.
    // A JavaScript program sorts every key to get any of these; here the tree keeps them sorted.
    wd::result<wd::document> catalog_made = wd::parse("{}");
    if (catalog_made.has_error()) {
        return fail(catalog_made);
    }
    wd::document& catalog = *catalog_made;
    wd::result<wd::object_ref> as_catalog = catalog.root().as_object();
    if (as_catalog.has_error()) {
        return fail(as_catalog);
    }
    const wd::object_ref catalog_obj = *as_catalog;
    const std::array<std::string_view, 13> names{
        "user_3", "admin",  "user_1", "guest",  "user_9", "user",   "user_2",
        "owner",  "user_7", "user_5", "user_4", "user_8", "user_6",
    };
    for (const std::string_view key : names) {
        const auto added = catalog_obj.insert(key, true);
        if (added.has_error()) {
            return fail(added);
        }
    }
    std::cout << "JavaScript order:";
    for (const auto& member : catalog_obj) {
        std::cout << ' ' << member.first;
    }
    std::cout << '\n';
    const auto keys = catalog_obj.by_key();
    std::cout << "by_key order:";
    for (const auto& member : keys) {
        std::cout << ' ' << member.first;
    }
    std::cout << '\n';

    // The keys with the prefix "user_" lie from its first key up to the first key at or after
    // "user`", the character after '_'.
    const std::ranges::subrange users(keys.lower_bound("user_"), keys.lower_bound("user`"));
    std::cout << "prefix user_: " << std::ranges::distance(users) << " keys, from "
              << users.begin()->first << " to " << std::ranges::prev(users.end())->first << '\n';

    const auto [exact, exact_end] = keys.equal_range("user_5");
    std::cout << "equal_range user_5: " << std::ranges::distance(exact, exact_end) << " member, "
              << exact->first << '\n';

    std::cout << "lower_bound p: " << keys.lower_bound("p")->first << '\n';
    const std::ranges::subrange between(keys.lower_bound("g"), keys.lower_bound("v"));
    std::cout << "keys from g up to v: " << std::ranges::distance(between) << '\n';
    // end::by_key[]
    return 0;
}

/** Inserts a batch into two objects, one of which reserved its image before the batch. */
int reserve_section() {
    // tag::reserve[]
    // The batch's size is known, so the image is reserved once, before the insertions: they then
    // move nothing. Without the reservation, the same insertions grow the image.
    constexpr std::int64_t batch = 10'000;
    constexpr std::uint64_t bytes_per_member = 256;

    wd::result<wd::document> plain_made = wd::parse("{}");
    if (plain_made.has_error()) {
        return fail(plain_made);
    }
    wd::document& plain = *plain_made;
    wd::result<wd::object_ref> as_plain = plain.root().as_object();
    if (as_plain.has_error()) {
        return fail(as_plain);
    }
    const std::uint64_t plain_before = plain.capacity();
    for (std::int64_t i = 0; i < batch; ++i) {
        const auto added = as_plain->insert("k" + std::to_string(i), i);
        if (added.has_error()) {
            return fail(added);
        }
    }
    std::cout << "without reserve, " << batch
              << " insertions grew the image: " << (plain.capacity() != plain_before) << '\n';

    wd::result<wd::document> reserved_made = wd::parse("{}");
    if (reserved_made.has_error()) {
        return fail(reserved_made);
    }
    wd::document& reserved = *reserved_made;
    wd::result<wd::object_ref> as_reserved = reserved.root().as_object();
    if (as_reserved.has_error()) {
        return fail(as_reserved);
    }
    const std::uint64_t reserved_before = reserved.capacity();
    const auto reservation = reserved.reserve(static_cast<std::uint64_t>(batch) * bytes_per_member);
    if (reservation.has_error()) {
        return fail(reservation);
    }
    const std::uint64_t reserved_capacity = reserved.capacity();
    std::cout << "reserve grew the image: " << (reserved_capacity != reserved_before) << '\n';
    for (std::int64_t i = 0; i < batch; ++i) {
        const auto added = as_reserved->insert("k" + std::to_string(i), i);
        if (added.has_error()) {
            return fail(added);
        }
    }
    std::cout << "after the reserve, " << batch
              << " insertions grew it: " << (reserved.capacity() != reserved_capacity) << '\n';
    // end::reserve[]
    return 0;
}

}  // namespace

int main() {
    std::cout << std::boolalpha;

    if (const int code = object_section(); code != 0) {
        return code;
    }
    if (const int code = array_section(); code != 0) {
        return code;
    }
    if (const int code = ranges_section(); code != 0) {
        return code;
    }
    if (const int code = by_key_section(); code != 0) {
        return code;
    }
    return reserve_section();
}

// end::program[]
