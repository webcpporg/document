// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A document's binary image: its bytes, the same on every target, copied into a new document;
// a document formatted in a buffer the program owns, filled until it is full and opened again in
// place; bytes refused as no image or as one of other traits; and an image shrunk to its content.

#include <webcpp/document.hpp>

#include <boost/hash2/sha2.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

/** The first eight bytes of the SHA-256 of `bytes`, in hexadecimal. */
std::string digest_of(std::span<const std::byte> bytes) {
    boost::hash2::sha2_256 h;
    h.update(bytes.data(), bytes.size());
    return boost::hash2::to_string(h.result()).substr(0, 16);
}

bool images() {
    // tag::image[]
    wd::result<wd::document> made = wd::parse(R"({"user":"Ada","langs":["Analytical","Note G"]})");
    if (made.has_error()) {
        return false;
    }
    const std::span<const std::byte> image = made->image();
    std::cout << image.size() << " bytes, sha-256 " << digest_of(image) << "...\n";

    // An owned copy, at another address, which grows as any owned document does.
    wd::result<wd::document> copy = wd::from_image(image);
    if (copy.has_error() || !(*copy)["user"].set("Lovelace")) {
        return false;
    }
    std::cout << copy->stringify() << '\n' << made->stringify() << '\n';
    // end::image[]

    // tag::refused[]
    std::cout << "16 zero bytes: " << wd::from_image(std::array<std::byte, 16>{}).error().message()
              << '\n';
    std::cout << "other traits: " << wd::from_image<wd::js_traits>(image).error().message() << '\n';
    // end::refused[]
    return true;
}

bool external() {
    // tag::external[]
    // A buffer the program owns, such as a browser's memory, 16-aligned.
    alignas(16) std::array<std::byte, 8192> buffer{};
    using external_document = wd::basic_document<wd::default_traits, wd::external>;
    wd::result<external_document> formatted = wd::format(buffer);
    if (formatted.has_error() || !formatted->root().set(wd::array{})) {
        return false;
    }
    wd::basic_value<wd::default_traits, wd::external> list = formatted->root();
    std::uint64_t pushed = 0;
    wd::result<std::uint64_t> length = list.push("an element of the list");
    for (; length.has_value(); length = list.push("an element of the list")) {
        ++pushed;
    }
    const std::string before = formatted->stringify();
    std::cout << pushed << " pushed, then: " << length.error().message() << '\n';
    // A failed operation changes nothing of the content.
    std::cout << "unchanged: " << std::boolalpha
              << (list.splice(0, 1, wd::array{1, 2, 3}).error() == wd::errc::capacity) << ' '
              << (formatted->stringify() == before) << '\n';

    // The buffer holds the image, which another document opens in place.
    wd::result<external_document> opened = wd::open(buffer);
    if (opened.has_error()) {
        return false;
    }
    std::cout << "opened in place: " << opened->root().size() << " elements\n";
    // end::external[]
    return true;
}

bool shrink() {
    // tag::shrink[]
    wd::result<wd::document> made = wd::parse("{}");
    if (made.has_error()) {
        return false;
    }
    for (std::uint64_t i = 0; i < 1000; ++i) {
        if (!(*made)[std::to_string(i)].set(std::string(100, 'x'))) {
            return false;
        }
    }
    const std::uint64_t grown = made->image().size();
    if (!made->root().set(wd::object{{"kept", true}}) || !made->shrink_to_fit()) {
        return false;
    }
    std::cout << "grown to " << grown << " bytes, shrunk to " << made->image().size() << ": "
              << made->stringify() << '\n';

    // Blocks never move: a deep copy into a new document packs the content.
    wd::result<wd::document> packed = wd::document::create();
    if (packed.has_error() || !packed->root().set(*made)) {
        return false;
    }
    std::cout << "packed: " << packed->image().size() << " bytes\n";
    // end::shrink[]
    return true;
}

}  // namespace

int main() {
    if (!images() || !external() || !shrink()) {
        std::cout << "error\n";
        return 1;
    }
}
