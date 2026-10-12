// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The first program of the guide: JSON text parsed, read, written, an element pushed, and the
// document written back with an indent of 2, as JSON.stringify(doc, null, 2) writes it.

// tag::program[]
// tag::includes[]
#include <webcpp/document.hpp>

#include <cstdint>
#include <iostream>
#include <string_view>

namespace wd = webcpp::document;
// end::includes[]

int main() {
    // tag::parse[]
    // JSON.parse, whose result holds the document or the error.
    wd::result<wd::document> made =
        wd::parse(R"({"name":"Ada","born":1815,"langs":["Analytical"]})");
    if (made.has_error()) {
        std::cout << "error: " << made.error().message() << '\n';
        return 1;
    }
    wd::document& doc = *made;
    // end::parse[]

    // tag::read[]
    // doc.name ?? "nobody", and doc.born ?? 0.
    std::cout << doc["name"].value_or(std::string_view("nobody")) << " was born in "
              << doc["born"].value_or(std::int64_t{0}) << '\n';
    // end::read[]

    // tag::write[]
    // doc.known = true; doc.langs.push("Note G"); each write returns its result.
    if (!doc["known"].set(true) || !doc["langs"].push("Note G")) {
        std::cout << "error: the document is full\n";
        return 1;
    }
    // end::write[]

    // tag::stringify[]
    // JSON.stringify(doc, null, 2).
    std::cout << doc.stringify(2) << '\n';
    // end::stringify[]
}

// end::program[]
