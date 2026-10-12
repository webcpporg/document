// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What the traits choose: the order of an object's keys, the encoding of strings, how dates are
// revived and written, and how bytes are written, with the default traits, JavaScript's own and
// two of the program's.

#include <webcpp/document.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

namespace {

// tag::define[]
/** The default traits, but every key in the order it was created, whatever it is. */
struct insertion_traits : wd::default_traits {
    using object = wd::insertion_order;
};

/** The default traits, but keys sorted by their code units, and bytes written as base64url. */
struct canonical_traits : wd::default_traits {
    using object = wd::sorted;
    using bytes = wd::base64url;
};

// end::define[]

/** Prints a document's text, or the error that made none. */
template <class Traits>
void print(std::string_view label, const wd::result<wd::basic_document<Traits>>& made) {
    std::cout << label << ": " << (made.has_value() ? made->stringify() : made.error().message())
              << '\n';
}

/** Prints the outcome of a write: ok, or its error. */
void print(std::string_view label, const wd::result<void>& r) {
    std::cout << label << ": " << (r.has_value() ? "ok" : r.error().message()) << '\n';
}

/** Prints a read: the value, a UTF-16 view's length in code units, or the error. */
template <class T>
void read(std::string_view label, const wd::result<T>& r) {
    std::cout << label << ": ";
    if (r.has_error()) {
        std::cout << r.error().message() << '\n';
    } else if constexpr (std::same_as<T, std::u16string_view>) {
        std::cout << r->size() << " code units\n";
    } else {
        std::cout << *r << '\n';
    }
}

bool order() {
    // tag::order[]
    constexpr std::string_view text = R"({"b":1,"2":2,"a":3,"1":4,"01":5})";
    print("javascript_order", wd::parse(text));
    print("insertion_order ", wd::parse<insertion_traits>(text));
    print("sorted          ", wd::parse<canonical_traits>(text));

    // An index key erased and created again goes back among the indices; any other key, last.
    wd::result<wd::document> made = wd::parse(text);
    if (made.has_error() || !made->root().erase("b") || !(*made)["b"].set(6) ||
        !made->root().erase("1") || !(*made)["1"].set(7)) {
        return false;
    }
    print("erased, created ", made);
    // end::order[]
    return true;
}

bool strings() {
    // tag::strings[]
    constexpr std::string_view text = R"(["é😀","tab\there \"q\" \u0001"])";
    wd::result<wd::document> utf8 = wd::parse(text);
    wd::result<wd::basic_document<wd::js_traits>> utf16 = wd::parse<wd::js_traits>(text);
    if (utf8.has_error() || utf16.has_error()) {
        return false;
    }
    // "é😀".length is 3 in UTF-16 code units, whatever the stored encoding.
    read("UTF-8 length ", (*utf8)[0].length());
    read("UTF-16 length", (*utf16)[0].length());
    read("UTF-8 as std::string_view", (*utf8)[0].get<std::string_view>());
    read("UTF-8 as std::u16string_view", (*utf8)[0].get<std::u16string_view>());
    read("UTF-16 as std::u16string_view", (*utf16)[0].get<std::u16string_view>());
    read("UTF-16 as std::string", (*utf16)[0].get<std::string>());
    std::cout << "both write: " << utf8->stringify() << ' ' << utf16->stringify() << '\n';

    // A lone surrogate: kept by UTF-16 and written as JSON.stringify writes it; refused by UTF-8.
    constexpr std::array<char16_t, 1> lone{0xD800};
    const std::u16string_view surrogate(lone.data(), lone.size());
    print("UTF-16 set", (*utf16)[2].set(surrogate));
    print("UTF-8 set ", (*utf8)[2].set(surrogate));
    std::cout << "UTF-16 writes: " << (*utf16)[2].stringify() << '\n';
    // end::strings[]
    return true;
}

bool dates() {
    // tag::dates[]
    constexpr std::string_view text = R"({"at":"2026-10-12T03:04:05.678Z","day":"2026-10-12"})";
    wd::parse_options revive;
    revive.revive_dates = true;
    wd::result<wd::document> plain = wd::parse(text);
    wd::result<wd::document> revived = wd::parse(text, revive);
    if (plain.has_error() || revived.has_error()) {
        return false;
    }
    std::cout << "is_date: " << std::boolalpha << (*plain)["at"].is_date() << ' '
              << (*revived)["at"].is_date() << ' ' << (*revived)["day"].is_date() << '\n';
    // A string in Date.prototype.toJSON's format reads as a date, revived or not.
    const wd::result<wd::date_ms> at = (*plain)["at"].get<wd::date_ms>();
    std::cout << "ms since the epoch: " << (at ? at->time_since_epoch().count() : 0) << '\n';

    const wd::date_ms last{std::chrono::milliseconds{8'640'000'000'000'000}};
    print("set the last date", (*plain)["last"].set(last));
    print("set one ms later ", (*plain)["later"].set(last + std::chrono::milliseconds{1}));
    print("written", plain);
    // end::dates[]
    return true;
}

bool bytes() {
    // tag::bytes[]
    const std::array<std::byte, 4> raw{
        std::byte{0xde},
        std::byte{0xad},
        std::byte{0xbe},
        std::byte{0xef},
    };
    const std::span<const std::byte> view(raw);
    wd::result<wd::document> base64 = wd::document::create();
    wd::result<wd::basic_document<canonical_traits>> base64url =
        wd::basic_document<canonical_traits>::create();
    wd::result<wd::basic_document<wd::js_traits>> js = wd::basic_document<wd::js_traits>::create();
    if (base64.has_error() || base64url.has_error() || js.has_error() ||
        !base64->root().set(view) || !base64url->root().set(view) || !js->root().set(view)) {
        return false;
    }
    print("base64          ", base64);
    print("base64url       ", base64url);
    print("javascript_bytes", js);

    // revive_bytes turns text of the bytes policy back into bytes.
    wd::parse_options revive;
    revive.revive_bytes = true;
    wd::result<wd::document> revived = wd::parse(R"({"key":"3q2+7w=="})", revive);
    if (revived.has_error()) {
        return false;
    }
    const wd::result<std::vector<std::byte>> key = (*revived)["key"].get<std::vector<std::byte>>();
    std::cout << "revived: " << std::boolalpha << (*revived)["key"].is_bytes() << ", "
              << (key ? key->size() : 0) << " bytes\n";
    // end::bytes[]
    return true;
}

}  // namespace

int main() {
    if (!order() || !strings() || !dates() || !bytes()) {
        std::cout << "error\n";
        return 1;
    }
}
