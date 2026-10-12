// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The binary image of a document: its 64-byte header, which says what the bytes after it are, and
 the check that refuses bytes that are not an image this build reads.
*/

#ifndef WEBCPP_DOCUMENT_IMAGE_HPP
#define WEBCPP_DOCUMENT_IMAGE_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/errors.hpp>

#include <boost/version.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace webcpp::document {

/** The version of the image's format; a change to its layout raises it. */
inline constexpr std::uint32_t format_version = 1;

/** The first eight bytes of every image: `WCPPDOC` and a zero byte. */
inline constexpr std::array<char, 8> image_magic{'W', 'C', 'P', 'P', 'D', 'O', 'C', '\0'};

/** The byte-order mark of the header, as the machine that wrote it writes this number. */
inline constexpr std::uint32_t byte_order_mark = 0x01020304;

/**
 The 64 bytes at the start of every image, in the byte order of the machine that wrote it.

 The heap follows it, at offset 64.
*/
struct image_header {
    /** @ref image_magic. */
    std::array<char, 8> magic;

    /** @ref format_version. */
    std::uint32_t version;

    /** @ref byte_order_mark, as the writer's machine writes it. */
    std::uint32_t byte_order;

    /** The `BOOST_VERSION` of the writer. */
    std::uint32_t boost_version;

    /**
     A hash of the node's, the container's and the heap's sizes and alignments, and of the traits.
    */
    std::uint32_t layout;

    /** The size of the whole image, header included. */
    std::uint64_t size;

    /** The offset of the tree's container from the image's start. */
    std::uint64_t container;

    /** The id the next node created takes; ids are never reused. */
    std::uint64_t next_id;

    /** Zero. */
    std::array<std::byte, 16> reserved;
};

static_assert(sizeof(image_header) == detail::header_bytes, "the header is the heap's offset");

/**
 Checks that bytes are an image this build reads, reading nothing past the header before its size
 is known.

 The magic is checked first, then the format version and the byte order, before the size, which an
 image of another version or byte order states in another way; then the Boost version and the
 layout.

 @param bytes The bytes, at any address.
 @param layout The layout tag this build expects.
 @return Success; @ref errc::not_a_document for bytes shorter than the header or than the size it
 states, a size below 64, or another magic; @ref errc::incompatible_document for another format
 version, byte order, Boost version or layout.
*/
inline result<void> check_image(std::span<const std::byte> bytes, std::uint32_t layout) noexcept {
    if (bytes.size() < sizeof(image_header)) {
        return errc::not_a_document;
    }
    // Copied, not cast: the bytes may be at any address.
    image_header header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    if (header.magic != image_magic) {
        return errc::not_a_document;
    }
    if (header.version != format_version || header.byte_order != byte_order_mark) {
        return errc::incompatible_document;
    }
    if (header.size < sizeof(image_header) || header.size > bytes.size()) {
        return errc::not_a_document;
    }
    if (header.boost_version != BOOST_VERSION || header.layout != layout) {
        return errc::incompatible_document;
    }
    return {};
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_IMAGE_HPP
