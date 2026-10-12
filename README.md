# document

document is a header-only JSON document for C++20, one of the libraries of
[webcpp](https://github.com/webcpporg/webcpp), whose behaviour is JavaScript's: what `JSON.parse`,
property access, assignment, `delete`, the array methods and `JSON.stringify` do in JavaScript,
document does in C++, with the same key order and the same text out, so that a JavaScript peer and
a C++ program agree byte for byte.

Numbers are exact by default, stored by value and written as JavaScript writes them, or
JavaScript's own doubles with `js_traits`. The parser takes text in chunks of any size, and its
selectors deliver values while they stream, in bounded memory. A document lives in one relocatable
binary image, which works at any address and has the same bytes on 64-bit native targets and on
wasm32, so that it can live in memory its user owns and travel as binary. Its core never throws:
every operation that can fail returns its error as a value, and the sugar that throws as
JavaScript does exists only in a build with exceptions.

`<webcpp/document.hpp>` includes every public header, whose names are in the namespace
`webcpp::document`. document needs Boost 1.92 or newer (Interprocess, MultiIndex, Decimal, System,
and JSON's parser) and webcpp's configuration, config. It runs natively, on emscripten, on
wasm32-wasip2 and on wasm32-wasip3.

## A minimal example

This is [example/document_quick_start.cpp](example/document_quick_start.cpp), which parses a JSON
text, reads two members, writes one, pushes an element, and writes the document back as
`JSON.stringify(doc, null, 2)` does:

<!-- include::example/document_quick_start.cpp[tag=program] -->
```cpp
#include <webcpp/document.hpp>

#include <cstdint>
#include <iostream>
#include <string_view>

namespace wd = webcpp::document;

int main() {
    // JSON.parse, whose result holds the document or the error.
    wd::result<wd::document> made =
        wd::parse(R"({"name":"Ada","born":1815,"langs":["Analytical"]})");
    if (made.has_error()) {
        std::cout << "error: " << made.error().message() << '\n';
        return 1;
    }
    wd::document& doc = *made;

    // doc.name ?? "nobody", and doc.born ?? 0.
    std::cout << doc["name"].value_or(std::string_view("nobody")) << " was born in "
              << doc["born"].value_or(std::int64_t{0}) << '\n';

    // doc.known = true; doc.langs.push("Note G"); each write returns its result.
    if (!doc["known"].set(true) || !doc["langs"].push("Note G")) {
        std::cout << "error: the document is full\n";
        return 1;
    }

    // JSON.stringify(doc, null, 2).
    std::cout << doc.stringify(2) << '\n';
}
```

It prints:

<!-- include::example/document_quick_start.expected[] -->
```
Ada was born in 1815
{
  "name": "Ada",
  "born": 1815,
  "langs": [
    "Analytical",
    "Note G"
  ],
  "known": true
}
```

## Building and testing

document is developed inside the webcpp superproject, as a Boost library is developed inside
Boost:

    git clone --recursive https://github.com/webcpporg/webcpp
    cd webcpp
    b2 libs/document/test libs/document/example

builds the tests and the examples natively, runs every example and compares its output with the
`.expected` file beside it. `toolset=emscripten` does the same on emscripten, run by node, with the
toolset that `user-config.jam` registers against emsdk.
`toolset=clang-wasip2 testing.launcher=wasmtime` and
`toolset=clang-wasip3 testing.launcher=wasmtime` do the same for WASI, with the toolsets that
`user-config.jam` registers against wasi-sdk. `b2 libs/document/test/oracle//oracle` runs the
cases of the oracle in Node, which the test `oracle_cases` runs through the document. A b2 project
uses document through `/webcpp/document//document`.

## Documentation

document's page, with its API reference, is published at
<https://webcpporg.github.io/libs/document/>; `b2 libs/document/doc` builds it into
`doc/html/index.html`.

## License

Distributed under the [Boost Software License, Version 1.0](LICENSE_1_0.txt).
