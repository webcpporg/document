// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The heap of a document's image: Boost.Interprocess's `rbtree_best_fit`, used directly, its
 allocator, which zeroes every block it hands out and frees, the strings it holds, and the region,
 the bytes of the image, owned and growing or external and fixed.

 Only `offset_ptr.hpp`, `mem_algo/rbtree_best_fit.hpp` and `sync/null_mutex.hpp` are included from
 Boost.Interprocess: its `managed_external_buffer`, `segment_manager`, `allocator` and
 `mutex_family` throw bare and do not compile without exceptions.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_HEAP_HPP
#define WEBCPP_DOCUMENT_DETAIL_HEAP_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/errors.hpp>

#include <boost/interprocess/mem_algo/rbtree_best_fit.hpp>  // lint-world: a heap in a buffer
#include <boost/interprocess/offset_ptr.hpp>                // lint-world: an offset, no process
#include <boost/interprocess/sync/null_mutex.hpp>  // lint-world: a mutex that locks nothing

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace webcpp::document::detail {

/** A pointer inside an image: an offset from its own address, the same on 64-bit and wasm32. */
template <class T>
using pointer = boost::interprocess::offset_ptr<T, std::int64_t, std::uint64_t, 8>;

/** The mutex family `rbtree_best_fit` asks for, without `mutex_family.hpp`: no locking at all. */
struct no_mutexes {
    /** The mutex of the heap: none. */
    using mutex_type = boost::interprocess::null_mutex;

    /** The recursive mutex of the heap: none. */
    using recursive_mutex_type = boost::interprocess::null_mutex;
};

/** The heap of an image, aligned to 16 bytes, addressed by offsets. */
using heap = boost::interprocess::rbtree_best_fit<no_mutexes, pointer<void>, 16>;

/** The size of an image's header, which the heap follows. */
inline constexpr std::uint64_t header_bytes = 64;

/**
 The allocator of the tree's container: it allocates in the heap, zeroes each block before it
 hands it out and before it frees it, and reports an allocation that fails through
 `boost::throw_exception`, since every operation finds its room first and such a failure is a bug.
*/
template <class T>
class allocator {
public:
    /** The type allocated. */
    using value_type = T;

    /** A pointer to it, inside the image. */
    using pointer = detail::pointer<T>;

    /** A pointer to it, read only, inside the image. */
    using const_pointer = detail::pointer<const T>;

    /** A pointer to anything, inside the image. */
    using void_pointer = detail::pointer<void>;

    /** A pointer to anything, read only, inside the image. */
    using const_void_pointer = detail::pointer<const void>;

    /** A size, the same on 64-bit and wasm32. */
    using size_type = std::uint64_t;

    /** A difference of pointers, the same on 64-bit and wasm32. */
    using difference_type = std::int64_t;

    /** The same allocator for another type. */
    template <class U>
    struct rebind {
        /** The allocator of `U`. */
        using other = allocator<U>;
    };

    /** Allocates in the heap `h`, which outlives the allocator. */
    explicit allocator(heap* h) noexcept;

    // The Allocator requirements convert an allocator to its rebound type implicitly, hence the
    // exemption from misc-explicit-constructor.
    /** The allocator of another type over the same heap. */
    template <class U>
    allocator(const allocator<U>& other) noexcept;  // NOLINT(misc-explicit-constructor)

    /**
     Allocates room for `n` objects, zeroed; a failure is
     `boost::throw_exception(std::bad_alloc())`.
    */
    pointer allocate(size_type n);

    /** Zeroes the room of `n` objects at `p`, then frees it. */
    void deallocate(pointer p, size_type n) noexcept;

    /** The heap it allocates in. */
    [[nodiscard]] heap* get_heap() const noexcept;

    /** Whether two allocators allocate in the same heap. */
    friend bool operator==(const allocator& a, const allocator& b) noexcept {
        return a.get_heap() == b.get_heap();
    }

private:
    template <class U>
    friend class allocator;

    detail::pointer<heap> heap_;
};

/**
 A string or a bytes value in the heap: its bytes, allocated at exactly their size, never a
 `boost::container::string`, whose allocation may grow past the size and throws on failure.
*/
struct byte_string {
    /** The first byte, or null when empty. */
    pointer<std::byte> data;

    /** How many bytes. */
    std::uint64_t size = 0;

    /** The bytes, valid until the image moves or the string is freed. */
    [[nodiscard]] std::span<const std::byte> view() const noexcept;

    /** The bytes as UTF-8 code units. */
    [[nodiscard]] std::string_view chars() const noexcept;

    /** The bytes as UTF-16 code units, in the machine's byte order. */
    [[nodiscard]] std::u16string_view units() const noexcept;
};

/**
 Allocates a string of exactly `bytes`' size in `h` and copies them; @ref errc::capacity when there
 is no room.
*/
inline result<byte_string> make_string(heap& h, std::span<const std::byte> bytes) noexcept;

/** Zeroes a string's bytes, frees them and leaves `s` empty. */
inline void free_string(heap& h, byte_string& s) noexcept;

/**
 Whether blocks of every size in `sizes` fit in `h` at once; allocates them and frees them all.
*/
inline bool probe(heap& h, std::span<const std::uint64_t> sizes) noexcept;

/**
 The bytes of an image: owned, which the region allocates, zeroes and grows, or external, a span
 its user owns, which never grows.
*/
class region {
public:
    /** A new owned region of `size` bytes, zeroed, with an empty heap after the header. */
    static result<region> owned(std::uint64_t size, std::uint64_t max_bytes) noexcept;

    /** An owned copy of an image's bytes, whose heap is the image's own. */
    static result<region> owned_copy(std::span<const std::byte> bytes,
                                     std::uint64_t max_bytes) noexcept;

    /** An external region over `buffer`, as it is: for an image already there. */
    static region external(std::span<std::byte> buffer) noexcept;

    /** An external region over `buffer`, zeroed, with an empty heap after the header. */
    static result<region> external_empty(std::span<std::byte> buffer) noexcept;

    /** An empty region, which holds no bytes. */
    region() noexcept = default;

    /** Takes the bytes of `other`, which is left empty. */
    region(region&& other) noexcept;

    /** Frees its own bytes, then takes the bytes of `other`, which is left empty. */
    region& operator=(region&& other) noexcept;

    /** A region is not copied: an image is copied by @ref owned_copy. */
    region(const region&) = delete;

    /** A region is not copied: an image is copied by @ref owned_copy. */
    region& operator=(const region&) = delete;

    /** Frees an owned region's bytes. */
    ~region();

    /** The first byte, 16-aligned, or null when empty. */
    [[nodiscard]] std::byte* data() const noexcept;

    /** How many bytes. */
    [[nodiscard]] std::uint64_t size() const noexcept;

    /** The size an owned region never grows past. */
    [[nodiscard]] std::uint64_t max_bytes() const noexcept;

    /** Whether the region owns its bytes, and so may grow. */
    [[nodiscard]] bool is_owned() const noexcept;

    /**
     Grows an owned region to twice its size or `at_least`, the larger, rounded up to 16, at most
     `max_bytes`: the bytes are copied, the rest zeroed and the heap grown; false when it cannot.
    */
    bool grow(std::uint64_t at_least) noexcept;

    /**
     Moves an owned region's first `size` bytes into a buffer of that size; false when it cannot.
    */
    bool shrink_to(std::uint64_t size) noexcept;

private:
    std::byte* data_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t max_bytes_ = 0;
    bool owned_ = false;
};

/** The heap of a region, at offset @ref header_bytes. */
inline heap& heap_of(const region& r) noexcept;

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_HEAP_HPP
