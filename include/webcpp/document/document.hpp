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
#include <webcpp/document/storage.hpp>
#include <webcpp/document/traits.hpp>

#include <boost/throw_exception.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::document {

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
 Formats a buffer as a new, empty document, holding `null`, whose operations keep their working
 state in `alloc`.

 @tparam Traits The traits.
 @tparam Allocator An allocator of `std::byte`.
 @param buffer The buffer, 16-aligned, which the document uses in place and its user keeps alive.
 @param alloc The allocator of the working state of the document's operations.
 @return The document; @ref errc::capacity when the buffer is too small for an empty document or
 is not 16-aligned.
 @see "Allocators", in the guide.
*/
template <class Traits = default_traits, detail::byte_allocator Allocator>
result<basic_document<Traits, basic_external<Allocator>>> format(std::span<std::byte> buffer,
                                                                 const Allocator& alloc) noexcept;

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
 Opens the image already in a buffer, in place, as @ref open does, the working state of the
 document's operations in `alloc`.

 @tparam Traits The traits the image was written with.
 @tparam Allocator An allocator of `std::byte`.
 @param buffer The buffer, which the document uses in place and its user keeps alive.
 @param alloc The allocator of the working state of the document's operations.
 @param check Whether a sealed image's bytes are checked against its seal.
 @return The document; the errors of @ref open.
 @see "Allocators", in the guide.
*/
template <class Traits = default_traits, detail::byte_allocator Allocator>
result<basic_document<Traits, basic_external<Allocator>>> open(
    std::span<std::byte> buffer, const Allocator& alloc, verify check = verify::seal) noexcept;

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
 Copies an image into a new owned document allocated through `alloc`, as @ref from_image does.

 @tparam Traits The traits the image was written with.
 @tparam Allocator An allocator of `std::byte`.
 @param bytes The image, at any address.
 @param alloc The allocator of the copy and of the working state of its operations.
 @param max_bytes The size the copy may grow to.
 @param check Whether a sealed image's bytes are checked against its seal.
 @return The document; the errors of @ref from_image, and @ref errc::capacity when `alloc` has no
 room for the copy.
 @see "Allocators", in the guide.
*/
template <class Traits = default_traits, detail::byte_allocator Allocator>
result<basic_document<Traits, basic_owned<Allocator>>> from_image(
    std::span<const std::byte> bytes, const Allocator& alloc,
    std::uint64_t max_bytes = default_max_bytes, verify check = verify::seal) noexcept;

/**
 A JSON document: one value, in one relocatable image.

 The image is a 64-byte header and a heap that holds the tree, every pointer in it an offset, so
 that it works at any 16-aligned address and has the same bytes on 64-bit native and wasm32. A
 document is moved cheaply; moving it, or destroying it, invalidates its handles, as a container's
 iterators would be. It is copied only with exceptions, or by @ref clone.

 It is allocator-aware as the standard containers are: its storage's allocator allocates, grows and
 frees an owned image and, rebound, holds the working state of its operations; a move takes the
 allocator, a move assignment between allocators that are not equal and do not propagate copies
 the image, and the copy constructor asks `select_on_container_copy_construction`.

 @tparam Traits The traits: the stored types, the encoding, the bytes policy and the key order.
 @tparam Storage @ref basic_owned or @ref basic_external of an allocator of `std::byte`: @ref owned
 and @ref external in the standard allocator, `pmr::owned` and `pmr::external` in a
 `std::pmr::memory_resource`.
 @see "Handles", in the guide.
 @see "Images", in the guide.
 @see "Allocators", in the guide.
*/
template <class Traits, class Storage>
class basic_document {
    static_assert(document_traits<Traits>, "the traits do not satisfy document_traits");
    static_assert(detail::storage<Storage>,
                  "a document's storage is basic_owned or basic_external of an allocator of "
                  "std::byte");

public:
    /** The allocator of the image, when owned, and of the working state of the operations. */
    using allocator_type = Storage::allocator_type;

    /**
     Makes a new owned document, holding `null`, in a default-constructed allocator.

     @param max_bytes The size its image may grow to; past it an operation returns
     @ref errc::capacity.
     @return The document; @ref errc::capacity when even an empty one does not fit `max_bytes`.
    */
    static result<basic_document> create(std::uint64_t max_bytes = default_max_bytes) noexcept
        requires detail::owned_storage<Storage>
    {
        return create(allocator_type(), max_bytes);
    }

    /**
     Makes a new owned document, holding `null`, allocated through `alloc`.

     @param alloc The allocator of the image and of the working state of the operations.
     @param max_bytes The size its image may grow to; past it an operation returns
     @ref errc::capacity.
     @return The document; @ref errc::capacity when even an empty one does not fit `max_bytes`, or
     `alloc` has no room for it.
     @see "Allocators", in the guide.
    */
    static result<basic_document> create(const allocator_type& alloc,
                                         std::uint64_t max_bytes = default_max_bytes) noexcept
        requires detail::owned_storage<Storage>
    {
        auto made =
            region_type::owned(std::min(first_bytes, max_bytes / 16 * 16), max_bytes, alloc);
        if (!made.has_value()) {
            return made.error();
        }
        if (const auto formatted = tree_type::format(made.unsafe_value()); !formatted.has_value()) {
            return formatted.error();
        }
        return basic_document(std::move(made.unsafe_value()));
    }

    /**
     A document that holds nothing, as a moved-from document does: it has no image, allocates
     nothing and cannot fail, so a library can hand one out where it has no document to give.

     Its root reads `undefined`, it writes no text, @ref image is empty, a change and @ref clone
     return @ref errc::type, and a move assignment gives it an image.

     @param alloc The allocator it keeps, as a moved-from document keeps its own.
     @return The document.
     @see "Allocators", in the guide.
    */
    static basic_document nothing(const allocator_type& alloc = allocator_type()) noexcept {
        return basic_document(region_type(alloc));
    }

    /**
     Makes a new owned document, holding `null`, as @ref create does, throwing on failure; only
     with exceptions.

     @throws capacity_error When even an empty document does not fit.
     @see "The sugar that throws", in the guide.
    */
    basic_document()
        requires detail::exceptions_enabled<Traits> && detail::owned_storage<Storage>;

    /**
     Makes a new owned document, holding `null`, allocated through `alloc`, as @ref create does,
     throwing on failure; only with exceptions.

     @param alloc The allocator of the image and of the working state of the operations.
     @throws capacity_error When even an empty document does not fit.
     @see "The sugar that throws", in the guide.
    */
    explicit basic_document(const allocator_type& alloc)
        requires detail::exceptions_enabled<Traits> && detail::owned_storage<Storage>;

    /**
     Copies a document's image into a new owned document, in the allocator that
     `select_on_container_copy_construction` gives for `other`'s, throwing on failure; only with
     exceptions.

     @param other The document.
     @throws capacity_error When the copy does not fit.
    */
    basic_document(const basic_document& other)
        requires detail::exceptions_enabled<Traits> && detail::owned_storage<Storage>;

    /**
     Copies a document's image into a new owned document allocated through `alloc`, as
     @ref clone does, throwing on failure; only with exceptions.

     @param other The document.
     @param alloc The allocator of the copy.
     @throws capacity_error When the copy does not fit.
     @see "Allocators", in the guide.
    */
    basic_document(const basic_document& other, const allocator_type& alloc)
        requires detail::exceptions_enabled<Traits> && detail::owned_storage<Storage>;

    /**
     Takes the image and the allocator of `other`, whose handles become invalid; `other` holds
     nothing after it.

     @param other The document.
    */
    basic_document(basic_document&& other) noexcept : region_(std::move(other.region_)) {
        // The other tree stays bound to the other region, now empty: it reads as nothing.
        tree_.rebind(region_);
    }

    /**
     Takes the image of `other`, freeing this document's own; the handles of both become invalid,
     and `other` holds nothing after it.

     The allocator is taken when it propagates on a move assignment. When it does not, and the two
     are not equal, an owned image is copied into this document's allocator, one copy of its bytes,
     and `other`'s freed; an external image, which no allocator holds, is taken as it is.

     @param other The document.
     @return This document.
     @throws std::bad_alloc When an owned image is copied and this document's allocator has no room
     for it, through `boost::throw_exception`, as a failure that cannot be returned: never for
     allocators that propagate or are always equal, the standard one among them, nor for an
     external document, whose image no allocator holds, for which it is `noexcept`.
     @see "Allocators", in the guide.
    */
    // It is noexcept for allocators that propagate or are always equal and for an external
    // document, none of which takes the copy, the one path that throws.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    basic_document& operator=(basic_document&& other) noexcept(moves_cheaply ||
                                                               detail::external_storage<Storage>) {
        if (this == &other) {
            return *this;
        }
        if constexpr (!moves_cheaply && detail::owned_storage<Storage>) {
            if (region_.get_allocator() != other.region_.get_allocator()) {
                take_by_copy(other);
                return *this;
            }
        }
        region_ = std::move(other.region_);
        tree_.rebind(region_);
        return *this;
    }

    /** A document is copied by @ref clone, or with exceptions by its copy constructor. */
    basic_document& operator=(const basic_document&) = delete;

    /** Frees an owned image. */
    ~basic_document() = default;

    /**
     The allocator of the image, when owned, and of the working state of the operations.

     @return A copy of it.
    */
    [[nodiscard]] allocator_type get_allocator() const noexcept { return region_.get_allocator(); }

    /**
     Copies the image into a new owned document, as `structuredClone` would, in this document's
     allocator.

     The copy keeps a seal, its bytes being the image's; the seal is not checked, the image being
     this document's own.

     @return The copy, equal and independent; @ref errc::capacity when it does not fit, and
     @ref errc::type for a moved-from document, which has no image.
    */
    [[nodiscard]] result<basic_document<Traits, detail::owned_of<Storage>>> clone() const noexcept {
        return clone(get_allocator());
    }

    /**
     Copies the image into a new owned document allocated through `alloc`, as `structuredClone`
     would.

     @param alloc The allocator of the copy and of the working state of its operations.
     @return The copy, equal and independent; @ref errc::capacity when it does not fit, or `alloc`
     has no room for it, and @ref errc::type for a moved-from document, which has no image.
     @see "Allocators", in the guide.
    */
    [[nodiscard]] result<basic_document<Traits, detail::owned_of<Storage>>> clone(
        const allocator_type& alloc) const noexcept {
        if (region_.data() == nullptr) {
            return errc::type;
        }
        return from_image<Traits>(image(), alloc, copy_limit(), verify::none);
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
        requires detail::owned_storage<Storage>
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
        requires detail::owned_storage<Storage>
    {
        if (region_.data() == nullptr) {
            return errc::type;
        }
        const std::uint64_t bytes = tree_.compact_bytes();
        auto made = region_type::owned(bytes, region_.max_bytes(), region_.get_allocator());
        if (!made.has_value()) {
            return made.error();
        }
        if (const auto built = tree_type::compact_to(made.unsafe_value(), tree_, bytes);
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
     @return The document, over the buffer's first @ref compact_size bytes, its working state in
     this document's allocator; @ref errc::capacity when the buffer is smaller or is not
     16-aligned, writing nothing; @ref errc::type for a moved-from document and for a buffer that
     overlaps the image.
     @see "Images", in the guide.
    */
    [[nodiscard]] result<basic_document<Traits, basic_external<allocator_type>>> compact_into(
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

    /** The region of the storage's allocator. */
    using region_type = detail::basic_region<allocator_type>;

    /** The tree of the storage's allocator. */
    using tree_type = detail::tree<Traits, allocator_type>;

    /** The allocator's traits. */
    using alloc_traits = std::allocator_traits<allocator_type>;

    /**
     Whether a move assignment only takes the other image: the allocator propagates on a move, or
     two of them are always equal.
    */
    static constexpr bool moves_cheaply =
        alloc_traits::propagate_on_container_move_assignment::value ||
        alloc_traits::is_always_equal::value;

    /** The size of a new owned document's first buffer, which grows as its content needs. */
    static constexpr std::uint64_t first_bytes = 4096;

    /** The document of a region whose tree was formatted or attached, the tree rebound to it. */
    explicit basic_document(region_type r) noexcept : region_(std::move(r)) {
        tree_.rebind(region_);
    }

    /** The limit of a copy: an owned image's own; an external image's grows as a new one would. */
    [[nodiscard]] std::uint64_t copy_limit() const noexcept {
        return detail::owned_storage<Storage> ? region_.max_bytes()
                                              : std::max(default_max_bytes, region_.size());
    }

    /**
     Copies the owned image of `other`, of an allocator that is not equal and does not propagate,
     into this document's allocator, and frees `other`'s; a moved-from `other` leaves this
     document holding nothing.
    */
    void take_by_copy(basic_document& other) {
        if (other.region_.data() == nullptr) {
            region_ = region_type(region_.get_allocator());
            tree_.rebind(region_);
            return;
        }
        auto copied = region_type::owned_copy(other.image(), other.region_.max_bytes(),
                                              region_.get_allocator());
        if (!copied.has_value()) {
            boost::throw_exception(std::bad_alloc());
        }
        region_ = std::move(copied.unsafe_value());
        tree_.rebind(region_);
        other.region_ = region_type(other.region_.get_allocator());
    }

    region_type region_;
    tree_type tree_;
};

/** A document of the default traits, owned: exact numbers, UTF-8, JavaScript's key order. */
using document = basic_document<>;

namespace pmr {

/**
 A document that owns its image, which a `std::pmr::memory_resource` holds with the working state
 of its operations.

 @tparam Traits The traits.
 @see "Allocators", in the guide.
*/
template <class Traits = default_traits>
using basic_document = webcpp::document::basic_document<Traits, owned>;

/**
 A document of the default traits whose image and working state a `std::pmr::memory_resource`
 holds.

 @see "Allocators", in the guide.
*/
using document = basic_document<>;

}  // namespace pmr

namespace detail {

/**
 The document of a region whose tree was formatted or attached, made by `access`, which
 `value.hpp` defines after this header: `Access` defers its lookup to where a call is instantiated.
*/
template <class Traits, class Storage, class Access = access>
basic_document<Traits, Storage> adopt(basic_region<typename Storage::allocator_type> r) noexcept {
    return Access::template make_document<Traits, Storage>(std::move(r));
}

}  // namespace detail

template <class Traits>
result<basic_document<Traits, external>> format(std::span<std::byte> buffer) noexcept {
    return format<Traits>(buffer, std::allocator<std::byte>());
}

template <class Traits, detail::byte_allocator Allocator>
result<basic_document<Traits, basic_external<Allocator>>> format(std::span<std::byte> buffer,
                                                                 const Allocator& alloc) noexcept {
    auto made = detail::basic_region<Allocator>::external_empty(buffer, alloc);
    if (!made.has_value()) {
        return made.error();
    }
    if (const auto formatted = detail::tree<Traits, Allocator>::format(made.unsafe_value());
        !formatted.has_value()) {
        return formatted.error();
    }
    return detail::adopt<Traits, basic_external<Allocator>>(std::move(made.unsafe_value()));
}

template <class Traits>
result<basic_document<Traits, external>> open(std::span<std::byte> buffer, verify check) noexcept {
    return open<Traits>(buffer, std::allocator<std::byte>(), check);
}

template <class Traits, detail::byte_allocator Allocator>
result<basic_document<Traits, basic_external<Allocator>>> open(std::span<std::byte> buffer,
                                                               const Allocator& alloc,
                                                               verify check) noexcept {
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
    auto r = detail::basic_region<Allocator>::external(
        buffer.first(static_cast<std::size_t>(header.size)), alloc);
    if (const auto attached = detail::tree<Traits, Allocator>::attach(r); !attached.has_value()) {
        return attached.error();
    }
    return detail::adopt<Traits, basic_external<Allocator>>(std::move(r));
}

template <class Traits>
result<basic_document<Traits, owned>> from_image(std::span<const std::byte> bytes,
                                                 std::uint64_t max_bytes, verify check) noexcept {
    return from_image<Traits>(bytes, std::allocator<std::byte>(), max_bytes, check);
}

template <class Traits, detail::byte_allocator Allocator>
result<basic_document<Traits, basic_owned<Allocator>>> from_image(std::span<const std::byte> bytes,
                                                                  const Allocator& alloc,
                                                                  std::uint64_t max_bytes,
                                                                  verify check) noexcept {
    if (const auto checked = check_image(bytes, detail::layout_tag<Traits>(), check);
        !checked.has_value()) {
        return checked.error();
    }
    image_header header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    auto made = detail::basic_region<Allocator>::owned_copy(
        bytes.first(static_cast<std::size_t>(header.size)), max_bytes, alloc);
    if (!made.has_value()) {
        return made.error();
    }
    if (const auto attached = detail::tree<Traits, Allocator>::attach(made.unsafe_value());
        !attached.has_value()) {
        return attached.error();
    }
    return detail::adopt<Traits, basic_owned<Allocator>>(std::move(made.unsafe_value()));
}

template <class Traits, class Storage>
result<basic_document<Traits, basic_external<typename Storage::allocator_type>>>
basic_document<Traits, Storage>::compact_into(std::span<std::byte> buffer) const noexcept {
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
    auto made = region_type::external_empty(buffer.first(static_cast<std::size_t>(bytes)),
                                            region_.get_allocator());
    if (!made.has_value()) {
        return made.error();
    }
    if (const auto built = tree_type::compact_to(made.unsafe_value(), tree_, bytes);
        !built.has_value()) {
        return built.error();
    }
    std::memset(buffer.data() + bytes, 0, buffer.size() - static_cast<std::size_t>(bytes));
    return detail::adopt<Traits, basic_external<allocator_type>>(std::move(made.unsafe_value()));
}

}  // namespace webcpp::document

#include <webcpp/document/value.hpp>

#endif  // WEBCPP_DOCUMENT_DOCUMENT_HPP
