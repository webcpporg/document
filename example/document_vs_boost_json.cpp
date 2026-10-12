// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What a webcpp::document does that Boost.JSON does not, side by side: each section runs the same
// JSON through both libraries and prints both outputs. The document's line is what JavaScript gives
// where the section says nothing else; Boost.JSON's line is what that library gives.

// tag::includes[]
#include <webcpp/document.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace wd = webcpp::document;
namespace json = boost::json;
// end::includes[]

namespace {

// tag::helpers[]
/** Prints the document's line of a section, under the label that names it. */
void print_document(std::string_view text) {
    std::cout << "  document:   " << text << '\n';
}

/** Prints the Boost.JSON line of a section, under the label that names it. */
void print_boost(std::string_view text) {
    std::cout << "  Boost.JSON: " << text << '\n';
}

/** The text Boost.JSON writes for `text` once parsed, or the error it reports without throwing. */
std::string boost_text(std::string_view text) {
    boost::system::error_code ec;
    const json::value v = json::parse(text, ec);
    return ec ? ec.message() : json::serialize(v);
}

// end::helpers[]

/** Key order: the document writes keys as JavaScript does, Boost.JSON in the order they came. */
bool key_order() {
    // tag::key_order[]
    constexpr std::string_view text = R"({"b":1,"2":2,"a":3,"1":4})";
    wd::result<wd::document> made = wd::parse(text);
    if (made.has_error()) {
        return false;
    }
    std::cout << "1. Key order\n";
    print_document(made->stringify());
    print_boost(boost_text(text));
    // end::key_order[]
    return true;
}

/** Erasing a member: the document keeps the order of the others, Boost.JSON moves the last one. */
bool erase_keeps_order() {
    // tag::erase[]
    constexpr std::string_view text = R"({"a":1,"b":2,"c":3,"d":4})";
    wd::result<wd::document> made = wd::parse(text);
    if (made.has_error() || !made->root().erase("a")) {
        return false;
    }
    std::cout << "2. Erase keeps the order of the other members\n";
    print_document(made->stringify());

    // Boost.JSON's erase copies the last member into the gap, so d comes before b and c.
    boost::system::error_code ec;
    json::value v = json::parse(text, ec);
    json::object* object = v.if_object();
    if (ec || object == nullptr) {
        return false;
    }
    object->erase("a");
    print_boost(json::serialize(v));
    // end::erase[]
    return true;
}

/** Numbers: the document keeps each one as it was written, Boost.JSON stores a double. */
bool exact_numbers() {
    // tag::numbers[]
    constexpr std::string_view text =
        R"([0.1000000000000000000000000000000001,12345678901234567890123])";
    wd::result<wd::document> made = wd::parse(text);
    if (made.has_error()) {
        return false;
    }
    // The digits are the ones read; 23 of them are written in JavaScript's exponent form.
    std::cout << "3. Exact numbers\n";
    print_document(made->stringify());
    print_boost(boost_text(text));
    // end::numbers[]
    return true;
}

/** Array methods: unshift and splice, which Boost.JSON's array does not have. */
bool array_methods() {
    // tag::arrays[]
    wd::result<wd::document> made = wd::parse("[1,2,3,4]");
    if (made.has_error()) {
        return false;
    }
    wd::value a = made->root();
    const wd::result<std::uint64_t> length = a.unshift(0);
    // splice removes two elements from index 1, returns them, and inserts "x" in their place.
    const wd::result<wd::document> removed = a.splice(1, 2, "x");
    if (length.has_error() || removed.has_error()) {
        return false;
    }
    std::cout << "4. Array methods\n";
    print_document("unshift(0) -> " + std::to_string(*length) + "; splice(1, 2, \"x\") removes " +
                   removed->stringify() + "; a = " + a.stringify());

    // Boost.JSON's array has insert and erase, which make the same edit but return no removed
    // values.
    json::array arr{1, 2, 3, 4};
    arr.insert(arr.begin(), 0);
    arr.erase(arr.begin() + 1, arr.begin() + 3);
    arr.insert(arr.begin() + 1, json::value("x"));
    print_boost(
        "no splice or unshift; insert and erase make the edit, without the removed "
        "values: a = " +
        json::serialize(arr));
    // end::arrays[]
    return true;
}

/** An image: the document's bytes are copied to a buffer and opened there; Boost.JSON cannot. */
bool image_in_place() {
    // tag::image[]
    constexpr std::string_view text = R"({"user":"Ada","langs":["Analytical","Note G"]})";
    wd::result<wd::document> made = wd::parse(text);
    if (made.has_error()) {
        return false;
    }
    const std::span<const std::byte> image = made->image();
    // A buffer of the program's, 16-aligned, large enough for the image.
    alignas(16) std::array<std::byte, 8192> buffer{};
    if (image.size() > buffer.size()) {
        return false;
    }
    std::ranges::copy(image, buffer.begin());
    // Opened in place: the buffer becomes the document, with no parse.
    wd::result<wd::basic_document<wd::default_traits, wd::external>> opened =
        wd::open(std::span<std::byte>(buffer.data(), image.size()));
    if (opened.has_error()) {
        return false;
    }
    std::cout << "5. An image copied to another buffer and opened in place\n";
    print_document(opened->stringify());
    // A Boost.JSON value points into its memory resource, so its bytes cannot be copied: the
    // only way to move it is to serialize it and parse it again.
    print_boost(boost_text(made->stringify()));
    // end::image[]
    return true;
}

/** Repeated keys: the last value, in the place of the first, as JavaScript does. */
bool repeated_keys() {
    // tag::repeated[]
    constexpr std::string_view text = R"({"b":1,"2":2,"b":3,"1":4})";
    wd::result<wd::document> made = wd::parse(text);
    if (made.has_error()) {
        return false;
    }
    std::cout << "6. Repeated keys\n";
    print_document(made->stringify());
    print_boost(boost_text(text));
    // end::repeated[]
    return true;
}

/** Errors: each operation returns its error, where Boost.JSON's parse reports one by error_code. */
bool errors_are_values() {
    // tag::errors[]
    // The document's operations return result; neither a bad parse nor a bad write throws.
    const wd::result<wd::document> broken = wd::parse("[1,");
    wd::result<wd::document> number = wd::parse("5");
    if (broken.has_value() || number.has_error()) {
        return false;
    }
    const wd::result<std::uint64_t> pushed = number->root().push(1);
    std::cout << "7. Errors are values\n";
    print_document("parse(\"[1,\") -> " + broken.error().message() + "; push on a number -> " +
                   pushed.error().message());

    // Boost.JSON's parse reports through error_code, but its value accessors (at, as_string)
    // throw, so this section stays with the error_code overload.
    boost::system::error_code ec;
    static_cast<void>(json::parse("[1,", ec));
    print_boost("parse(\"[1,\", ec) -> " + ec.message());
    // end::errors[]
    return true;
}

}  // namespace

int main() {
    if (!key_order() || !erase_keeps_order() || !exact_numbers() || !array_methods() ||
        !image_in_place() || !repeated_keys() || !errors_are_values()) {
        std::cout << "error: a section failed\n";
        return 1;
    }
}
