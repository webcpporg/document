// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The Emscripten side of webcpp::document's JavaScript API. It exports the classes and functions
// that webcpp-document.mjs wraps with JavaScript's own values and errors: every operation returns
// an Outcome, a code and a message or the value it gives, and nothing here throws into JavaScript.

#include <webcpp/document.hpp>

#include <emscripten/bind.h>
#include <emscripten/val.h>

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
using emscripten::val;
using owned_document = wd::basic_document<wd::default_traits, wd::owned>;
using external_document = wd::basic_document<wd::default_traits, wd::external>;

// The result of an operation: code 0 with its value, or the error's code and message.
struct Outcome {
    int code = 0;
    std::string message;
    val value = val::undefined();
};

Outcome succeeded(val value = val::undefined()) {
    Outcome out;
    out.value = std::move(value);
    return out;
}

Outcome failed(boost::system::error_code error) {
    Outcome out;
    out.code = error.value();
    out.message = error.message();
    return out;
}

Outcome failed(wd::errc error) { return failed(wd::make_error_code(error)); }

// The outcome of an operation whose result has no value.
Outcome outcome_of(const wd::result<void>& done) {
    return done.has_value() ? succeeded() : failed(done.error());
}

// One step of a path, validated by the JavaScript side: a key of an object, or an index of an
// array.
struct step {
    bool is_index = false;
    std::string key;
    std::uint64_t index = 0;
};

std::vector<step> steps_of(const val& path) {
    std::vector<step> steps;
    const auto count = path["length"].as<std::size_t>();
    steps.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const val element = path[i];
        if (element.typeOf().as<std::string>() == "number") {
            steps.push_back({true, {}, static_cast<std::uint64_t>(element.as<double>())});
        } else {
            steps.push_back({false, element.as<std::string>(), 0});
        }
    }
    return steps;
}

// The handle that the steps lead to, read or written through. A step that finds nothing gives a
// handle to nothing, which reads as undefined and refuses a write.
template <class Handle>
Handle walk(Handle handle, std::span<const step> path) {
    if (path.empty()) {
        return handle;
    }
    const step& next = path.front();
    return walk(next.is_index ? handle[next.index] : handle[std::string_view(next.key)],
                path.subspan(1));
}

// The JavaScript value of a read-only handle: scalars as their JavaScript values, a number as
// Number(text) of its exact digits, a container as a copy made by JSON.parse of its text.
template <class Handle>
val to_javascript(const Handle& handle) {
    switch (handle.kind()) {
        case wd::kind::undefined:
            return val::undefined();
        case wd::kind::null:
            return val::null();
        case wd::kind::boolean:
            return val(handle.as_bool());
        case wd::kind::string:
            return val(std::string(handle.as_string()));
        case wd::kind::number:
            return val::global("Number")(val(handle.stringify(0)));
        default:
            return val::global("JSON").call<val>("parse", val(handle.stringify(0)));
    }
}

// A Uint8Array sharing the bytes at their address in wasm memory.
val view_of(std::span<const std::byte> bytes) {
    return val(emscripten::typed_memory_view(bytes.size(),
                                             reinterpret_cast<const std::uint8_t*>(bytes.data())));
}

// The buffer of wasm memory, which a view of address 0 names.
val wasm_memory_buffer() {
    return val(emscripten::typed_memory_view(0, static_cast<const std::uint8_t*>(nullptr)))["buffer"];
}

// The NativeDocument objects alive, counted by every construction and destruction (a move counts
// as both, so it nets to none). A debugging count for the tests of the garbage collector, which
// frees a document it finds unreachable: it is the only mutable state in this file.
std::int64_t live_native_documents = 0;

struct live_counted {
    live_counted() { ++live_native_documents; }
    live_counted(const live_counted&) { ++live_native_documents; }
    live_counted& operator=(const live_counted&) = default;
    ~live_counted() { --live_native_documents; }
};

// A document of either storage: owned, or external over the bytes it was opened from. Empty until
// it holds one, as a stream's next() gives it when no document is complete yet.
class NativeDocument : private live_counted {
public:
    NativeDocument() = default;

    explicit NativeDocument(boost::system::error_code error)
        : code_(error.value()), message_(error.message()) {}

    explicit NativeDocument(wd::errc error) : NativeDocument(wd::make_error_code(error)) {}

    explicit NativeDocument(owned_document document) : storage_(std::move(document)) {}

    NativeDocument(external_document document, void* copy)
        : storage_(std::move(document)), copy_(copy) {}

    NativeDocument(const NativeDocument&) = delete;
    NativeDocument& operator=(const NativeDocument&) = delete;
    NativeDocument& operator=(NativeDocument&&) = delete;

    NativeDocument(NativeDocument&& other) noexcept
        : live_counted(other),
          code_(other.code_),
          message_(std::move(other.message_)),
          storage_(std::move(other.storage_)),
          copy_(std::exchange(other.copy_, nullptr)) {}

    // Frees the copy in memory of a document opened from bytes outside it, once the document
    // that reads it is gone.
    ~NativeDocument() { std::free(copy_); }

    [[nodiscard]] int code() const { return code_; }

    [[nodiscard]] std::string message() const { return message_; }

    [[nodiscard]] bool has() const { return storage_.index() != 0; }

    // A JavaScript number, not a BigInt: an image is far below 2^53 bytes.
    [[nodiscard]] double byte_length() const {
        return with_document(
            [](const auto& document) { return static_cast<double>(document.image().size()); }, 0.0);
    }

    [[nodiscard]] val image() const {
        return with_document(
            [](const auto& document) { return view_of(document.image()); },
            view_of({}));
    }

    Outcome get(const val& path) const {
        const auto steps = steps_of(path);
        return with_document(
            [&](const auto& document) { return succeeded(to_javascript(walk(document.root(), steps))); },
            failed(wd::errc::type));
    }

    Outcome set(const val& path, const val& json) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return set_outcome(document, steps, json); });
    }

    Outcome erase(const val& path) {
        const auto steps = steps_of(path);
        if (steps.empty() || steps.back().is_index) {
            return failed(wd::errc::type);
        }
        const std::span<const step> parent = std::span(steps).first(steps.size() - 1);
        const std::string key = steps.back().key;
        return with_mutable_document([&](auto& document) {
            auto erased = walk(document.root(), parent).erase(std::string_view(key));
            return erased.has_value() ? succeeded(val(erased.unsafe_value())) : failed(erased.error());
        });
    }

    Outcome push(const val& path, const std::string& json) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return push_outcome(document, steps, json); });
    }

    Outcome splice(const val& path, double start, double count, const val& items) {
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

    Outcome length(const val& path) const {
        const auto steps = steps_of(path);
        return with_document(
            [&](const auto& document) {
                auto length = walk(document.root(), steps).length();
                return length.has_value() ? succeeded(val(static_cast<double>(length.unsafe_value())))
                                          : failed(length.error());
            },
            failed(wd::errc::type));
    }

    Outcome get_decimal(const val& path) const {
        const auto steps = steps_of(path);
        return with_document(
            [&](const auto& document) {
                const auto handle = walk(document.root(), steps);
                return handle.kind() == wd::kind::number ? succeeded(val(handle.stringify(0)))
                                                         : failed(wd::errc::type);
            },
            failed(wd::errc::type));
    }

    Outcome keys_in_range(const val& path, const std::string& low, const std::string& high) const {
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
        return with_document(
            [&](const auto& document) { return document.stringify(indent); }, std::string());
    }

    Outcome seal() {
        return with_mutable_document([](auto& document) { return outcome_of(document.seal()); });
    }

    [[nodiscard]] bool sealed() const {
        return with_document([](const auto& document) { return document.sealed(); }, false);
    }

    [[nodiscard]] double capacity() const {
        return with_document(
            [](const auto& document) { return static_cast<double>(document.capacity()); }, 0.0);
    }

    Outcome reserve(double bytes) {
        return with_mutable_document([&](auto& document) {
            return outcome_of(document.reserve(static_cast<std::uint64_t>(bytes)));
        });
    }

    Outcome pop(const val& path) {
        const auto steps = steps_of(path);
        return with_mutable_document([&](auto& document) {
            return removed_outcome(walk(document.root(), steps).pop());
        });
    }

    Outcome shift(const val& path) {
        const auto steps = steps_of(path);
        return with_mutable_document([&](auto& document) {
            return removed_outcome(walk(document.root(), steps).shift());
        });
    }

    Outcome unshift(const val& path, const std::string& json) {
        const auto steps = steps_of(path);
        return with_mutable_document([&](auto& document) {
            auto parsed = wd::parse(json);
            if (!parsed.has_value()) {
                return failed(parsed.error());
            }
            const owned_document& source = parsed.unsafe_value();
            auto length = walk(document.root(), steps).unshift(source.root());
            return length.has_value() ? succeeded(val(static_cast<double>(length.unsafe_value())))
                                      : failed(length.error());
        });
    }

    Outcome reverse(const val& path) {
        const auto steps = steps_of(path);
        return with_mutable_document(
            [&](auto& document) { return outcome_of(walk(document.root(), steps).reverse()); });
    }

    Outcome set_length(const val& path, double length) {
        const auto steps = steps_of(path);
        return with_mutable_document([&](auto& document) {
            return outcome_of(
                walk(document.root(), steps).set_length(static_cast<std::uint64_t>(length)));
        });
    }

    // Object.assign of the JSON text's value into the object the path names.
    Outcome assign(const val& path, const std::string& json) {
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
    static NativeDocument open_in_place(std::uintptr_t address, std::size_t length) {
        auto opened = wd::open(std::span<std::byte>(reinterpret_cast<std::byte*>(address), length));
        if (!opened.has_value()) {
            return NativeDocument(opened.error());
        }
        return NativeDocument(std::move(opened.unsafe_value()), nullptr);
    }

    // Opened from a copy of the bytes made in wasm memory, which this document frees.
    static NativeDocument open_copy(const val& bytes, std::size_t length) {
        const std::size_t capacity = std::max<std::size_t>(16, (length + 15) / 16 * 16);
        void* copy = std::aligned_alloc(16, capacity);
        if (copy == nullptr) {
            return NativeDocument(wd::errc::capacity);
        }
        val target(emscripten::typed_memory_view(length, static_cast<std::uint8_t*>(copy)));
        target.call<void>("set", bytes);
        auto opened = wd::open(std::span<std::byte>(static_cast<std::byte*>(copy), length));
        if (!opened.has_value()) {
            std::free(copy);
            return NativeDocument(opened.error());
        }
        return NativeDocument(std::move(opened.unsafe_value()), copy);
    }

    static NativeDocument parse(const std::string& text) {
        auto parsed = wd::parse(text);
        if (!parsed.has_value()) {
            return NativeDocument(parsed.error());
        }
        return NativeDocument(std::move(parsed.unsafe_value()));
    }

    static NativeDocument from_document(owned_document document) {
        return NativeDocument(std::move(document));
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
    Outcome with_mutable_document(Operation&& operation) {
        if (auto* owned = std::get_if<owned_document>(&storage_)) {
            return operation(*owned);
        }
        if (auto* external = std::get_if<external_document>(&storage_)) {
            return operation(*external);
        }
        return failed(wd::errc::type);
    }

    // The element pop or shift removed, as a JavaScript value: undefined from an empty array.
    static Outcome removed_outcome(const wd::result<owned_document>& removed) {
        if (!removed.has_value()) {
            return failed(removed.error());
        }
        return succeeded(to_javascript(removed.unsafe_value().root()));
    }

    template <class Document>
    static Outcome set_outcome(Document& document, const std::vector<step>& steps, const val& json) {
        auto place = walk(document.root(), steps);
        if (json.isUndefined()) {
            return outcome_of(place.set(wd::undefined));
        }
        auto parsed = wd::parse(json.as<std::string>());
        if (!parsed.has_value()) {
            return failed(parsed.error());
        }
        const owned_document& source = parsed.unsafe_value();
        return outcome_of(place.set(source.root()));
    }

    template <class Document>
    static Outcome push_outcome(Document& document, const std::vector<step>& steps,
                                const std::string& json) {
        auto parsed = wd::parse(json);
        if (!parsed.has_value()) {
            return failed(parsed.error());
        }
        const owned_document& source = parsed.unsafe_value();
        auto pushed = walk(document.root(), steps).push(source.root());
        return pushed.has_value() ? succeeded(val(static_cast<double>(pushed.unsafe_value())))
                                  : failed(pushed.error());
    }

    // The first inserted value goes in with the removal, and each next one after the one before it.
    template <class Document>
    static Outcome splice_outcome(Document& document, const std::vector<step>& steps,
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
            auto added = target.splice(start + static_cast<std::int64_t>(i), 0,
                                       std::as_const(inserted[i]).root());
            if (!added.has_value()) {
                return failed(added.error());
            }
        }
        const owned_document& gone = removed.unsafe_value();
        return succeeded(to_javascript(gone.root()));
    }

    int code_ = 0;
    std::string message_;
    std::variant<std::monostate, owned_document, external_document> storage_;
    void* copy_ = nullptr;
};

// A stream of documents: bytes in, and each document as it completes, taken by next().
class NativeStream {
public:
    NativeStream(const std::string& framing, bool skip_errors)
        : parser_(options(framing, skip_errors)) {}

    NativeStream(const NativeStream&) = delete;
    NativeStream& operator=(const NativeStream&) = delete;

    Outcome write(const val& chunk) {
        const auto length = chunk["byteLength"].as<std::size_t>();
        std::string bytes(length, '\0');
        val view(emscripten::typed_memory_view(length, reinterpret_cast<std::uint8_t*>(bytes.data())));
        view.call<void>("set", chunk);
        const auto written = parser_.write(bytes);
        return written.has_value() ? succeeded() : failed(written.error());
    }

    // The next complete document, or an empty one when none is complete; JavaScript owns it.
    NativeDocument* next() {
        auto taken = parser_.next();
        if (!taken.has_value()) {
            return new NativeDocument(taken.error());
        }
        if (!taken.unsafe_value().has_value()) {
            return new NativeDocument();
        }
        return new NativeDocument(NativeDocument::from_document(std::move(*taken.unsafe_value())));
    }

    Outcome finish() {
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

NativeDocument* parse_text(const std::string& text) {
    return new NativeDocument(NativeDocument::parse(text));
}

double live_documents() { return static_cast<double>(live_native_documents); }

// Opens the bytes in place when they are a 16-aligned view of wasm memory, else from a copy.
NativeDocument* open_bytes(const val& bytes) {
    const auto length = bytes["byteLength"].as<std::size_t>();
    const auto offset = bytes["byteOffset"].as<std::size_t>();
    if (bytes["buffer"].strictlyEquals(wasm_memory_buffer()) && offset % 16 == 0) {
        return new NativeDocument(
            NativeDocument::open_in_place(static_cast<std::uintptr_t>(offset), length));
    }
    return new NativeDocument(NativeDocument::open_copy(bytes, length));
}

}  // namespace

EMSCRIPTEN_BINDINGS(webcpp_document) {
    emscripten::value_object<Outcome>("Outcome")
        .field("code", &Outcome::code)
        .field("message", &Outcome::message)
        .field("value", &Outcome::value);

    emscripten::class_<NativeDocument>("NativeDocument")
        .function("code", &NativeDocument::code)
        .function("message", &NativeDocument::message)
        .function("has", &NativeDocument::has)
        .function("byteLength", &NativeDocument::byte_length)
        .function("image", &NativeDocument::image)
        .function("get", &NativeDocument::get)
        .function("set", &NativeDocument::set)
        .function("erase", &NativeDocument::erase)
        .function("push", &NativeDocument::push)
        .function("splice", &NativeDocument::splice)
        .function("length", &NativeDocument::length)
        .function("getDecimal", &NativeDocument::get_decimal)
        .function("keysInRange", &NativeDocument::keys_in_range)
        .function("stringify", &NativeDocument::stringify)
        .function("seal", &NativeDocument::seal)
        .function("sealed", &NativeDocument::sealed)
        .function("capacity", &NativeDocument::capacity)
        .function("reserve", &NativeDocument::reserve)
        .function("pop", &NativeDocument::pop)
        .function("shift", &NativeDocument::shift)
        .function("unshift", &NativeDocument::unshift)
        .function("reverse", &NativeDocument::reverse)
        .function("setLength", &NativeDocument::set_length)
        .function("assign", &NativeDocument::assign);

    emscripten::class_<NativeStream>("NativeStream")
        .constructor<const std::string&, bool>()
        .function("write", &NativeStream::write)
        .function("next", &NativeStream::next, emscripten::return_value_policy::take_ownership())
        .function("finish", &NativeStream::finish);

    emscripten::function("parseText", &parse_text, emscripten::return_value_policy::take_ownership());
    emscripten::function("openBytes", &open_bytes, emscripten::return_value_policy::take_ownership());
    emscripten::function("liveDocuments", &live_documents);
}
