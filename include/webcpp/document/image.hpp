// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The binary image of a document: its 64-byte header, which says what the bytes after it are, its
 seal, and the check that refuses bytes that are not an image this build reads or a sealed image
 whose bytes changed.
*/

#ifndef WEBCPP_DOCUMENT_IMAGE_HPP
#define WEBCPP_DOCUMENT_IMAGE_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/errors.hpp>

#include <boost/crc.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace webcpp::document {

/** The first eight bytes of every image: `WCPPDOC` and a zero byte. */
inline constexpr std::array<char, 8> image_magic{'W', 'C', 'P', 'P', 'D', 'O', 'C', '\0'};

/** The byte-order mark of the header, as the machine that wrote it writes this number. */
inline constexpr std::uint32_t byte_order_mark = 0x01020304;

/**
 The 64 bytes at the start of every image, in the byte order of the machine that wrote it.

 The heap follows it, at offset 64.

 @see "Images", in the guide.
*/
struct image_header {
    /** @ref image_magic. */
    std::array<char, 8> magic;

    /** @ref format_version. */
    std::uint32_t version;

    /** @ref byte_order_mark, as the writer's machine writes it. */
    std::uint32_t byte_order;

    /** Zero, and kept for a future version: an image whose reserved bytes are not zero is refused.
     */
    std::uint32_t reserved_4;

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

    /**
     Zero for an image that is not sealed; else the image's CRC-32C, with these eight bytes taken
     as zero, in the low 32 bits, and the high bit set, which marks the image sealed.
    */
    std::uint64_t seal;

    /** Zero. */
    std::array<std::byte, 8> reserved;
};

static_assert(sizeof(image_header) == detail::header_bytes, "the header is the heap's offset");
static_assert(offsetof(image_header, seal) == 48, "the seal is at offset 48");

/**
 Whether opening an image checks its seal.

 @see "Images", in the guide.
*/
enum class verify : std::uint8_t {
    /**
     A sealed image's bytes are checked against its seal, a pass over the image, and refused with
     @ref errc::damaged_document when they differ; an image that is not sealed is not checked.
    */
    seal,

    /**
     The seal is not checked: for a program that trusts its memory, such as an image it has just
     written itself.
    */
    none,
};

namespace detail {

/** The high bit of a seal, which marks an image sealed; the CRC is in the low 32 bits. */
inline constexpr std::uint64_t sealed_marker = std::uint64_t{1} << 63U;

/** Whether a seal is marked: the marker, and no other bit above the CRC's 32. */
constexpr bool marks_sealed(std::uint64_t seal) noexcept {
    return (seal >> 32U) == (sealed_marker >> 32U);
}

/**
 The seal of an image of at least 64 bytes: the marker and the CRC-32C (Castagnoli) of its bytes,
 the seal's own eight taken as zero.
*/
inline std::uint64_t seal_of(std::span<const std::byte> image) noexcept {
    boost::crc_optimal<32, 0x1EDC6F41, 0xFFFFFFFF, 0xFFFFFFFF, true, true> crc;
    constexpr std::size_t at = offsetof(image_header, seal);
    constexpr std::array<std::byte, sizeof(std::uint64_t)> zero{};
    crc.process_bytes(image.data(), at);
    crc.process_bytes(zero.data(), zero.size());
    crc.process_bytes(image.data() + at + zero.size(), image.size() - at - zero.size());
    return sealed_marker | crc.checksum();
}

/** Writes an image's seal into its header. */
inline void seal_image(std::span<std::byte> image) noexcept {
    const std::uint64_t seal = seal_of(image);
    std::memcpy(image.data() + offsetof(image_header, seal), &seal, sizeof seal);
}

/** Whether the header of an image of at least 64 bytes carries a seal. */
inline bool is_sealed(std::span<const std::byte> image) noexcept {
    std::uint64_t seal = 0;
    std::memcpy(&seal, image.data() + offsetof(image_header, seal), sizeof seal);
    return marks_sealed(seal);
}

}  // namespace detail

/**
 Checks that bytes are an image this build reads, reading nothing past the header before its size
 is known, and that a sealed image's bytes match its seal.

 A seal is checked first, before anything else its header states is trusted, so that a change
 anywhere in a sealed image, its magic and its size included, is reported as damage. Then the
 magic, the format version and the byte order, before the size, which an image of another version
 or byte order states in another way; then the reserved bytes and the layout.

 @param bytes The bytes, at any address.
 @param layout The layout tag this build expects.
 @param check Whether a sealed image's bytes are checked against its seal.
 @return Success; @ref errc::not_a_document for bytes shorter than the header or than the size it
 states, a size below 64, or another magic; @ref errc::incompatible_document for another format
 version, byte order, nonzero reserved bytes or layout; @ref errc::damaged_document for a sealed
 image whose bytes do not match its seal, or are fewer than its size, and for a seal that is neither
 zero nor marked.
*/
inline result<void> check_image(std::span<const std::byte> bytes, std::uint32_t layout,
                                verify check = verify::seal) noexcept {
    if (bytes.size() < sizeof(image_header)) {
        return errc::not_a_document;
    }
    // Copied, not cast: the bytes may be at any address.
    image_header header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    if (check == verify::seal && detail::marks_sealed(header.seal)) {
        // A sealed image cut short is a torn write.
        if (header.size < sizeof(image_header) || header.size > bytes.size()) {
            return errc::damaged_document;
        }
        if (detail::seal_of(bytes.first(static_cast<std::size_t>(header.size))) != header.seal) {
            return errc::damaged_document;
        }
    }
    if (header.magic != image_magic) {
        return errc::not_a_document;
    }
    if (header.version != format_version || header.byte_order != byte_order_mark) {
        return errc::incompatible_document;
    }
    if (header.size < sizeof(image_header) || header.size > bytes.size()) {
        return errc::not_a_document;
    }
    if (header.reserved_4 != 0 || header.layout != layout) {
        return errc::incompatible_document;
    }
    // Only a changed byte makes a seal that is neither zero nor marked: the byte order, which
    // would read a sealed image's seal so, is the image's own.
    if (check == verify::seal && header.seal != 0 && !detail::marks_sealed(header.seal)) {
        return errc::damaged_document;
    }
    return {};
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_IMAGE_HPP
