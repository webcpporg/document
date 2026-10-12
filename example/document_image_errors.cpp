// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What opening an image refuses, and how a program tells the cases apart: each refusal prints its
// errc and its message; a changed byte of a sealed image is refused, and verify::none accepts it;
// an old image left in reused memory is refused once it is sealed; and a sealed image opens.

#include <webcpp/document.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>

namespace wd = webcpp::document;

namespace {

/** The errc of a failure of document's category, by its name, such as `not_a_document`. */
std::string_view name_of(boost::system::error_code code) {
    if (code.category() != wd::error_category()) {
        return "other";
    }
    switch (static_cast<wd::errc>(code.value())) {
        case wd::errc::syntax: return "syntax";
        case wd::errc::type: return "type";
        case wd::errc::range: return "range";
        case wd::errc::capacity: return "capacity";
        case wd::errc::not_a_document: return "not_a_document";
        case wd::errc::incompatible_document: return "incompatible_document";
        case wd::errc::damaged_document: return "damaged_document";
    }
    return "unknown";
}

/** Prints the errc and the message of a refused `outcome`; false when it was not refused. */
template <class T>
bool refused(std::string_view label, const wd::result<T>& outcome) {
    if (outcome.has_value()) {
        return false;
    }
    std::cout << label << ": " << name_of(outcome.error()) << ", " << outcome.error().message()
              << '\n';
    return true;
}

/** Copies `image` to the start of `buffer` and returns the copy, as aligned as `buffer` is. */
std::span<std::byte> copy_into(std::span<std::byte> buffer, std::span<const std::byte> image) {
    std::ranges::copy(image, buffer.begin());
    return buffer.first(image.size());
}

/** Writes `value` into `image` at `offset`, as the writing machine lays a header field out. */
template <class T>
void set_field(std::span<std::byte> image, std::size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof value);
}

/** The refusals of bytes that are not an image a program can open, each of its own kind. */
bool refusals() {
    wd::result<wd::document> made = wd::parse(R"({"user":"Ada","langs":["Analytical","Note G"]})");
    if (made.has_error()) {
        return false;
    }
    const std::span<const std::byte> image = made->image();
    alignas(16) std::array<std::byte, 8192> storage{};

    // tag::layout[]
    // An image written with default_traits is no image of js_traits: the layout tag differs.
    if (!refused("js_traits", wd::from_image<wd::js_traits>(image))) {
        return false;
    }
    // end::layout[]

    // tag::version[]
    // The format version is the header's field at offset 8; another version is not read.
    std::span<std::byte> bytes = copy_into(storage, image);
    set_field(bytes, offsetof(wd::image_header, version), std::uint32_t{2});
    if (!refused("format version 2", wd::open(bytes))) {
        return false;
    }
    // end::version[]

    // tag::garbage[]
    // Bytes that are not an image, and are longer than a header, have no magic.
    constexpr std::string_view text =
        "this text is not an image, and it is longer than the 64 bytes of a header";
    if (!refused("garbage", wd::from_image(std::as_bytes(std::span(text))))) {
        return false;
    }
    // end::garbage[]

    // tag::zeroed[]
    // Memory that was never written reads as zeros, which are no magic either.
    alignas(16) std::array<std::byte, 256> zeros{};
    if (!refused("zeroed memory", wd::open(zeros))) {
        return false;
    }
    // end::zeroed[]

    // tag::misaligned[]
    // open reads the image in place, so its buffer must be 16-aligned: this one starts 8 bytes in.
    bytes = copy_into(std::span<std::byte>(storage).subspan(8), image);
    if (!refused("misaligned buffer", wd::open(bytes))) {
        return false;
    }
    // end::misaligned[]

    // tag::truncated[]
    // Cut short, an image that is not sealed is no image at all.
    bytes = copy_into(storage, image);
    if (!refused("truncated, unsealed", wd::open(bytes.first(bytes.size() - 16)))) {
        return false;
    }
    // end::truncated[]

    // tag::size[]
    // A header that states a size larger than the bytes it is given is refused before it is read.
    bytes = copy_into(storage, image);
    set_field(bytes, offsetof(wd::image_header, size), std::uint64_t{image.size() + 4096});
    if (!refused("size", wd::open(bytes.first(image.size())))) {
        return false;
    }
    // end::size[]

    // tag::container[]
    // A header that names a container outside the heap is refused before anything is read there.
    bytes = copy_into(storage, image);
    set_field(bytes, offsetof(wd::image_header, container), std::uint64_t{image.size()});
    return refused("container offset", wd::open(bytes));
    // end::container[]
}

/** A sealed image cut short, changed or written over, refused; and the same image accepted. */
bool damage() {
    alignas(16) std::array<std::byte, 8192> storage{};
    wd::result<wd::document> made = wd::parse(R"({"user":"Ada","langs":["Analytical","Note G"]})");
    if (made.has_error()) {
        return false;
    }
    if (made->seal().has_error()) {
        return false;
    }

    // tag::truncated[]
    // Cut short, a sealed image is a torn write: damaged, not merely no image.
    std::span<std::byte> bytes = copy_into(storage, made->image());
    if (!refused("truncated, sealed", wd::open(bytes.first(bytes.size() - 16)))) {
        return false;
    }
    // end::truncated[]

    // tag::damaged[]
    // A sealed image with one byte changed is refused as damaged, by open and by from_image.
    const auto found =
        std::ranges::search(bytes, std::as_bytes(std::span(std::string_view("Ada"))));
    if (found.empty()) {
        return false;
    }
    found.front() = std::byte{'a'};
    if (!refused("one byte changed", wd::open(bytes))) {
        return false;
    }
    // end::damaged[]

    // tag::unverified[]
    // verify::none skips the seal: the caller trusts the source, and the changed byte reads as is.
    wd::result<wd::document> trusted =
        wd::from_image(bytes, wd::default_max_bytes, wd::verify::none);
    if (trusted.has_error()) {
        return false;
    }
    std::cout << "verify::none, accepted: " << trusted->stringify() << '\n';
    // end::unverified[]

    // tag::reused[]
    // An old sealed image left in memory, which a later use wrote over in part, is refused as
    // damaged: the seal shows that the bytes changed since the image was written.
    bytes = copy_into(storage, made->image());
    std::ranges::fill(bytes.subspan(2048, 64), std::byte{0xAA});
    if (!refused("reused memory", wd::open(bytes))) {
        return false;
    }
    // end::reused[]

    // tag::opened[]
    // An intact sealed image opens in place, and is still sealed.
    bytes = copy_into(storage, made->image());
    wd::result<wd::basic_document<wd::default_traits, wd::external>> opened = wd::open(bytes);
    if (opened.has_error()) {
        return false;
    }
    std::cout << "opened, sealed " << std::boolalpha << opened->sealed() << ": "
              << opened->stringify() << '\n';
    // end::opened[]
    return true;
}

}  // namespace

int main() {
    if (!refusals() || !damage()) {
        std::cout << "error\n";
        return 1;
    }
}
