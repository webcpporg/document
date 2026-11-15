// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The heap of a document's image: Boost.Interprocess's `rbtree_best_fit`, used directly, its
 allocator, which zeroes every block it hands out and frees, the strings it holds, and the region,
 the bytes of the image, owned and growing through the document's allocator or external and fixed.

 Only `offset_ptr.hpp`, `mem_algo/rbtree_best_fit.hpp` and `sync/null_mutex.hpp` are included from
 Boost.Interprocess: its `managed_external_buffer`, `segment_manager`, `allocator` and
 `mutex_family` throw bare and do not compile without exceptions.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_HEAP_HPP
#define WEBCPP_DOCUMENT_DETAIL_HEAP_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/allocation.hpp>
#include <webcpp/document/errors.hpp>

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/interprocess/mem_algo/rbtree_best_fit.hpp>  // lint-world: a heap in a buffer
#include <boost/interprocess/offset_ptr.hpp>                // lint-world: an offset, no process
#include <boost/interprocess/sync/null_mutex.hpp>  // lint-world: a mutex that locks nothing
#include <boost/throw_exception.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

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
 The offset from an image's start of the first block its heap hands out, the same in every image:
 the header, the heap's own header and as many bytes again, which the region reserves after it,
 rounded up to the heap's alignment, then the block's size word.

 Tip: it repeats `rbtree_best_fit`'s layout, which the format test pins.
*/
inline constexpr std::uint64_t first_block_offset =
    header_bytes +
    (((std::uint64_t{2} * sizeof(heap)) + heap::Alignment - 1) / heap::Alignment *
     heap::Alignment) +
    heap::PayloadPerAllocation + sizeof(heap::size_type);

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
    explicit allocator(heap* h) noexcept : heap_(h) {}

    // The Allocator requirements convert an allocator to its rebound type implicitly, hence the
    // exemption from misc-explicit-constructor.
    /** The allocator of another type over the same heap. */
    template <class U>
    allocator(const allocator<U>& other) noexcept  // NOLINT(misc-explicit-constructor)
        : heap_(other.heap_) {}

    /**
     Allocates room for `n` objects, zeroed; a failure is
     `boost::throw_exception(std::bad_alloc())`.
    */
    pointer allocate(size_type n) {
        if (n > std::numeric_limits<size_type>::max() / sizeof(T)) {
            boost::throw_exception(std::bad_alloc());
        }
        const size_type bytes = n * sizeof(T);
        void* block = heap_->allocate_aligned(bytes, alignof(T));
        if (block == nullptr) {
            boost::throw_exception(std::bad_alloc());
        }
        // Free memory still holds the links the heap wrote there while the block was free.
        std::memset(block, 0, static_cast<std::size_t>(bytes));
        return pointer(static_cast<T*>(block));
    }

    /** Zeroes the room of `n` objects at `p`, then frees it. */
    void deallocate(const pointer& p, size_type n) noexcept {
        if (p == nullptr) {
            return;
        }
        std::memset(static_cast<void*>(p.get()), 0, static_cast<std::size_t>(n * sizeof(T)));
        heap_->deallocate(p.get());
    }

    /** The heap it allocates in. */
    [[nodiscard]] heap* get_heap() const noexcept { return heap_.get(); }

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
    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        if (data == nullptr) {
            return {};
        }
        return {data.get(), static_cast<std::size_t>(size)};
    }

    /** The bytes as UTF-8 code units. */
    [[nodiscard]] std::string_view chars() const noexcept {
        const std::span<const std::byte> bytes = view();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    /** The bytes as UTF-16 code units, in the machine's byte order. */
    [[nodiscard]] std::u16string_view units() const noexcept {
        // A block of the heap is 16-aligned, so its bytes are char16_t's too.
        const std::span<const std::byte> bytes = view();
        return {reinterpret_cast<const char16_t*>(bytes.data()), bytes.size() / sizeof(char16_t)};
    }
};

/**
 Allocates a string of exactly `bytes`' size in `h` and copies them; @ref errc::capacity when there
 is no room.
*/
inline result<byte_string> make_string(heap& h, std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return byte_string{};
    }
    void* block = h.allocate(bytes.size());
    if (block == nullptr) {
        return errc::capacity;
    }
    std::memcpy(block, bytes.data(), bytes.size());
    byte_string made;
    made.data = static_cast<std::byte*>(block);
    made.size = bytes.size();
    return made;
}

/** Zeroes a string's bytes, frees them and leaves `s` empty. */
inline void free_string(heap& h, byte_string& s) noexcept {
    if (s.data != nullptr) {
        std::memset(s.data.get(), 0, static_cast<std::size_t>(s.size));
        h.deallocate(s.data.get());
    }
    s.data = nullptr;
    s.size = 0;
}

/**
 Whether blocks of every size in `sizes` fit in `h` at once; allocates them and frees them all.
*/
inline bool probe(heap& h, std::span<const std::uint64_t> sizes) noexcept {
    // The blocks held are linked through their own first bytes, so a probe of any length needs no
    // memory but the heap's. A size of 0 is a string make_string does not allocate.
    void* last = nullptr;
    bool fits = true;
    for (const std::uint64_t size : sizes) {
        if (size == 0) {
            continue;
        }
        void* block = h.allocate(std::max<std::uint64_t>(size, sizeof last));
        if (block == nullptr) {
            fits = false;
            break;
        }
        std::memcpy(block, static_cast<const void*>(&last), sizeof last);
        last = block;
    }
    // Each link is an address: it is zeroed before its block is freed, so that the image's bytes
    // stay the same at any address.
    while (last != nullptr) {
        void* previous = nullptr;
        std::memcpy(static_cast<void*>(&previous), last, sizeof previous);
        std::memset(last, 0, sizeof previous);
        h.deallocate(last);
        last = previous;
    }
    return fits;
}

/**
 What a heap takes for its blocks, measured in a scratch heap rather than repeated from
 `rbtree_best_fit`: how many bytes an empty heap keeps for itself, and the smallest block; the
 size of any other block follows from them and the heap's public constants.

 A heap whose blocks are allocated one after another, with nothing freed in between, is exactly
 `overhead` plus the sum of its blocks' sizes, so that the size of the image holding a given set of
 blocks is known before the first is allocated.
*/
struct heap_costs {
    /** The bytes of a heap, whatever its size, that no block can use: its own data and its end. */
    std::uint64_t overhead = 0;

    /**
     The bytes `get_free_memory` counts that no block can use: a heap with `get_free_memory() >=
     bytes + slack` free can allocate a block of `bytes`.
    */
    std::uint64_t slack = 0;

    /** The bytes of the smallest block, which a payload of one byte takes. */
    std::uint64_t min_block = 0;

    /**
     The bytes of the block `h.allocate(payload)` takes: the payload past the bytes the control
     data lends it, rounded up to the alignment, and the control bytes, but not below the smallest.

     @param payload The bytes asked for; 0 is a string that is not allocated at all.
     @return The block's bytes; 0 for a payload of 0.
    */
    [[nodiscard]] std::uint64_t block(std::uint64_t payload) const noexcept {
        if (payload == 0) {
            return 0;
        }
        constexpr std::uint64_t lent = sizeof(heap::size_type);
        constexpr std::uint64_t alignment = heap::Alignment;
        const std::uint64_t beyond = payload > lent ? payload - lent : 0;
        const std::uint64_t rounded = (beyond + alignment - 1) / alignment * alignment;
        return std::max(min_block, rounded + heap::PayloadPerAllocation + lent);
    }
};

/**
 Measures a heap's @ref heap_costs in a heap of a scratch buffer on the stack: the smallest block
 by allocating one, the bytes no block can use by asking for the largest block it holds.

 Tip: the numbers follow the Boost release of the build, whose layout the format test pins, and a
 test compares the sizes they predict with the heap's own.
*/
inline heap_costs measure_heap_costs() noexcept {
    constexpr std::size_t scratch_bytes = 512;
    alignas(16) std::array<std::byte, scratch_bytes> scratch{};
    heap* h = new (scratch.data()) heap(scratch.size(), sizeof(heap));
    heap_costs costs;
    const std::uint64_t free_at_start = h->get_free_memory();
    void* smallest = h->allocate(1);
    costs.min_block = free_at_start - h->get_free_memory();
    h->deallocate(smallest);
    // The largest request the heap accepts takes its one block whole.
    for (std::uint64_t payload = free_at_start; payload != 0; --payload) {
        const void* largest = h->allocate(payload);
        if (largest != nullptr) {
            const std::uint64_t usable = free_at_start - h->get_free_memory();
            costs.overhead = scratch_bytes - usable;
            costs.slack = free_at_start - usable;
            break;
        }
    }
    return costs;
}

/**
 The unit an owned image is allocated in: 16 bytes, 16-aligned, so that an allocator rebound to it
 is asked for the alignment the heap needs, a memory resource for an alignment of 16. A buffer an
 allocator gives at another alignment is given back, and the region has no room.
*/
struct alignas(16) image_block {
    /** The block's bytes. */
    std::array<std::byte, 16> bytes;
};

template <class Allocator>
class basic_region;

/**
 The bytes of an image, whatever allocated them: where they are, how many, the size an owned image
 never grows past, and whether they are owned. The tree reads an image through it.
*/
class region_bytes {
public:
    /** The first byte, 16-aligned, or null when empty. */
    [[nodiscard]] std::byte* data() const noexcept { return data_; }

    /** How many bytes. */
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

    /** The size an owned region never grows past. */
    [[nodiscard]] std::uint64_t max_bytes() const noexcept { return max_bytes_; }

    /** Whether the region owns its bytes, and so may grow. */
    [[nodiscard]] bool is_owned() const noexcept { return owned_; }

protected:
    /** No bytes. */
    region_bytes() noexcept = default;

    /** The bytes of a region are moved by the region, which knows their allocator. */
    region_bytes(const region_bytes&) noexcept = default;

    /** The bytes of a region are moved by the region, which knows their allocator. */
    region_bytes& operator=(const region_bytes&) noexcept = default;

    /** Destroyed by the region, which frees what it owns. */
    ~region_bytes() = default;

    /** Takes the fields of `other`, which is left empty. */
    void take(region_bytes& other) noexcept {
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        max_bytes_ = std::exchange(other.max_bytes_, 0);
        owned_ = std::exchange(other.owned_, false);
    }

private:
    template <class>
    friend class basic_region;

    /** The first byte, or null. */
    std::byte* data_ = nullptr;

    /** How many bytes. */
    std::uint64_t size_ = 0;

    /** The size an owned region never grows past. */
    std::uint64_t max_bytes_ = 0;

    /** Whether the bytes are owned. */
    bool owned_ = false;
};

/**
 The bytes of an image: owned, which the region allocates through `Allocator`, zeroes and grows,
 or external, a span its user owns, which never grows; and the allocator, which also holds the
 working state of the operations on the image.

 The allocator's pointer to an owned image's blocks is kept as the allocator gave it, and the
 blocks are given back through it: a pointer that is not a plain one may carry more than an
 address, a segment or a tag, which a pointer rebuilt from the address would not.

 @tparam Allocator An allocator of `std::byte`, rebound to @ref image_block for the image.
*/
template <class Allocator>
class basic_region : public region_bytes {
public:
    /** The allocator of the image and of the working state. */
    using allocator_type = Allocator;

    /** A new owned region of `size` bytes, zeroed, with an empty heap after the header. */
    static result<basic_region> owned(std::uint64_t size, std::uint64_t max_bytes,
                                      const Allocator& a = Allocator()) noexcept {
        if (size > max_bytes || size < smallest() || size > largest()) {
            return errc::capacity;
        }
        basic_region made(a);
        const buffer allocated = made.allocate(size);
        if (allocated.bytes == nullptr) {
            return errc::capacity;
        }
        std::memset(allocated.bytes, 0, static_cast<std::size_t>(size));
        made.hold(allocated, size);
        made.max_bytes_ = max_bytes;
        make_heap(allocated.bytes, size);
        return made;
    }

    /** An owned copy of an image's bytes, whose heap is the image's own. */
    static result<basic_region> owned_copy(std::span<const std::byte> bytes,
                                           std::uint64_t max_bytes,
                                           const Allocator& a = Allocator()) noexcept {
        if (bytes.size() > max_bytes) {
            return errc::capacity;
        }
        // Bytes too few for a heap are no image, and its heap would be read past their end.
        if (bytes.size() < smallest()) {
            return errc::not_a_document;
        }
        basic_region made(a);
        const buffer copy = made.allocate(bytes.size());
        if (copy.bytes == nullptr) {
            return errc::capacity;
        }
        std::memcpy(copy.bytes, bytes.data(), bytes.size());
        made.hold(copy, bytes.size());
        made.max_bytes_ = max_bytes;
        return made;
    }

    /** An external region over `buffer`, as it is: for an image already there. */
    static basic_region external(std::span<std::byte> buffer,
                                 const Allocator& a = Allocator()) noexcept {
        basic_region made(a);
        made.data_ = buffer.data();
        made.size_ = buffer.size();
        made.max_bytes_ = buffer.size();
        return made;
    }

    /** An external region over `buffer`, zeroed, with an empty heap after the header. */
    static result<basic_region> external_empty(std::span<std::byte> buffer,
                                               const Allocator& a = Allocator()) noexcept {
        // The heap's offsets are the same at every 16-aligned address, and only there.
        if (buffer.size() < smallest() ||
            reinterpret_cast<std::uintptr_t>(buffer.data()) % 16 != 0) {
            return errc::capacity;
        }
        std::memset(buffer.data(), 0, buffer.size());
        make_heap(buffer.data(), buffer.size());
        return external(buffer, a);
    }

    /** An empty region, which holds no bytes, with a default allocator. */
    basic_region() noexcept(std::is_nothrow_default_constructible_v<Allocator>) = default;

    /** An empty region, which holds no bytes, with the allocator `a`. */
    explicit basic_region(const Allocator& a) noexcept : alloc_(a) {}

    /** Takes the bytes and a copy of the allocator of `other`, which is left empty. */
    basic_region(basic_region&& other) noexcept
        : alloc_(other.alloc_), kept_(std::exchange(other.kept_, kept_pointer{})) {
        take(other);
    }

    /**
     Frees its own bytes, then takes the bytes of `other`, which is left empty; the allocator is
     taken only when it propagates on a move, else it equals `other`'s or `other` owns nothing.
    */
    basic_region& operator=(basic_region&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        release();
        if constexpr (block_traits::propagate_on_container_move_assignment::value) {
            alloc_ = other.alloc_;
        } else {
            BOOST_ASSERT(alloc_ == other.alloc_ || !other.owned_);
        }
        take(other);
        kept_ = std::exchange(other.kept_, kept_pointer{});
        return *this;
    }

    /** A region is not copied: an image is copied by @ref owned_copy. */
    basic_region(const basic_region&) = delete;

    /** A region is not copied: an image is copied by @ref owned_copy. */
    basic_region& operator=(const basic_region&) = delete;

    /** Frees an owned region's bytes. */
    ~basic_region() { release(); }

    /** The allocator. */
    [[nodiscard]] Allocator get_allocator() const noexcept { return alloc_; }

    /**
     Grows an owned region to twice its size or `at_least`, the larger, rounded up to 16, at most
     `max_bytes`: the bytes are copied, the rest zeroed and the heap grown; false when it cannot.
    */
    bool grow(std::uint64_t at_least) noexcept {
        // The limit is rounded down to 16 so that every size a region grows to is a multiple of it.
        const std::uint64_t limit = std::min(max_bytes_, largest()) / 16 * 16;
        if (!owned_ || data_ == nullptr || at_least > limit) {
            return false;
        }
        const std::uint64_t doubled = size_ > limit / 2 ? limit : 2 * size_;
        const std::uint64_t wanted = std::max(doubled, at_least);
        const std::uint64_t new_size = std::min((wanted + 15) / 16 * 16, limit);
        if (new_size <= size_) {
            return false;
        }
        const buffer bigger = allocate(new_size);
        if (bigger.bytes == nullptr) {
            return false;
        }
        const std::uint64_t old_size = size_;
        std::memcpy(bigger.bytes, data_, static_cast<std::size_t>(old_size));
        std::memset(bigger.bytes + old_size, 0, static_cast<std::size_t>(new_size - old_size));
        deallocate(held(), old_size);
        hold(bigger, new_size);
        // The heap's links are offsets, so the copy is a heap already: it only takes the new end.
        own_heap().grow(new_size - old_size);
        return true;
    }

    /**
     Moves an owned region's first `size` bytes into a buffer of that size; false when it cannot.
    */
    bool shrink_to(std::uint64_t size) noexcept {
        if (!owned_ || data_ == nullptr || size > size_) {
            return false;
        }
        // The heap must already end within `size`: its own shrink_to_fit has given its end back.
        if (size < header_bytes + own_heap().get_size()) {
            return false;
        }
        if (size == size_) {
            return true;
        }
        const buffer smaller = allocate(size);
        if (smaller.bytes == nullptr) {
            return false;
        }
        std::memcpy(smaller.bytes, data_, static_cast<std::size_t>(size));
        deallocate(held(), size_);
        hold(smaller, size);
        return true;
    }

private:
    /** The allocator of the image's blocks. */
    using block_allocator = std::allocator_traits<Allocator>::template rebind_alloc<image_block>;

    /** Its traits. */
    using block_traits = std::allocator_traits<block_allocator>;

    /** Its pointer to the image's blocks. */
    using block_pointer = block_traits::pointer;

    /** Whether that pointer is a plain one, which the bytes' address gives back exactly. */
    static constexpr bool plain_pointer = std::is_pointer_v<block_pointer>;

    /** What is kept in the place of a plain pointer, which the bytes' address is: nothing. */
    struct no_pointer {};

    /** The allocator's pointer to an owned image's blocks, kept when it is not a plain one. */
    using kept_pointer = std::conditional_t<plain_pointer, no_pointer, block_pointer>;

    /** A buffer the allocator gave: its bytes, and its pointer when it is not a plain one. */
    struct buffer {
        /** The first byte, 16-aligned, or null when the allocator gave none. */
        std::byte* bytes = nullptr;

        /** The allocator's pointer to it. */
        BOOST_ATTRIBUTE_NO_UNIQUE_ADDRESS kept_pointer pointer{};
    };

    /** Whether the allocator is the standard one, whose `allocate` is the global operator new. */
    static constexpr bool standard = std::same_as<Allocator, std::allocator<std::byte>>;

    /** The fewest bytes that hold a header and an empty heap. */
    static std::uint64_t smallest() noexcept {
        return header_bytes + heap::get_min_size(sizeof(heap));
    }

    /** The most bytes this machine's address space allows a region. */
    static constexpr std::uint64_t largest() noexcept {
        return std::numeric_limits<std::size_t>::max();
    }

    /** How many blocks hold `size` bytes. */
    static std::size_t blocks_of(std::uint64_t size) noexcept {
        return static_cast<std::size_t>((size + sizeof(image_block) - 1) / sizeof(image_block));
    }

    /**
     A new buffer of `size` bytes, 16-aligned and uninitialized, or none when the allocator has
     none: with exceptions, its failure is caught (@ref unless_allocation_fails). A buffer the
     allocator gives at another alignment, as one that ignores an over-aligned request may, is
     given back, and is none too.
    */
    buffer allocate(std::uint64_t size) noexcept {
        if constexpr (standard) {
            // std::allocator allocates with the global operator new: its form that returns null
            // keeps a failure a value without exceptions too.
            return {static_cast<std::byte*>(::operator new(static_cast<std::size_t>(size),
                                                           std::align_val_t{16}, std::nothrow))};
        } else {
            block_allocator blocks(alloc_);
            const std::size_t count = blocks_of(size);
            if (count > block_traits::max_size(blocks)) {
                return {};
            }
            return unless_allocation_fails(
                [&] {
                    return aligned_or_given_back(blocks, block_traits::allocate(blocks, count),
                                                 count);
                },
                [] { return buffer{}; });
        }
    }

    /**
     The buffer of the `count` blocks at `p`, which `blocks` allocated, when they are 16-aligned;
     else none, the blocks given back: the heap's offsets hold at 16-aligned addresses alone.
    */
    static buffer aligned_or_given_back(block_allocator& blocks, block_pointer p,
                                        std::size_t count) noexcept {
        auto* bytes = reinterpret_cast<std::byte*>(std::to_address(p));
        if (bytes == nullptr) {
            return {};
        }
        if (reinterpret_cast<std::uintptr_t>(bytes) % alignof(image_block) != 0) {
            block_traits::deallocate(blocks, std::move(p), count);
            return {};
        }
        buffer made{bytes};
        if constexpr (!plain_pointer) {
            made.pointer = std::move(p);
        }
        return made;
    }

    /** Frees a buffer of `size` bytes that @ref allocate made, through the allocator's pointer. */
    void deallocate(const buffer& b, std::uint64_t size) noexcept {
        if constexpr (standard) {
            ::operator delete(b.bytes, std::align_val_t{16});
        } else {
            block_allocator blocks(alloc_);
            if constexpr (plain_pointer) {
                block_traits::deallocate(blocks, reinterpret_cast<image_block*>(b.bytes),
                                         blocks_of(size));
            } else {
                block_traits::deallocate(blocks, b.pointer, blocks_of(size));
            }
        }
    }

    /** The owned buffer it holds. */
    [[nodiscard]] buffer held() const noexcept { return {data_, kept_}; }

    /** Holds `b`, of `size` bytes, as its owned bytes. */
    void hold(const buffer& b, std::uint64_t size) noexcept {
        data_ = b.bytes;
        kept_ = b.pointer;
        size_ = size;
        owned_ = true;
    }

    /** Its heap, as @ref heap_of finds it. */
    [[nodiscard]] heap& own_heap() const noexcept {
        return *std::launder(reinterpret_cast<heap*>(data_ + header_bytes));
    }

    /**
     Builds an empty heap after the header of `size` zeroed bytes, its first block where
     @ref first_block_offset says.
    */
    static void make_heap(std::byte* bytes, std::uint64_t size) noexcept {
        new (bytes + header_bytes) heap(size - header_bytes, sizeof(heap));
    }

    /** Frees its bytes when it owns them, and leaves it empty. */
    void release() noexcept {
        if (owned_ && data_ != nullptr) {
            deallocate(held(), size_);
        }
        data_ = nullptr;
        kept_ = kept_pointer{};
        size_ = 0;
        max_bytes_ = 0;
        owned_ = false;
    }

    BOOST_ATTRIBUTE_NO_UNIQUE_ADDRESS Allocator alloc_{};

    /** The allocator's pointer to an owned image's blocks, when it is not a plain one. */
    BOOST_ATTRIBUTE_NO_UNIQUE_ADDRESS kept_pointer kept_{};
};

/** The region of an image in the standard allocator. */
using region = basic_region<std::allocator<std::byte>>;

/** The heap of a region, at offset @ref header_bytes. */
inline heap& heap_of(const region_bytes& r) noexcept {
    return *std::launder(reinterpret_cast<heap*>(r.data() + header_bytes));
}

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_HEAP_HPP
