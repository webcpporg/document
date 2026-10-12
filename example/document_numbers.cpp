// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Numbers: exact by default, stored by value and written as JavaScript's Number::toString writes
// them; JavaScript's own doubles with js_traits; what each refuses; and reading a number as a C++
// type, which fails rather than round.

#include <webcpp/document.hpp>

#include <boost/decimal/decimal128_t.hpp>

#include <cstdint>
#include <iostream>
#include <string_view>

namespace wd = webcpp::document;

namespace {

/** Prints what a parse gives: the text the document writes back, or the error. */
template <class Traits>
void parsed(std::string_view label, const wd::result<wd::basic_document<Traits>>& made) {
    std::cout << label << ": " << (made.has_value() ? made->stringify() : made.error().message())
              << '\n';
}

/** Prints what a read gives: the value, or the error. */
template <class T>
void read(std::string_view label, const wd::result<T>& r) {
    std::cout << label << ": ";
    if (r.has_value()) {
        // The unary plus prints an 8-bit integer as a number, not as a character.
        std::cout << +*r << '\n';
    } else {
        std::cout << r.error().message() << '\n';
    }
}

}  // namespace

int main() {
    // tag::exact[]
    constexpr std::string_view text =
        "[0.1000000000000000000000000000000001,18446744073709551615,4611686018427387904,"
        "1e21,1.50,1E-7,1.0,-0]";
    parsed("default_traits", wd::parse(text));
    parsed("js_traits     ", wd::parse<wd::js_traits>(text));
    // end::exact[]

    // tag::refused[]
    constexpr std::string_view forty_digits = "1234567890123456789012345678901234567890";
    parsed("40 digits, default_traits", wd::parse(forty_digits));
    parsed("40 digits, js_traits     ", wd::parse<wd::js_traits>(forty_digits));
    parsed("1e999, default_traits    ", wd::parse("1e999"));
    parsed("1e999, js_traits         ", wd::parse<wd::js_traits>("1e999"));
    // end::refused[]

    // tag::read[]
    wd::result<wd::document> made =
        wd::parse(R"({"small":300,"big":18446744073709551615,"half":0.5,"huge":1e30})");
    if (made.has_error()) {
        return 1;
    }
    const wd::document& doc = *made;
    read("small as int64_t", doc["small"].get<std::int64_t>());
    read("small as int8_t", doc["small"].get<std::int8_t>());
    read("big as uint64_t", doc["big"].get<std::uint64_t>());
    read("big as int64_t", doc["big"].get<std::int64_t>());
    read("half as int64_t", doc["half"].get<std::int64_t>());
    std::cout << "Number.isInteger(huge): " << std::boolalpha << doc["huge"].is_integer() << '\n';
    // end::read[]

    // tag::arithmetic[]
    // 0.1 + 0.2, computed by the program in the traits' own number type, then stored.
    wd::result<wd::document> exact = wd::parse(R"({"a":0.1,"b":0.2})");
    wd::result<wd::basic_document<wd::js_traits>> js =
        wd::parse<wd::js_traits>(R"({"a":0.1,"b":0.2})");
    if (exact.has_error() || js.has_error()) {
        return 1;
    }
    using decimal = boost::decimal::decimal128_t;
    const wd::result<decimal> a = (*exact)["a"].get<decimal>();
    const wd::result<decimal> b = (*exact)["b"].get<decimal>();
    const wd::result<double> x = (*js)["a"].get<double>();
    const wd::result<double> y = (*js)["b"].get<double>();
    if (!a || !b || !x || !y || !(*exact)["sum"].set(*a + *b) || !(*js)["sum"].set(*x + *y)) {
        return 1;
    }
    std::cout << exact->stringify() << '\n' << js->stringify() << '\n';
    // end::arithmetic[]
}
