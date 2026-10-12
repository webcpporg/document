// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The document: one JSON value in one relocatable image, owned and growing, or in a buffer its user
 owns; how a document is made, copied, read from an image and given back as one.
*/

#ifndef WEBCPP_DOCUMENT_DOCUMENT_HPP
#define WEBCPP_DOCUMENT_DOCUMENT_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/image.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/traits.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::document {

/**
 The storage of a document that owns its image: a buffer of its own, 16-aligned, which grows,
 twice the size each time, up to its `max_bytes`.

 @see "Images", in the guide.
*/
struct owned {};

/**
 The storage of a document whose image is a buffer its user owns, such as a browser's memory: when
 it is full, an operation returns @ref errc::capacity and changes nothing.

 @see "Images", in the guide.
*/
struct external {};

/** The largest image an owned document grows to unless told otherwise: 1 GiB. */
inline constexpr std::uint64_t default_max_bytes = std::uint64_t{1} << 30U;

/**
 Formats a buffer as a new, empty document, holding `null`.

 @tparam Traits The traits.
 @param buffer The buffer, 16-aligned, which the document uses in place and its user keeps alive.
 @return The document; @ref errc::capacity when the buffer is too small for an empty document or
 is not 16-aligned.
 @see "Images", in the guide.
*/
template <class Traits = default_traits>
result<basic_document<Traits, external>> format(std::span<std::byte> buffer) noexcept;

/**
 Opens the image already in a buffer, in place.

 A sealed image is checked against its seal first, a pass over the image, unless `check` is
 @ref verify::none.

 @tparam Traits The traits the image was written with.
 @param buffer The buffer, which the document uses in place and its user keeps alive.
 @param check Whether a sealed image's bytes are checked against its seal.
 @return The document; @ref errc::not_a_document for bytes that are not an image or a buffer that
 is not 16-aligned, @ref errc::incompatible_document for an image of another format version,
 reserved bytes, layout, byte order or traits, @ref errc::damaged_document for a sealed image
 whose bytes do not match its seal.
 @see "Images", in the guide.
*/
template <class Traits = default_traits>
result<basic_document<Traits, external>> open(std::span<std::byte> buffer,
                                              verify check = verify::seal) noexcept;

/**
 Copies an image into a new owned document.

 A sealed image is checked against its seal first, as @ref open checks it; the copy keeps the
 seal, its bytes being the image's.

 @tparam Traits The traits the image was written with.
 @param bytes The image, at any address.
 @param max_bytes The size the copy may grow to.
 @param check Whether a sealed image's bytes are checked against its seal.
 @return The document; the errors of @ref open, and @ref errc::capacity when the image is larger
 than `max_bytes`.
 @see "Images", in the guide.
*/
template <class Traits = default_traits>
result<basic_document<Traits, owned>> from_image(std::span<const std::byte> bytes,
                                                 std::uint64_t max_bytes = default_max_bytes,
                                                 verify check = verify::seal) noexcept;

/**
 A JSON document: one value, in one relocatable image.

 The image is a 64-byte header and a heap that holds the tree, every pointer in it an offset, so
 that it works at any 16-aligned address and has the same bytes on 64-bit native and wasm32. A
 document is moved cheaply; moving it, or destroying it, invalidates its handles, as a container's
 iterators would be. It is copied only with exceptions, or by @ref clone.

 @tparam Traits The traits: the stored types, the encoding, the bytes policy and the key order.
 @tparam Storage @ref owned or @ref external.
 @see "Handles", in the guide.
 @see "Images", in the guide.
*/
template <class Traits, class Storage>
class basic_document {
    static_assert(document_traits<Traits>, "the traits do not satisfy document_traits");
    static_assert(std::same_as<Storage, owned> || std::same_as<Storage, external>,
                  "a document's storage is owned or external");

public:
    /**
     Makes a new owned document, holding `null`.

     @param max_bytes The size its image may grow to; past it an operation returns
     @ref errc::capacity.
     @return The document; @ref errc::capacity when even an empty one does not fit `max_bytes`.
    */
    static result<basic_document> create(std::uint64_t max_bytes = default_max_bytes) noexcept
        requires std::same_as<Storage, owned>
    {
        auto made = detail::region::owned(std::min(first_bytes, max_bytes / 16 * 16), max_bytes);
        if (!made.has_value()) {
            return made.error();
        }
        if (const auto formatted = detail::tree<Traits>::format(made.unsafe_value());
            !formatted.has_value()) {
            return formatted.error();
        }
        return basic_document(std::move(made.unsafe_value()));
    }

    /**
     Makes a new owned document, holding `null`, as @ref create does, throwing on failure; only
     with exceptions.

     @throws capacity_error When even an empty document does not fit.
     @see "The sugar that throws", in the guide.
    */
    basic_document()
        requires detail::exceptions_enabled<Traits> && std::same_as<Storage, owned>;

    /**
     Copies a document's image into a new owned document, as @ref clone does, throwing on failure;
     only with exceptions.

     @param other The document.
     @throws capacity_error When the copy does not fit.
    */
    basic_document(const basic_document& other)
        requires detail::exceptions_enabled<Traits> && std::same_as<Storage, owned>;

    /**
     Takes the image of `other`, whose handles become invalid; `other` holds nothing after it.

     @param other The document.
    */
    basic_document(basic_document&& other) noexcept : region_(std::move(other.region_)) {
        // The other tree stays bound to the other region, now empty: it reads as nothing.
        tree_.rebind(region_);
    }

    /**
     Takes the image of `other`, freeing this document's own; the handles of both become invalid.

     @param other The document.
     @return This document.
    */
    basic_document& operator=(basic_document&& other) noexcept {
        if (this != &other) {
            region_ = std::move(other.region_);
            tree_.rebind(region_);
        }
        return *this;
    }

    /** A document is copied by @ref clone, or with exceptions by its copy constructor. */
    basic_document& operator=(const basic_document&) = delete;

    /** Frees an owned image. */
    ~basic_document() = default;

    /**
     Copies the image into a new owned document, as `structuredClone` would.

     The copy keeps a seal, its bytes being the image's; the seal is not checked, the image being
     this document's own.

     @return The copy, equal and independent; @ref errc::capacity when it does not fit, and
     @ref errc::type for a moved-from document, which has no image.
    */
    [[nodiscard]] result<basic_document<Traits, owned>> clone() const noexcept {
        if (region_.data() == nullptr) {
            return errc::type;
        }
        // A copy keeps an owned image's limit; an external image's grows as a new document would.
        const std::uint64_t limit = std::same_as<Storage, owned>
                                        ? region_.max_bytes()
                                        : std::max(default_max_bytes, region_.size());
        return from_image<Traits>(image(), limit, verify::none);
    }

    /**
     The handle of the document's value.

     @return The handle, which reads and writes.
    */
    basic_value<Traits, Storage> root() noexcept;

    /**
     The handle of the document's value, read only.

     @return The handle.
    */
    [[nodiscard]] basic_const_value<Traits, Storage> root() const noexcept;

    /**
     The member `key` of the document's value, as `root()[key]`.

     @param key The key, in UTF-8.
     @return The handle, which never fails.
    */
    basic_value<Traits, Storage> operator[](std::string_view key) noexcept;

    /**
     The element `index` of the document's value, as `root()[index]`.

     @param index The index.
     @return The handle, which never fails.
    */
    basic_value<Traits, Storage> operator[](std::uint64_t index) noexcept;

    /**
     The member `key` of the document's value, read only.

     @param key The key, in UTF-8.
     @return The handle, which never fails.
    */
    basic_const_value<Traits, Storage> operator[](std::string_view key) const noexcept;

    /**
     The element `index` of the document's value, read only.

     @param index The index.
     @return The handle, which never fails.
    */
    basic_const_value<Traits, Storage> operator[](std::uint64_t index) const noexcept;

    /**
     The image: the header and the heap, which @ref open and @ref from_image read back.

     @return Its bytes, valid until the document's next change.
    */
    [[nodiscard]] std::span<const std::byte> image() const noexcept {
        if (region_.data() == nullptr) {
            return {};
        }
        std::uint64_t size = 0;
        std::memcpy(&size, region_.data() + offsetof(image_header, size), sizeof size);
        return {region_.data(), static_cast<std::size_t>(size)};
    }

    /**
     Seals the image: writes into its header the CRC-32C of its bytes, which @ref open,
     @ref from_image and the image reader then check, refusing an image whose bytes changed.

     The seal holds until the document's next change, which clears it: the image is then simply
     not sealed, never wrongly sealed. It catches bytes changed inside an image (a flipped bit on
     a disk or a wire, a torn write, a stray write), not an old but valid image in reused memory,
     which carries a valid seal of its own. It costs a pass over the image, Boost.CRC's table
     a byte at a time: measured natively at -O2 on one core of an Apple M5 Max, 1.7 ms per MiB
     (about 600 MB/s), and the check when an image is opened costs the same.

     @return Success; @ref errc::type for a moved-from document, which has no image.
     @see "Images", in the guide.
    */
    result<void> seal() noexcept {
        if (region_.data() == nullptr) {
            return errc::type;
        }
        const std::span<const std::byte> bytes = image();
        detail::seal_image({region_.data(), bytes.size()});
        return {};
    }

    /**
     Whether the image carries a seal, which @ref seal wrote and no change has cleared since.

     @return Whether it is sealed; false for a moved-from document. The seal is not checked here.
    */
    [[nodiscard]] bool sealed() const noexcept {
        return region_.data() != nullptr && detail::is_sealed(image());
    }

    /**
     Shrinks an owned image to the end of its last block in use, its content unchanged.

     Blocks never move, so the free blocks before the last one in use stay in the image: a
     document that grew and then let most of its values go may stay nearly as large. A deep copy
     into a new document, `packed.root().set(doc)`, packs the content, and @ref compact packs it
     in one order whatever the document's history.

     @return Success; @ref errc::capacity when the smaller buffer cannot be allocated.
    */
    result<void> shrink_to_fit() noexcept
        requires std::same_as<Storage, owned>
    {
        return tree_.shrink_to_fit();
    }

    /**
     Rebuilds an owned image as the compact image of its content: the nodes in document order,
     ids numbered from the first, each block right after the last with no hole between, the image
     exactly as large as they are.

     Two documents of equal content, whatever edits made them, have byte-identical images after it,
     and a compact image compacts to itself. The copy is built before it replaces the image, so a
     failure changes nothing. As a vector's reallocation does, it invalidates every handle, every
     `std::string_view` read from a value and every iterator, and, the bytes having changed, it
     clears a seal: sealing after it is the caller's. The image's size is @ref compact_size
     afterwards, which is also its @ref capacity, so the next change that needs room grows it.

     @return Success; @ref errc::capacity when the new buffer cannot be allocated, and
     @ref errc::type for a moved-from document.
     @see "Images", in the guide.
    */
    result<void> compact() noexcept
        requires std::same_as<Storage, owned>
    {
        if (region_.data() == nullptr) {
            return errc::type;
        }
        const std::uint64_t bytes = tree_.compact_bytes();
        auto made = detail::region::owned(bytes, region_.max_bytes());
        if (!made.has_value()) {
            return made.error();
        }
        if (const auto built = detail::tree<Traits>::compact_to(made.unsafe_value(), tree_, bytes);
            !built.has_value()) {
            return built.error();
        }
        region_ = std::move(made.unsafe_value());
        tree_.rebind(region_);
        return {};
    }

    /**
     Writes the compact image of the document's content into a buffer, as @ref compact builds it,
     and opens it there: for a document that cannot reallocate, such as one leased from a pool or
     held in a shared block, or to send an image without waste.

     The buffer is zeroed past the image. This document is not changed.

     @param buffer The buffer, 16-aligned, of at least @ref compact_size bytes, which the returned
     document uses in place and its user keeps alive; it must not overlap this document's image.
     @return The document, over the buffer's first @ref compact_size bytes; @ref errc::capacity
     when the buffer is smaller or is not 16-aligned, writing nothing; @ref errc::type for a
     moved-from document and for a buffer that overlaps the image.
     @see "Images", in the guide.
    */
    [[nodiscard]] result<basic_document<Traits, external>> compact_into(
        std::span<std::byte> buffer) const noexcept;

    /**
     The size in bytes of the compact image of the document's content, which @ref compact and
     @ref compact_into produce exactly: a program sizes a block with it.

     It reads every node and allocates nothing, a fraction of the cost of the copy.

     @return The size; 0 for a moved-from document.
     @see "Images", in the guide.
    */
    [[nodiscard]] std::uint64_t compact_size() const noexcept {
        return region_.data() == nullptr ? 0 : tree_.compact_bytes();
    }

    /**
     The image's size in bytes, which the document's changes fill before an owned image grows and
     moves, and before an external one fails.

     @return The size, as @ref image gives it; 0 for a moved-from document.
     @see "Images", in the guide.
    */
    [[nodiscard]] std::uint64_t capacity() const noexcept {
        return region_.data() == nullptr ? 0 : region_.size();
    }

    /**
     Makes the image hold at least `bytes`, as `std::vector::reserve`: an owned image grows in one
     step, as a change that needs room grows it, to `bytes` or twice its size, the larger, at most
     its `max_bytes`.

     Growing moves the image, which invalidates every `std::string_view` read from a value but no
     handle or iterator, and clears a seal; a reservation within the capacity changes nothing.
     Building a large document after one reservation of its size moves the image once, not at
     each doubling.

     @param bytes The size the image must reach.
     @return Success; @ref errc::capacity when `bytes` is past `max_bytes`, or past an external
     image's size, which never grows, or the larger buffer cannot be allocated, each changing
     nothing; @ref errc::type for a moved-from document.
     @see "Images", in the guide.
    */
    [[nodiscard]] result<void> reserve(std::uint64_t bytes) noexcept {
        if (region_.data() == nullptr) {
            return errc::type;
        }
        return tree_.reserve(bytes);
    }

    /**
     The document's value as `JSON.stringify(value, null, indent)` writes it.

     @param indent The spaces of each level, clamped to 10; 0 writes compactly.
     @return The text; empty when the document holds `undefined`.
    */
    [[nodiscard]] std::string stringify(std::uint32_t indent = 0) const;

    /**
     Whether two documents hold equal values, structurally, key order aside.

     @param a A document.
     @param b A document.
     @return Whether `a.root() == b.root()`.
    */
    friend bool operator==(const basic_document& a, const basic_document& b) noexcept {
        return a.root() == b.root();
    }

private:
    template <class, class>
    friend class basic_document;
    template <class, class>
    friend class basic_const_value;
    template <class, class>
    friend class basic_value;
    template <class, class>
    friend class basic_parser;
    template <class, class>
    friend class detail::handler;
    friend struct detail::access;

    /** The size of a new owned document's first buffer, which grows as its content needs. */
    static constexpr std::uint64_t first_bytes = 4096;

    /** The document of a region whose tree was formatted or attached, the tree rebound to it. */
    explicit basic_document(detail::region r) noexcept : region_(std::move(r)) {
        tree_.rebind(region_);
    }

    detail::region region_;
    detail::tree<Traits> tree_;
};

/** A document of the default traits, owned: exact numbers, UTF-8, JavaScript's key order. */
using document = basic_document<>;

namespace detail {

/**
 The document of a region whose tree was formatted or attached, made by `access`, which
 `value.hpp` defines after this header: `Access` defers its lookup to where a call is instantiated.
*/
template <class Traits, class Storage, class Access = access>
basic_document<Traits, Storage> adopt(region r) noexcept {
    return Access::template make_document<Traits, Storage>(std::move(r));
}

}  // namespace detail

template <class Traits>
result<basic_document<Traits, external>> format(std::span<std::byte> buffer) noexcept {
    auto made = detail::region::external_empty(buffer);
    if (!made.has_value()) {
        return made.error();
    }
    if (const auto formatted = detail::tree<Traits>::format(made.unsafe_value());
        !formatted.has_value()) {
        return formatted.error();
    }
    return detail::adopt<Traits, external>(std::move(made.unsafe_value()));
}

template <class Traits>
result<basic_document<Traits, external>> open(std::span<std::byte> buffer, verify check) noexcept {
    if (reinterpret_cast<std::uintptr_t>(buffer.data()) % 16 != 0) {
        return errc::not_a_document;
    }
    if (const auto checked = check_image(buffer, detail::layout_tag<Traits>(), check);
        !checked.has_value()) {
        return checked.error();
    }
    image_header header{};
    std::memcpy(&header, buffer.data(), sizeof header);
    // The image is what its header states, which a larger buffer may hold.
    detail::region r =
        detail::region::external(buffer.first(static_cast<std::size_t>(header.size)));
    if (const auto attached = detail::tree<Traits>::attach(r); !attached.has_value()) {
        return attached.error();
    }
    return detail::adopt<Traits, external>(std::move(r));
}

template <class Traits>
result<basic_document<Traits, owned>> from_image(std::span<const std::byte> bytes,
                                                 std::uint64_t max_bytes, verify check) noexcept {
    if (const auto checked = check_image(bytes, detail::layout_tag<Traits>(), check);
        !checked.has_value()) {
        return checked.error();
    }
    image_header header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    auto made =
        detail::region::owned_copy(bytes.first(static_cast<std::size_t>(header.size)), max_bytes);
    if (!made.has_value()) {
        return made.error();
    }
    if (const auto attached = detail::tree<Traits>::attach(made.unsafe_value());
        !attached.has_value()) {
        return attached.error();
    }
    return detail::adopt<Traits, owned>(std::move(made.unsafe_value()));
}

template <class Traits, class Storage>
result<basic_document<Traits, external>> basic_document<Traits, Storage>::compact_into(
    std::span<std::byte> buffer) const noexcept {
    if (region_.data() == nullptr) {
        return errc::type;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(buffer.data());
    const auto image_first = reinterpret_cast<std::uintptr_t>(region_.data());
    if (first < image_first + region_.size() && image_first < first + buffer.size()) {
        return errc::type;
    }
    const std::uint64_t bytes = tree_.compact_bytes();
    if (bytes > buffer.size()) {
        return errc::capacity;
    }
    auto made = detail::region::external_empty(buffer.first(static_cast<std::size_t>(bytes)));
    if (!made.has_value()) {
        return made.error();
    }
    if (const auto built = detail::tree<Traits>::compact_to(made.unsafe_value(), tree_, bytes);
        !built.has_value()) {
        return built.error();
    }
    std::memset(buffer.data() + bytes, 0, buffer.size() - static_cast<std::size_t>(bytes));
    return detail::adopt<Traits, external>(std::move(made.unsafe_value()));
}

}  // namespace webcpp::document

#include <webcpp/document/value.hpp>

#endif  // WEBCPP_DOCUMENT_DOCUMENT_HPP
