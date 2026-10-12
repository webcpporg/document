// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 What the two Emscripten modules of webcpp::document's JavaScript API have in common: the outcome
 of an operation, the path JavaScript names a place by, the conversion of a document's value to a
 JavaScript value, and the views of an image in WebAssembly memory.

 Tip: nothing here throws into JavaScript and nothing uses the library's sugar that throws: a
 failed `result` becomes the code and message of an `outcome`, which `webcpp-document.mjs` and
 `webcpp-document-shared.mjs` turn into `SyntaxError`, `RangeError` or `TypeError`.
*/

#ifndef WEBCPP_DOCUMENT_JS_BINDING_HPP
#define WEBCPP_DOCUMENT_JS_BINDING_HPP

#include <webcpp/document.hpp>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::document::binding {

using emscripten::val;
using owned_document = basic_document<default_traits, owned>;
using external_document = basic_document<default_traits, external>;

/**
 The result of an operation: code 0 with its value, or the error's code and message.

 Tip: JavaScript names it `Outcome`, as `register_outcome` registers it.
*/
struct outcome {
    int code = 0;
    std::string message;
    val value = val::undefined();
};

/** The outcome of an operation that gave `value`. */
inline outcome succeeded(val value = val::undefined()) {
    outcome out;
    out.value = std::move(value);
    return out;
}

/** The outcome of an operation that failed with `error`. */
inline outcome failed(boost::system::error_code error) {
    outcome out;
    out.code = error.value();
    out.message = error.message();
    return out;
}

/** The outcome of an operation that failed with `error`. */
inline outcome failed(errc error) {
    return failed(make_error_code(error));
}

/** The outcome of an operation whose result has no value. */
inline outcome outcome_of(const result<void>& done) {
    return done.has_value() ? succeeded() : failed(done.error());
}

/** The outcome of an operation whose result is a number: the number as a JavaScript number. */
inline outcome outcome_of(const result<std::uint64_t>& number) {
    return number.has_value() ? succeeded(val(static_cast<double>(number.unsafe_value())))
                              : failed(number.error());
}

/**
 One step of a path, validated by the JavaScript side: a key of an object, or an index of an
 array.
*/
struct step {
    bool is_index = false;
    std::string key{};
    std::uint64_t index = 0;
};

/** The steps of a JavaScript array of keys and indices. */
inline std::vector<step> steps_of(const val& path) {
    std::vector<step> steps;
    const auto count = path["length"].as<std::size_t>();
    steps.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const val element = path[i];
        if (element.typeOf().as<std::string>() == "number") {
            steps.push_back(
                {.is_index = true, .index = static_cast<std::uint64_t>(element.as<double>())});
        } else {
            steps.push_back({.key = element.as<std::string>()});
        }
    }
    return steps;
}

/**
 The handle that the steps lead to, read or written through.

 Tip: a step that finds nothing gives a handle to nothing, which reads as undefined and refuses a
 write.
*/
template <class Handle>
Handle walk(const Handle& handle, std::span<const step> path) {
    // A handle is not assignable, so each step's handle replaces the one it came from.
    std::optional<Handle> place(handle);
    for (const step& next : path) {
        Handle stepped =
            next.is_index ? (*place)[next.index] : (*place)[std::string_view(next.key)];
        place.emplace(std::move(stepped));
    }
    return *std::move(place);
}

/**
 The JavaScript value of a read-only handle: scalars as their JavaScript values, a number as
 `Number(text)` of its exact digits, a container as a copy made by `JSON.parse` of its text.

 Tip: the kind is checked first, so a scalar's read cannot fail; a failed read is still the
 outcome's error and not an exception.
*/
template <class Handle>
outcome to_javascript(const Handle& handle) {
    switch (handle.kind()) {
        case kind::undefined: return succeeded();
        case kind::null: return succeeded(val::null());
        case kind::boolean: {
            const result<bool> flag = handle.template get<bool>();
            return flag.has_value() ? succeeded(val(flag.unsafe_value())) : failed(flag.error());
        }
        case kind::string: {
            const result<std::string_view> text = handle.template get<std::string_view>();
            return text.has_value() ? succeeded(val(std::string(text.unsafe_value())))
                                    : failed(text.error());
        }
        case kind::number: return succeeded(val::global("Number")(val(handle.stringify(0))));
        default: return succeeded(val::global("JSON").call<val>("parse", val(handle.stringify(0))));
    }
}

/** The outcome of `get`: the JavaScript value at the path from `root`. */
template <class Handle>
outcome read_outcome(const Handle& root, const std::vector<step>& steps) {
    return to_javascript(walk(root, steps));
}

/** The outcome of `getDecimal`: the exact text of the number at the path from `root`. */
template <class Handle>
outcome decimal_outcome(const Handle& root, const std::vector<step>& steps) {
    const auto handle = walk(root, steps);
    return handle.kind() == kind::number ? succeeded(val(handle.stringify(0))) : failed(errc::type);
}

/** The outcome of `length`: the length of the array or string at the path from `root`. */
template <class Handle>
outcome length_outcome(const Handle& root, const std::vector<step>& steps) {
    return outcome_of(walk(root, steps).length());
}

/**
 The outcome of `set`: writes the JSON text `json` at the path from `root`, or erases the member
 when it is undefined.
*/
template <class Handle>
outcome write_outcome(const Handle& root, const std::vector<step>& steps, const val& json) {
    auto place = walk(root, steps);
    if (json.isUndefined()) {
        return outcome_of(place.set(undefined));
    }
    auto parsed = parse(json.as<std::string>());
    if (!parsed.has_value()) {
        return failed(parsed.error());
    }
    const owned_document& source = parsed.unsafe_value();
    return outcome_of(place.set(source.root()));
}

/**
 The bytes at `address` in wasm memory.

 Tip: JavaScript names an address as a number, the one form it passes through embind, and nothing
 else can turn it into a pointer.
*/
inline std::byte* memory_at(std::uintptr_t address) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<std::byte*>(address);
}

/** A Uint8Array sharing the bytes at their address in wasm memory. */
inline val view_of(std::span<const std::byte> bytes) {
    return val(emscripten::typed_memory_view(bytes.size(),
                                             reinterpret_cast<const std::uint8_t*>(bytes.data())));
}

/** The buffer of wasm memory, which a view of address 0 names. */
inline val wasm_memory_buffer() {
    return val(
        emscripten::typed_memory_view(0, static_cast<const std::uint8_t*>(nullptr)))["buffer"];
}

/**
 Registers `outcome` with embind, as a value object of JavaScript.

 Tip: each module registers it from its own EMSCRIPTEN_BINDINGS, as each is a module of its own.
*/
inline void register_outcome() {
    emscripten::value_object<outcome>("Outcome")
        .field("code", &outcome::code)
        .field("message", &outcome::message)
        .field("value", &outcome::value);
}

}  // namespace webcpp::document::binding

#endif  // WEBCPP_DOCUMENT_JS_BINDING_HPP
