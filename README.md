# document

webcpp::document brings JavaScript programmers the wonders of C++ without the risks of C++.

document is a header-only JSON document for C++20, one of the libraries of
[webcpp](https://github.com/webcpporg/webcpp), whose behaviour is JavaScript's: what `JSON.parse`,
property access, assignment, `delete`, the array methods and `JSON.stringify` do in JavaScript,
document does in C++, with the same key order and the same text out, so that a JavaScript peer and
a C++ program agree byte for byte. Its core never throws: every operation that can fail returns its
error as a value, and the sugar that throws as JavaScript does exists only in a build with
exceptions.

What C++ gives JavaScript, and JavaScript does not have:

- JSON and NDJSON streams, read as the bytes arrive, in chunks of any size, with Boost.Asio,
  Boost.Cobalt or an actor system.
- Exact numbers: stored as the decimal that was written, written as JavaScript writes them, read
  back in full with `getDecimal`; or JavaScript's own doubles with `js_traits`.
- Images sealed by a CRC, so that a byte changed by accident is refused when an image is opened.
- Memory you manage: one relocatable image, `reserve` and `capacity`, `shrink_to_fit` and `compact`,
  pools of blocks, and no garbage collector in the way; the image has the same bytes on 64-bit
  native targets and on wasm32.
- Shared documents, counted by reference and copied on write, between threads and between processes.
- Binary transmission: an image is sent as it is and opened in place, with no parse and no
  allocation on receipt.
- Ordered-index queries: `lower_bound`, `equal_range` and prefix queries over an object's keys,
  with no sorting.

The parser takes text in chunks of any size, and its selectors deliver values while they stream,
in bounded memory.

Binary transmission is where document wins outright: moving an image costs nothing or a memory
copy between a page and its Worker, between threads, through shared memory between processes, and
from a socket into a pool block on a machine or a fast local link. It is honest about where it
loses. It parses more slowly than Boost.JSON. An image is many times larger than its JSON text, so
over a network the compressed text is the smaller payload. And a seal is a CRC against accidental
damage, not authentication: never open an image from a source you do not trust.

The same document is a JavaScript module, `js/webcpp-document.mjs`, compiled with Emscripten, for
a page, a Worker or Node: `Document.parse`, `get`, `set`, `push`, `splice`, `stringify`,
`getDecimal`, `keysInRange`, `image`, `Document.open`, `StreamParser` and `free`.

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
