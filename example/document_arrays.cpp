// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// JavaScript's array methods on a document: what each returns and what it leaves, with
// JavaScript's index rules, then the length, writing past the end, and where an array differs from
// JavaScript's. Each line it prints is what Node prints for the same JavaScript, but where the
// comments say otherwise.

#include <webcpp/document.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

// tag::helpers[]
/** The text of a value, `undefined` when nothing is there. */
std::string text(const wd::const_value& v) {
    return v.is_undefined() ? "undefined" : v.stringify();
}

/** Prints a call, what it returned and the array after it. */
void log(std::string_view call, std::string_view returned, const wd::const_value& a) {
    std::cout << call << " -> " << returned << ", a = " << text(a) << '\n';
}

/** Prints a call that returns a document: the removed values, or a new array. */
void log(std::string_view call, const wd::result<wd::document>& r, const wd::const_value& a) {
    log(call, r.has_value() ? text(r->root()) : r.error().message(), a);
}

/** Prints a call that returns a new length. */
void log(std::string_view call, const wd::result<std::uint64_t>& r, const wd::const_value& a) {
    log(call, r.has_value() ? std::to_string(*r) : r.error().message(), a);
}

/** Prints a call that returns nothing. */
void log(std::string_view call, const wd::result<void>& r, const wd::const_value& a) {
    log(call, r.has_value() ? "undefined" : r.error().message(), a);
}

// end::helpers[]

}  // namespace

int main() {
    wd::result<wd::document> made = wd::parse("[1,2,3,4,5]");
    if (made.has_error()) {
        return 1;
    }
    // tag::methods[]
    wd::value a = made->root();
    log("a.push(6, 7)", a.push(6, 7), a);
    log("a.pop()", a.pop(), a);
    log("a.shift()", a.shift(), a);
    log("a.unshift(0)", a.unshift(0), a);
    log("a.splice(1, 2, 'x', 'y', 'z')", a.splice(1, 2, "x", "y", "z"), a);
    log("a.reverse()", a.reverse(), a);
    log("a.slice(-2)", a.slice(-2), a);
    log("a.concat([8, 9], 10)", a.concat(wd::array{8, 9}, 10), a);
    log("a.at(-1)", text(a.at(-1)), a);
    // end::methods[]

    // tag::indices[]
    if (!a.set(wd::array{1, 2, 3, 4, 5})) {
        return 1;
    }
    log("a.splice(-2, 1)", a.splice(-2, 1), a);
    log("a.splice(10, 0, 'end')", a.splice(10, 0, "end"), a);
    log("a.splice(1, -5)", a.splice(1, -5), a);
    log("a.splice(3)", a.splice(3), a);
    // end::indices[]

    // tag::length[]
    log("a.length = 1", a.set_length(1), a);
    log("a.length = 3", a.set_length(3), a);
    log("a[5] = 6", a[5].set(6), a);
    log("a.pop()", a.pop(), a);
    log("a.pop()", a.pop(), a);  // JavaScript: undefined, the element of a hole
    // end::length[]

    // tag::differences[]
    if (!a.set(wd::array{})) {
        return 1;
    }
    log("a.pop()", a.pop(), a);  // a document that holds undefined
    // JavaScript inserts the array itself, a cycle; a document inserts a copy of it.
    if (!a.set(wd::array{1, 2})) {
        return 1;
    }
    log("a.splice(0, 1, a)", a.splice(0, 1, a), a);
    log("a[0].push({})", a[0].push(wd::object{}), a);  // the copy, not a
    // end::differences[]
}
