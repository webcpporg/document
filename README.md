# document

A JSON document for C++20 that behaves as JavaScript does: the same key order, the same
`JSON.stringify` bytes, the array methods, exact numbers, streaming parse with selectors, and one
relocatable binary image that works at any address and is the same on 64-bit native and wasm32. Its
core never throws. Header-only; it needs Boost 1.92 (Interprocess, MultiIndex, Decimal, JSON's parser).
Targets: native, wasm32-wasip2, wasm32-wasip3 and Emscripten.

## Building and testing

    b2 libs/document/test libs/document/example

## Documentation

https://webcpporg.github.io/libs/document/

## License

Distributed under the [Boost Software License, Version 1.0](https://www.boost.org/LICENSE_1_0.txt).
