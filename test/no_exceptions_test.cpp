// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests that a build without exceptions refuses each member that throws: the document's default
// and copy constructors, both assignments to a handle, as, as_string, as_bool and parse_or_throw.
// Each call stops with the error clang's -verify finds, and the note that names the constraint
// exceptions_enabled, which no build without exceptions satisfies. The expectations name whole
// messages, longer than a line, which clang-format would otherwise break.

#include <webcpp/document.hpp>

#include <cstdint>

namespace wd = webcpp::document;

namespace {

// clang-format off
void each_member_that_throws(wd::document& made) {
    // expected-error@+2 {{no matching constructor for initialization of 'wd::document'}}
    // expected-note@webcpp/document/document.hpp:* {{because 'webcpp::document::default_traits' does not satisfy 'exceptions_enabled'}}
    wd::document doc;

    // expected-error@+2 {{no matching constructor for initialization of 'const wd::document'}}
    // expected-note@webcpp/document/document.hpp:* {{because 'webcpp::document::default_traits' does not satisfy 'exceptions_enabled'}}
    const wd::document copy(made);

    // expected-error@+3 {{no viable overloaded '='}}
    // expected-note@webcpp/document/value.hpp:* {{because 'int' does not satisfy 'exceptions_enabled'}}
    // expected-note@webcpp/document/value.hpp:* {{because 'webcpp::document::default_traits' does not satisfy 'exceptions_enabled'}}
    made["a"] = 1;

    // expected-error@+3 {{no viable overloaded '='}}
    // expected-note@webcpp/document/value.hpp:* {{because 'webcpp::document::basic_value<>' does not satisfy 'exceptions_enabled'}}
    // expected-note@webcpp/document/value.hpp:* {{because 'webcpp::document::default_traits' does not satisfy 'exceptions_enabled'}}
    made["a"] = made["b"];

    // expected-error@+2 {{no matching member function for call to 'as'}}
    // expected-note@webcpp/document/value.hpp:* {{because 'int' does not satisfy 'exceptions_enabled'}}
    (void)made["a"].as<std::int32_t>();

    // expected-error@+2 {{no matching member function for call to 'as_string'}}
    // expected-note@webcpp/document/value.hpp:* {{because 'char' does not satisfy 'exceptions_enabled'}}
    (void)made["a"].as_string();

    // expected-error@+2 {{no matching member function for call to 'as_bool'}}
    // expected-note@webcpp/document/value.hpp:* {{because 'bool' does not satisfy 'exceptions_enabled'}}
    (void)made["a"].as_bool();

    // expected-error@+2 {{no matching function for call to 'parse_or_throw'}}
    // expected-note@webcpp/document/parse.hpp:* {{because 'webcpp::document::default_traits' does not satisfy 'exceptions_enabled'}}
    (void)wd::parse_or_throw("[1]");
}
// clang-format on

}  // namespace

int main() {
    auto made = wd::document::create();
    if (made.has_value()) {
        each_member_that_throws(*made);
    }
    return 0;
}
