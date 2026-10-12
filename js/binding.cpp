// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The Emscripten side of webcpp::document's JavaScript API. It exports the classes and functions
// that webcpp-document.mjs wraps with JavaScript's own values and errors: every operation returns
// an outcome, a code and a message or the value it gives, and nothing here throws into JavaScript.
// What the shared module (shared/binding.cpp) has in common with it is in binding.hpp.

#include "binding.hpp"

#include <webcpp/document.hpp>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <malloc.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace wd = webcpp::document;
using wd::binding::decimal_outcome;
using wd::binding::external_document;
using wd::binding::failed;
using wd::binding::length_outcome;
using wd::binding::outcome;
using wd::binding::outcome_of;
using wd::binding::owned_document;
using wd::binding::read_outcome;
using wd::binding::step;
using wd::binding::steps_of;
using wd::binding::succeeded;
using wd::binding::to_javascript;
using wd::binding::val;
using wd::binding::view_of;
using wd::binding::walk;
using wd::binding::wasm_memory_buffer;
using wd::binding::write_outcome;

// A document of either storage: owned, or external over the bytes it was opened from. Empty until
// it holds one, as a stream's next() gives it when no document is complete yet.
class native_document {
public:
    native_document() = default;

    explicit native_document(boost::system::error_code error)
        : code_(error.value()), message_(error.message()) {}

    explicit native_document(wd::errc error) : native_document(wd::make_error_code(error)) {}

    explicit native_document(owned_document document) : storage_(std::move(document)) {}

    native_document(external_document document, void* copy)
        : storage_(std::move(document)), copy_(copy) {}

    native_document(const native_document&) = delete;
    native_document& operator=(const native_document&) = delete;
    native_document& operator=(native_document&&) = delete;

    native_document(native_document&& other) noexcept
        : code_(other.code_),
          message_(std::move(other.message_)),
          storage_(std::move(other.storage_)),
          copy_(std::exchange(other.copy_, nullptr)) {}

    // Frees the copy in memory of a document opened from bytes outside it, once the document
    // that reads it is gone.
    ~native_document() { std::free(copy_); }

    [[nodiscard]] int code() const { return code_; }

    [[nodiscard]] std::string message() const { return message_; }

    [[nodiscard]] bool has() const { return storage_.index() != 0; }

    // A JavaScript number, not a BigInt: an image is far below 2^53 bytes.
    [[nodiscard]] double byte_length() const {
        return with_document(
            [](const auto& document) { return static_cast<double>(document.image().size()); }, 0.0);
    }

    [[nodiscard]] val image() const {
        return with_document([](const auto& document) { return view_of(document.image()); },
                             view_of({}));
    }

    [[nodiscard]] outcome get(const val& path) const {
        const auto steps = steps_of(path);
        return with_document(
            [&](const auto& document) { return read_outcome(document.root(), steps); },
            failed(wd::errc::type));
    }

    outcome set(const val& path, const val& json) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return write_outcome(document.root(), steps, json); });
    }

    outcome erase(const val& path) {
        const auto steps = steps_of(path);
        if (steps.empty() || steps.back().is_index) {
            return failed(wd::errc::type);
        }
        const std::span<const step> parent = std::span(steps).first(steps.size() - 1);
        const std::string key = steps.back().key;
        return with_mutable_document([&](auto& document) {
            const auto erased = walk(document.root(), parent).erase(std::string_view(key));
            return erased.has_value() ? succeeded(val(erased.unsafe_value()))
                                      : failed(erased.error());
        });
    }

    outcome push(const val& path, const std::string& json) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return push_outcome(document, steps, json); });
    }

    outcome splice(const val& path, double start, double count, const val& items) {
        const auto steps = steps_of(path);
        std::vector<owned_document> inserted;
        const auto texts = items["length"].as<std::size_t>();
        inserted.reserve(texts);
        for (std::size_t i = 0; i < texts; ++i) {
            auto parsed = wd::parse(items[i].as<std::string>());
            if (!parsed.has_value()) {
                return failed(parsed.error());
            }
            inserted.push_back(std::move(parsed.unsafe_value()));
        }
        return with_mutable_document([&](auto& document) {
            return splice_outcome(document, steps, static_cast<std::int64_t>(start),
                                  static_cast<std::int64_t>(count), inserted);
        });
    }

    [[nodiscard]] outcome length(const val& path) const {
        const auto steps = steps_of(path);
        return with_document(
            [&](const auto& document) { return length_outcome(document.root(), steps); },
            failed(wd::errc::type));
    }

    [[nodiscard]] outcome get_decimal(const val& path) const {
        const auto steps = steps_of(path);
        return with_document(
            [&](const auto& document) { return decimal_outcome(document.root(), steps); },
            failed(wd::errc::type));
    }

    [[nodiscard]] outcome keys_in_range(const val& path, const std::string& low,
                                        const std::string& high) const {
        const auto steps = steps_of(path);
        return with_document(
            [&](const auto& document) {
                auto object = walk(document.root(), steps).as_object();
                if (!object.has_value()) {
                    return failed(object.error());
                }
                const auto keys = object.unsafe_value().by_key();
                const auto end = keys.lower_bound(high);
                val array = val::array();
                for (auto it = keys.lower_bound(low); it != end; ++it) {
                    array.call<void>("push", val(std::string((*it).first)));
                }
                return succeeded(array);
            },
            failed(wd::errc::type));
    }

    [[nodiscard]] std::string stringify(std::uint32_t indent) const {
        return with_document([&](const auto& document) { return document.stringify(indent); },
                             std::string());
    }

    outcome seal() {
        return with_mutable_document([](auto& document) { return outcome_of(document.seal()); });
    }

    [[nodiscard]] bool sealed() const {
        return with_document([](const auto& document) { return document.sealed(); }, false);
    }

    [[nodiscard]] double capacity() const {
        return with_document(
            [](const auto& document) { return static_cast<double>(document.capacity()); }, 0.0);
    }

    outcome reserve(double bytes) {
        return with_mutable_document([&](auto& document) {
            return outcome_of(document.reserve(static_cast<std::uint64_t>(bytes)));
        });
    }

    outcome pop(const val& path) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return removed_outcome(walk(document.root(), steps).pop()); });
    }

    outcome shift(const val& path) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return removed_outcome(walk(document.root(), steps).shift()); });
    }

    outcome unshift(const val& path, const std::string& json) {
        const auto steps = steps_of(path);
        return with_mutable_document([&](auto& document) {
            auto parsed = wd::parse(json);
            if (!parsed.has_value()) {
                return failed(parsed.error());
            }
            const owned_document& source = parsed.unsafe_value();
            return outcome_of(walk(document.root(), steps).unshift(source.root()));
        });
    }

    outcome reverse(const val& path) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return outcome_of(walk(document.root(), steps).reverse()); });
    }

    outcome set_length(const val& path, double length) {
        const auto steps = steps_of(path);
        return with_mutable_document([&](auto& document) {
            return outcome_of(
                walk(document.root(), steps).set_length(static_cast<std::uint64_t>(length)));
        });
    }

    // Object.assign of the JSON text's value into the object the path names.
    outcome assign(const val& path, const std::string& json) {
        const auto steps = steps_of(path);
        return with_mutable_document([&](auto& document) {
            auto parsed = wd::parse(json);
            if (!parsed.has_value()) {
                return failed(parsed.error());
            }
            const owned_document& source = parsed.unsafe_value();
            return outcome_of(walk(document.root(), steps).assign_from(source.root()));
        });
    }

    // Opened in place: the external document of the bytes at `address`, which the caller keeps.
    static native_document open_in_place(std::uintptr_t address, std::size_t length) {
        auto opened = wd::open(std::span<std::byte>(wd::binding::memory_at(address), length));
        if (!opened.has_value()) {
            return native_document(opened.error());
        }
        return {std::move(opened.unsafe_value()), nullptr};
    }

    // Opened from a copy of the bytes made in wasm memory, which this document frees.
    static native_document open_copy(const val& bytes, std::size_t length) {
        const std::size_t capacity = std::max<std::size_t>(16, (length + 15) / 16 * 16);
        void* copy = std::aligned_alloc(16, capacity);
        if (copy == nullptr) {
            return native_document(wd::errc::capacity);
        }
        val target(emscripten::typed_memory_view(length, static_cast<std::uint8_t*>(copy)));
        target.call<void>("set", bytes);
        auto opened = wd::open(std::span<std::byte>(static_cast<std::byte*>(copy), length));
        if (!opened.has_value()) {
            std::free(copy);
            return native_document(opened.error());
        }
        return {std::move(opened.unsafe_value()), copy};
    }

    static native_document parse(const std::string& text) {
        auto parsed = wd::parse(text);
        if (!parsed.has_value()) {
            return native_document(parsed.error());
        }
        return native_document(std::move(parsed.unsafe_value()));
    }

    static native_document from_document(owned_document document) {
        return native_document(std::move(document));
    }

private:
    // The operation on the document it holds, or the fallback when it holds none.
    template <class Operation, class Fallback>
    Fallback with_document(Operation&& operation, Fallback fallback) const {
        if (const auto* owned = std::get_if<owned_document>(&storage_)) {
            return operation(*owned);
        }
        if (const auto* external = std::get_if<external_document>(&storage_)) {
            return operation(*external);
        }
        return fallback;
    }

    template <class Operation>
    outcome with_mutable_document(Operation&& operation) {
        if (auto* owned = std::get_if<owned_document>(&storage_)) {
            return operation(*owned);
        }
        if (auto* external = std::get_if<external_document>(&storage_)) {
            return operation(*external);
        }
        return failed(wd::errc::type);
    }

    // The element pop or shift removed, as a JavaScript value: undefined from an empty array.
    static outcome removed_outcome(const wd::result<owned_document>& removed) {
        if (!removed.has_value()) {
            return failed(removed.error());
        }
        return to_javascript(removed.unsafe_value().root());
    }

    template <class Document>
    static outcome push_outcome(Document& document, const std::vector<step>& steps,
                                const std::string& json) {
        auto parsed = wd::parse(json);
        if (!parsed.has_value()) {
            return failed(parsed.error());
        }
        const owned_document& source = parsed.unsafe_value();
        return outcome_of(walk(document.root(), steps).push(source.root()));
    }

    // The first inserted value goes in with the removal, and each next one after the one before it.
    template <class Document>
    static outcome splice_outcome(Document& document, const std::vector<step>& steps,
                                  std::int64_t start, std::int64_t count,
                                  const std::vector<owned_document>& inserted) {
        auto target = walk(document.root(), steps);
        auto removed = inserted.empty()
                           ? target.splice(start, count)
                           : target.splice(start, count, std::as_const(inserted.front()).root());
        if (!removed.has_value()) {
            return failed(removed.error());
        }
        for (std::size_t i = 1; i < inserted.size(); ++i) {
            const auto added = target.splice(start + static_cast<std::int64_t>(i), 0,
                                             std::as_const(inserted[i]).root());
            if (!added.has_value()) {
                return failed(added.error());
            }
        }
        const owned_document& gone = removed.unsafe_value();
        return to_javascript(gone.root());
    }

    int code_ = 0;
    std::string message_;
    std::variant<std::monostate, owned_document, external_document> storage_;
    void* copy_ = nullptr;
};

// A stream of documents: bytes in, and each document as it completes, taken by next().
class native_stream {
public:
    native_stream(const std::string& framing, bool skip_errors)
        : parser_(options(framing, skip_errors)) {}

    native_stream(const native_stream&) = delete;
    native_stream& operator=(const native_stream&) = delete;

    outcome write(const val& chunk) {
        const auto length = chunk["byteLength"].as<std::size_t>();
        std::string bytes(length, '\0');
        val view(
            emscripten::typed_memory_view(length, reinterpret_cast<std::uint8_t*>(bytes.data())));
        view.call<void>("set", chunk);
        const auto written = parser_.write(bytes);
        return written.has_value() ? succeeded() : failed(written.error());
    }

    // The next complete document, or an empty one when none is complete; JavaScript owns it.
    native_document* next() {
        auto taken = parser_.next();
        if (!taken.has_value()) {
            return new native_document(taken.error());
        }
        auto& completed = taken.unsafe_value();
        if (!completed.has_value()) {
            return new native_document();
        }
        return new native_document(native_document::from_document(std::move(*completed)));
    }

    outcome finish() {
        const auto finished = parser_.finish();
        return finished.has_value() ? succeeded() : failed(finished.error());
    }

private:
    static wd::stream_options options(const std::string& framing, bool skip_errors) {
        wd::stream_options out;
        if (framing == "lines") {
            out.framing = wd::framing::lines;
        } else if (framing == "rfc7464") {
            out.framing = wd::framing::rfc7464;
        }
        out.on_error = skip_errors ? wd::on_error::skip : wd::on_error::stop;
        return out;
    }

    wd::basic_stream_parser<wd::default_traits> parser_;
};

native_document* parse_text(const std::string& text) {
    return new native_document(native_document::parse(text));
}

// The bytes of the wasm heap the allocator has handed out and not taken back: the tests of the
// garbage collector read it to see the memory of a document it frees.
double heap_bytes_in_use() {
    return static_cast<double>(mallinfo().uordblks);
}

// Opens the bytes in place when they are a 16-aligned view of wasm memory, else from a copy.
native_document* open_bytes(const val& bytes) {
    const auto length = bytes["byteLength"].as<std::size_t>();
    const auto offset = bytes["byteOffset"].as<std::size_t>();
    if (bytes["buffer"].strictlyEquals(wasm_memory_buffer()) && offset % 16 == 0) {
        return new native_document(
            native_document::open_in_place(static_cast<std::uintptr_t>(offset), length));
    }
    return new native_document(native_document::open_copy(bytes, length));
}

}  // namespace

EMSCRIPTEN_BINDINGS(webcpp_document) {
    wd::binding::register_outcome();

    emscripten::class_<native_document>("NativeDocument")
        .function("code", &native_document::code)
        .function("message", &native_document::message)
        .function("has", &native_document::has)
        .function("byteLength", &native_document::byte_length)
        .function("image", &native_document::image)
        .function("get", &native_document::get)
        .function("set", &native_document::set)
        .function("erase", &native_document::erase)
        .function("push", &native_document::push)
        .function("splice", &native_document::splice)
        .function("length", &native_document::length)
        .function("getDecimal", &native_document::get_decimal)
        .function("keysInRange", &native_document::keys_in_range)
        .function("stringify", &native_document::stringify)
        .function("seal", &native_document::seal)
        .function("sealed", &native_document::sealed)
        .function("capacity", &native_document::capacity)
        .function("reserve", &native_document::reserve)
        .function("pop", &native_document::pop)
        .function("shift", &native_document::shift)
        .function("unshift", &native_document::unshift)
        .function("reverse", &native_document::reverse)
        .function("setLength", &native_document::set_length)
        .function("assign", &native_document::assign);

    emscripten::class_<native_stream>("NativeStream")
        .constructor<const std::string&, bool>()
        .function("write", &native_stream::write)
        .function("next", &native_stream::next, emscripten::return_value_policy::take_ownership())
        .function("finish", &native_stream::finish);

    emscripten::function("parseText", &parse_text,
                         emscripten::return_value_policy::take_ownership());
    emscripten::function("openBytes", &open_bytes,
                         emscripten::return_value_policy::take_ownership());
    emscripten::function("heapBytesInUse", &heap_bytes_in_use);
}
