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

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::document {

/**
 The storage of a document that owns its image: a buffer of its own, 16-aligned, which grows,
 twice the size each time, up to its `max_bytes`.
*/
struct owned {};

/**
 The storage of a document whose image is a buffer its user owns, such as a browser's memory: when
 it is full, an operation returns @ref errc::capacity and changes nothing.
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
*/
template <class Traits = default_traits>
result<basic_document<Traits, external>> format(std::span<std::byte> buffer) noexcept;

/**
 Opens the image already in a buffer, in place.

 @tparam Traits The traits the image was written with.
 @param buffer The buffer, which the document uses in place and its user keeps alive.
 @return The document; @ref errc::not_a_document for bytes that are not an image or a buffer that
 is not 16-aligned, @ref errc::incompatible_document for an image of another format version,
 Boost version, layout, byte order or traits.
*/
template <class Traits = default_traits>
result<basic_document<Traits, external>> open(std::span<std::byte> buffer) noexcept;

/**
 Copies an image into a new owned document.

 @tparam Traits The traits the image was written with.
 @param bytes The image, at any address.
 @param max_bytes The size the copy may grow to.
 @return The document; the errors of @ref open, and @ref errc::capacity when the image is larger
 than `max_bytes`.
*/
template <class Traits = default_traits>
result<basic_document<Traits, owned>> from_image(
    std::span<const std::byte> bytes, std::uint64_t max_bytes = default_max_bytes) noexcept;

/**
 A JSON document: one value, in one relocatable image.

 The image is a 64-byte header and a heap that holds the tree, every pointer in it an offset, so
 that it works at any 16-aligned address and has the same bytes on 64-bit native and wasm32. A
 document is moved cheaply; moving it, or destroying it, invalidates its handles, as a container's
 iterators would be. It is copied only with exceptions, or by @ref clone.

 @tparam Traits The traits: the stored types, the encoding, the bytes policy and the key order.
 @tparam Storage @ref owned or @ref external.
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
        requires std::same_as<Storage, owned>;

    /**
     Makes a new owned document, holding `null`, as @ref create does, throwing on failure; only
     with exceptions.

     @throws capacity_error When even an empty document does not fit.
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
    basic_document(basic_document&& other) noexcept;

    /**
     Takes the image of `other`, freeing this document's own; the handles of both become invalid.

     @param other The document.
     @return This document.
    */
    basic_document& operator=(basic_document&& other) noexcept;

    /** A document is copied by @ref clone, or with exceptions by its copy constructor. */
    basic_document& operator=(const basic_document&) = delete;

    /** Frees an owned image. */
    ~basic_document() = default;

    /**
     Copies the image into a new owned document, as `structuredClone` would.

     @return The copy, equal and independent; @ref errc::capacity when it does not fit.
    */
    [[nodiscard]] result<basic_document<Traits, owned>> clone() const noexcept;

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
    [[nodiscard]] std::span<const std::byte> image() const noexcept;

    /**
     Makes an owned image as small as its content, which is unchanged.

     @return Success; @ref errc::capacity when the smaller buffer cannot be allocated.
    */
    result<void> shrink_to_fit() noexcept
        requires std::same_as<Storage, owned>;

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

    /** The document of a region whose tree was formatted or attached, the tree rebound to it. */
    explicit basic_document(detail::region r) noexcept;

    detail::region region_;
    detail::tree<Traits> tree_;
};

/** A document of the default traits, owned: exact numbers, UTF-8, JavaScript's key order. */
using document = basic_document<>;

}  // namespace webcpp::document

#include <webcpp/document/value.hpp>

#endif  // WEBCPP_DOCUMENT_DOCUMENT_HPP
