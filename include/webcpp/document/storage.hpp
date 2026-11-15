// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The storage of a document, its second template parameter: where its image lives, owned and
 growing or in a buffer its user owns, and the allocator of everything it allocates.
*/

#ifndef WEBCPP_DOCUMENT_STORAGE_HPP
#define WEBCPP_DOCUMENT_STORAGE_HPP

#include <webcpp/document/config.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <memory_resource>

namespace webcpp::document {

/**
 The storage of a document that owns its image: a buffer of its own, 16-aligned, which grows,
 twice the size each time, up to its `max_bytes`, allocated, grown and freed through `Allocator`.

 The same allocator, rebound, holds what the document's operations need while they run: the
 parser's, the tree's and the writer's working state. A move takes the allocator with the image,
 and a move assignment between allocators that are not equal and do not propagate copies the image
 into the target's allocator.

 The image is allocated as blocks of 16 bytes, 16-aligned, and the allocator rebound to them must
 give them that alignment: a buffer it gives at another alignment is given back, and the operation
 returns @ref errc::capacity.

 @tparam Allocator An allocator of `std::byte`; `std::allocator<std::byte>` by default, whose
 documents are those of @ref owned.
 @see "Images", in the guide.
 @see "Allocators", in the guide.
*/
template <class Allocator = std::allocator<std::byte>>
struct basic_owned {
    /** The allocator of the image and of the working state. */
    using allocator_type = Allocator;
};

/**
 The storage of a document whose image is a buffer its user owns, such as a browser's memory: when
 it is full, an operation returns @ref errc::capacity and changes nothing.

 The image never grows, and `Allocator` holds only the working state of the document's operations:
 the parser's, the tree's and the writer's.

 @tparam Allocator An allocator of `std::byte`; `std::allocator<std::byte>` by default, whose
 documents are those of @ref external.
 @see "Images", in the guide.
 @see "Allocators", in the guide.
*/
template <class Allocator = std::allocator<std::byte>>
struct basic_external {
    /** The allocator of the working state. */
    using allocator_type = Allocator;
};

/**
 The storage of a document that owns its image, in the standard allocator.

 @see "Images", in the guide.
*/
using owned = basic_owned<>;

/**
 The storage of a document whose image is a buffer its user owns, its working state in the
 standard allocator.

 @see "Images", in the guide.
*/
using external = basic_external<>;

/** The storages of documents whose memory is a `std::pmr::memory_resource`. */
namespace pmr {

/**
 The storage of a document that owns its image, which a `std::pmr::memory_resource` holds with
 the working state of its operations.

 @see "Allocators", in the guide.
*/
using owned = basic_owned<std::pmr::polymorphic_allocator<std::byte>>;

/**
 The storage of a document whose image is a buffer its user owns, the working state of its
 operations in a `std::pmr::memory_resource`.

 @see "Allocators", in the guide.
*/
using external = basic_external<std::pmr::polymorphic_allocator<std::byte>>;

}  // namespace pmr

namespace detail {

/**
 Whether `A` is an allocator of `std::byte`, which a document's storage takes: its `value_type` is
 checked before `std::allocator_traits` is named, which would not compile for a type that is no
 allocator, so that a call whose argument is no allocator chooses another overload.
*/
template <class A>
concept byte_allocator =
    requires { typename A::value_type; } && std::same_as<typename A::value_type, std::byte> &&
    requires(A& a, std::size_t n) { a.allocate(n); } && std::copy_constructible<A> &&
    std::equality_comparable<A>;

/** Whether `S` is @ref basic_owned of an allocator. */
template <class S>
inline constexpr bool is_owned = false;

/** Whether `S` is @ref basic_owned of an allocator: it is. */
template <class A>
inline constexpr bool is_owned<basic_owned<A>> = true;

/** Whether `S` is @ref basic_external of an allocator. */
template <class S>
inline constexpr bool is_external = false;

/** Whether `S` is @ref basic_external of an allocator: it is. */
template <class A>
inline constexpr bool is_external<basic_external<A>> = true;

/** A storage that owns its image: @ref basic_owned of an allocator of `std::byte`. */
template <class S>
concept owned_storage = is_owned<S> && byte_allocator<typename S::allocator_type>;

/** A storage over a buffer its user owns: @ref basic_external of an allocator of `std::byte`. */
template <class S>
concept external_storage = is_external<S> && byte_allocator<typename S::allocator_type>;

/** A document's storage, owned or external. */
template <class S>
concept storage = owned_storage<S> || external_storage<S>;

/**
 The owned storage of the same allocator as `S`: the storage of the documents an operation on a
 document of `S` makes, a copy, a `pop` or a `slice`.
*/
template <class S>
using owned_of = basic_owned<typename S::allocator_type>;

/** The allocator `A` gives for objects of `T`. */
template <class A, class T>
using rebound = std::allocator_traits<A>::template rebind_alloc<T>;

}  // namespace detail

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_STORAGE_HPP
