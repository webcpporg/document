// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 Shared documents: a pool of blocks over memory the program gives, each block a header that counts
 the references to the image after it, and the document that holds one reference, as
 `std::shared_ptr` holds its object.

 The pool works through offsets from its region's start, never addresses, so every process that
 maps the region, at whatever address, uses the same pool. Its counts are lock-free atomics, which
 are address-free, and its free blocks a lock-free stack whose top carries a tag against ABA: no
 lock is held at any moment, so no thread or process waits on another, and one that dies leaves no
 lock behind. The count lives in the block's header, never in the image, which stays the document's
 content, the same bytes wherever it lives.
*/

#ifndef WEBCPP_DOCUMENT_SHARED_HPP
#define WEBCPP_DOCUMENT_SHARED_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/image.hpp>
#include <webcpp/document/traits.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace webcpp::document {

namespace detail {

// A count in memory several processes map must work there without a lock of the process's own:
// a lock-free atomic is address-free.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "a shared pool needs lock-free 64-bit atomics");

/** The first eight bytes of a shared pool's region: `WCPPPOOL`. */
inline constexpr std::array<char, 8> shared_pool_magic{'W', 'C', 'P', 'P', 'P', 'O', 'O', 'L'};

/** The version of a shared pool's layout. */
inline constexpr std::uint32_t shared_pool_version = 1;

/** The size of a shared pool's header, at its region's start. */
inline constexpr std::size_t shared_header_bytes = 64;

/** The size of a block's header, before its image. */
inline constexpr std::size_t shared_block_header_bytes = 64;

/** The most blocks a pool has: a block's index plus one fits in 32 bits, 0 meaning none. */
inline constexpr std::size_t shared_most_blocks = 0xFFFFFFFE;

/** The header of a shared pool, at its region's start. */
struct shared_pool_header {
    /** The pool's magic, @ref shared_pool_magic. */
    std::array<char, 8> magic{};

    /** The version of the pool's layout, @ref shared_pool_version. */
    std::uint32_t version = 0;

    /** The byte-order mark, as the machine that formatted the pool writes it. */
    std::uint32_t byte_order = 0;

    /** The size of each block's image. */
    std::uint64_t block_bytes = 0;

    /** How many blocks. */
    std::uint64_t blocks = 0;

    /**
     The top of the stack of free blocks: its index plus one in the low 32 bits, 0 when every
     block is leased, and in the high 32 a tag that each change of the top raises.
    */
    std::atomic<std::uint64_t> top{0};

    /** How many blocks are free. */
    std::atomic<std::uint64_t> available{0};

    /** Zero. */
    std::array<std::byte, 16> reserved{};
};

static_assert(sizeof(shared_pool_header) == shared_header_bytes, "a pool's header is 64 bytes");

/** The header of a block, before its image. */
struct shared_block_header {
    /** How many references the block's document has: 0 for a free block. */
    std::atomic<std::uint64_t> count{0};

    /** While the block is free, the next free block's index plus one, or 0. */
    std::atomic<std::uint64_t> next{0};

    /** The size of the image the block holds, which its last reference zeroes: 0 when free. */
    std::uint64_t size = 0;

    /** Zero. */
    std::array<std::byte, 40> reserved{};
};

static_assert(sizeof(shared_block_header) == shared_block_header_bytes,
              "a block's header is 64 bytes");

/** The blocks of a shared pool, reached from the start of its region in this process. */
class shared_blocks {
public:
    /** The index of no block. */
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

    /** The pool's header. */
    static shared_pool_header& pool(std::byte* base) noexcept {
        return *std::launder(reinterpret_cast<shared_pool_header*>(base));
    }

    /** The distance from one block to the next. */
    static std::size_t stride(std::byte* base) noexcept {
        return shared_block_header_bytes + static_cast<std::size_t>(pool(base).block_bytes);
    }

    /** The offset of block `index`'s image from the region's start. */
    static std::size_t image_offset(std::byte* base, std::size_t index) noexcept {
        return shared_header_bytes + (index * stride(base)) + shared_block_header_bytes;
    }

    /** The header of block `index`. */
    static shared_block_header& block(std::byte* base, std::size_t index) noexcept {
        return *std::launder(reinterpret_cast<shared_block_header*>(
            base + image_offset(base, index) - shared_block_header_bytes));
    }

    /** The image of block `index`: the bytes its header's size states. */
    static std::span<std::byte> image(std::byte* base, std::size_t index) noexcept {
        return {base + image_offset(base, index),
                static_cast<std::size_t>(block(base, index).size)};
    }

    /** The whole room of block `index`'s image. */
    static std::span<std::byte> room(std::byte* base, std::size_t index) noexcept {
        return {base + image_offset(base, index), static_cast<std::size_t>(pool(base).block_bytes)};
    }

    /** Takes the free block on top, with no reference yet, or returns @ref none. */
    static std::size_t acquire(std::byte* base) noexcept {
        shared_pool_header& header = pool(base);
        // Acquire: the block's link, and its zeroed bytes, are what the thread that gave it back
        // wrote before its release.
        std::uint64_t top = header.top.load(std::memory_order_acquire);
        for (;;) {
            const std::uint64_t first = top & index_mask;
            if (first == 0) {
                return none;
            }
            const std::uint64_t next = block(base, static_cast<std::size_t>(first - 1))
                                           .next.load(std::memory_order_relaxed);
            // The tag changes with every top, so a top taken and given back since it was read
            // fails the exchange.
            if (header.top.compare_exchange_weak(top, raised(top) | next, std::memory_order_acquire,
                                                 std::memory_order_acquire)) {
                header.available.fetch_sub(1, std::memory_order_relaxed);
                const auto index = static_cast<std::size_t>(first - 1);
                block(base, index).next.store(0, std::memory_order_relaxed);
                return index;
            }
        }
    }

    /** Zeroes a block whose last reference is gone and gives it back. */
    static void give_back(std::byte* base, std::size_t index) noexcept {
        shared_block_header& freed = block(base, index);
        const std::span<std::byte> bytes = image(base, index);
        std::memset(bytes.data(), 0, bytes.size());
        freed.size = 0;
        shared_pool_header& header = pool(base);
        std::uint64_t top = header.top.load(std::memory_order_relaxed);
        // Release: whoever takes the block next sees it zeroed.
        do {
            freed.next.store(top & index_mask, std::memory_order_relaxed);
        } while (!header.top.compare_exchange_weak(
            top, raised(top) | (index + 1), std::memory_order_release, std::memory_order_relaxed));
        header.available.fetch_add(1, std::memory_order_relaxed);
    }

    /** Drops one reference to block `index`; the last one zeroes the block and gives it back. */
    static void drop(std::byte* base, std::size_t index) noexcept {
        // Acquire and release: every holder's writes happen before the last one zeroes the block.
        if (block(base, index).count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            give_back(base, index);
        }
    }

private:
    static constexpr std::uint64_t index_mask = 0xFFFFFFFF;

    /** The tag of `top` raised by one, wrapping, in the high 32 bits, its low 32 bits zero. */
    static constexpr std::uint64_t raised(std::uint64_t top) noexcept {
        return ((top >> 32U) + 1U) << 32U;
    }
};

}  // namespace detail

template <class Traits>
class shared_document;

/**
 A pool of blocks for shared documents, over a region of memory the program gives: the process
 heap, or a region of shared memory that several processes map, each at an address of its own.

 The region holds a 64-byte header, then the blocks, each a 64-byte header, which counts the
 references to its document, and the room of one image. A @ref shared_pool is a view of the region,
 copied freely: the pool is the region's, and every view of it, in any process, is the same pool.
 Leasing a block and giving one back are lock-free and safe from any number of threads and
 processes at once; nothing is allocated, and a free block is all zero.

 @note A process that dies holding references leaves their blocks counted for good, as
 Boost.Interprocess's `shared_ptr` does; recovering them is the program's.
 @see "Images", in the guide.
*/
class shared_pool {
public:
    /** The size of the pool's header, at its region's start. */
    static constexpr std::size_t header_bytes = detail::shared_header_bytes;

    /** The size of a block's header, before its image. */
    static constexpr std::size_t block_header_bytes = detail::shared_block_header_bytes;

    /**
     The size of a region that holds `blocks` blocks of `block_bytes` each.

     @param block_bytes The room of each block's image, a multiple of 16 and at least 64.
     @param blocks How many blocks, at least one and at most 4,294,967,294.
     @return The size; @ref errc::range for a size or a count outside those bounds, and
     @ref errc::capacity when the size overflows.
    */
    static result<std::size_t> region_bytes(std::size_t block_bytes, std::size_t blocks) noexcept {
        if (!valid_block_bytes(block_bytes) || blocks == 0 || blocks > detail::shared_most_blocks) {
            return errc::range;
        }
        constexpr std::size_t most = std::numeric_limits<std::size_t>::max();
        const std::size_t stride = block_header_bytes + block_bytes;
        if (stride < block_bytes || blocks > (most - header_bytes) / stride) {
            return errc::capacity;
        }
        return header_bytes + (blocks * stride);
    }

    /**
     Formats a region as a new pool of as many blocks of `block_bytes` as it holds, every one free
     and zero.

     @param region The region, 16-aligned, which the program keeps mapped while the pool and its
     documents are used.
     @param block_bytes The room of each block's image, a multiple of 16 and at least 64: the
     largest image a block holds.
     @return The pool; @ref errc::range for a region that is not 16-aligned or a size outside those
     bounds, @ref errc::capacity for a region too small for one block.
    */
    static result<shared_pool> format(std::span<std::byte> region,
                                      std::size_t block_bytes) noexcept {
        if (!valid_block_bytes(block_bytes) ||
            reinterpret_cast<std::uintptr_t>(region.data()) % 16 != 0) {
            return errc::range;
        }
        const std::size_t stride = block_header_bytes + block_bytes;
        if (stride < block_bytes || region.size() < header_bytes ||
            (region.size() - header_bytes) / stride == 0) {
            return errc::capacity;
        }
        const std::size_t blocks =
            std::min((region.size() - header_bytes) / stride, detail::shared_most_blocks);
        const std::size_t size = header_bytes + (blocks * stride);
        std::memset(region.data(), 0, size);

        auto* header = new (region.data()) detail::shared_pool_header();
        header->magic = detail::shared_pool_magic;
        header->version = detail::shared_pool_version;
        header->byte_order = byte_order_mark;
        header->block_bytes = block_bytes;
        header->blocks = blocks;
        // The free blocks are a stack, the first block on top: a pool hands out its blocks in
        // order, and a block given back is the next one handed out.
        for (std::size_t i = 0; i < blocks; ++i) {
            auto* block =
                new (region.data() + header_bytes + (i * stride)) detail::shared_block_header();
            block->next.store(i + 1 < blocks ? i + 2 : 0, std::memory_order_relaxed);
        }
        header->available.store(blocks, std::memory_order_relaxed);
        // Release: a thread that attaches through this store sees the blocks' links.
        header->top.store(1, std::memory_order_release);
        return shared_pool(region.first(size));
    }

    /**
     Attaches to a pool another view formatted, in this process or in another that maps the same
     region at any address.

     @param region The region, 16-aligned, which holds the pool from its first byte.
     @return The pool; @ref errc::not_a_document for a region that is not 16-aligned, holds no pool
     or is shorter than its pool, @ref errc::incompatible_document for a pool of another layout
     version or byte order.
    */
    static result<shared_pool> attach(std::span<std::byte> region) noexcept {
        if (reinterpret_cast<std::uintptr_t>(region.data()) % 16 != 0 ||
            region.size() < header_bytes) {
            return errc::not_a_document;
        }
        // Copied, not read in place: the region may hold anything.
        std::array<char, 8> magic{};
        std::uint32_t version = 0;
        std::uint32_t byte_order = 0;
        std::uint64_t block_bytes = 0;
        std::uint64_t blocks = 0;
        std::memcpy(magic.data(), region.data() + offsetof(detail::shared_pool_header, magic),
                    magic.size());
        std::memcpy(&version, region.data() + offsetof(detail::shared_pool_header, version),
                    sizeof version);
        std::memcpy(&byte_order, region.data() + offsetof(detail::shared_pool_header, byte_order),
                    sizeof byte_order);
        std::memcpy(&block_bytes, region.data() + offsetof(detail::shared_pool_header, block_bytes),
                    sizeof block_bytes);
        std::memcpy(&blocks, region.data() + offsetof(detail::shared_pool_header, blocks),
                    sizeof blocks);
        if (magic != detail::shared_pool_magic) {
            return errc::not_a_document;
        }
        if (version != detail::shared_pool_version || byte_order != byte_order_mark) {
            return errc::incompatible_document;
        }
        if (block_bytes > std::numeric_limits<std::size_t>::max() ||
            blocks > detail::shared_most_blocks) {
            return errc::not_a_document;
        }
        const auto size =
            region_bytes(static_cast<std::size_t>(block_bytes), static_cast<std::size_t>(blocks));
        if (!size.has_value() || size.unsafe_value() > region.size()) {
            return errc::not_a_document;
        }
        return shared_pool(region.first(size.unsafe_value()));
    }

    /** A view of no pool, which has no block. */
    shared_pool() noexcept = default;

    /**
     The room of each block's image.

     @return The size, 0 for a view of no pool.
    */
    [[nodiscard]] std::size_t block_bytes() const noexcept {
        return base_ == nullptr
                   ? 0
                   : static_cast<std::size_t>(detail::shared_blocks::pool(base_).block_bytes);
    }

    /**
     How many blocks the pool has.

     @return The count, 0 for a view of no pool.
    */
    [[nodiscard]] std::size_t blocks() const noexcept {
        return base_ == nullptr
                   ? 0
                   : static_cast<std::size_t>(detail::shared_blocks::pool(base_).blocks);
    }

    /**
     How many blocks are free.

     @return The count, exact when no block is being leased or given back at the same moment; 0
     for a view of no pool.
    */
    [[nodiscard]] std::size_t available() const noexcept {
        return base_ == nullptr
                   ? 0
                   : static_cast<std::size_t>(detail::shared_blocks::pool(base_).available.load(
                         std::memory_order_relaxed));
    }

    /**
     The pool's bytes, its header and its blocks, at the start of the region it was made over.

     @return The bytes, empty for a view of no pool.
    */
    [[nodiscard]] std::span<std::byte> region() const noexcept { return {base_, size_}; }

    /**
     Gives up one reference to a document by its image's offset, without opening the image; the
     last reference zeroes the block and gives it back.

     A receiver that refuses an offset it was handed, because `adopt` refused its image as damaged
     or of another layout, or because the program does not want it, gives the reference back so:
     its image is never read, so a broken header cannot make the block leak.

     @param offset The image's offset from the region's start, as `release` returned it.
     @return Success; @ref errc::range for an offset that is no block's image, or a block no one
     holds, which changes nothing.
    */
    [[nodiscard]] result<void> drop(std::uint64_t offset) const noexcept {
        const auto held = block_at(offset);
        if (!held.has_value()) {
            return held.error();
        }
        const std::size_t index = held.unsafe_value();
        std::atomic<std::uint64_t>& count = detail::shared_blocks::block(base_, index).count;
        // Never below zero, should another holder drop the last reference meanwhile.
        std::uint64_t seen = count.load(std::memory_order_relaxed);
        do {
            if (seen == 0) {
                return errc::range;
            }
        } while (!count.compare_exchange_weak(seen, seen - 1, std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
        if (seen == 1) {
            detail::shared_blocks::give_back(base_, index);
        }
        return {};
    }

private:
    template <class>
    friend class shared_document;

    /**
     The index of the block whose image is at `offset`, held by someone; @ref errc::range for any
     other offset.
    */
    [[nodiscard]] result<std::size_t> block_at(std::uint64_t offset) const noexcept {
        constexpr std::uint64_t first = header_bytes + block_header_bytes;
        if (base_ == nullptr || offset < first) {
            return errc::range;
        }
        const std::uint64_t stride = detail::shared_blocks::stride(base_);
        if ((offset - first) % stride != 0 || (offset - first) / stride >= blocks()) {
            return errc::range;
        }
        const auto index = static_cast<std::size_t>((offset - first) / stride);
        if (detail::shared_blocks::block(base_, index).count.load(std::memory_order_acquire) == 0) {
            return errc::range;
        }
        return index;
    }

    /** A view of the pool `region` holds. */
    explicit shared_pool(std::span<std::byte> region) noexcept
        : base_(region.data()), size_(region.size()) {}

    /** Whether a block's room holds a header and keeps the next block's image 16-aligned. */
    static constexpr bool valid_block_bytes(std::size_t block_bytes) noexcept {
        return block_bytes >= sizeof(image_header) && block_bytes % 16 == 0;
    }

    std::byte* base_ = nullptr;
    std::size_t size_ = 0;
};

/**
 A document in a block of a @ref shared_pool, holding one reference to it, as `std::shared_ptr`
 holds its object: copying adds a reference, destroying drops one, and the last reference zeroes
 the block and gives it back to the pool.

 Every holder reads and writes the same image, in place: a write through one is every holder's.
 Whether several holders may write at once is the program's decision, and so is the
 synchronisation of writers, with each other and with readers, which the library does not do. A
 holder that wants a document of its own to change calls @ref make_mutable, which copies the image
 into a new block when others hold it (copy on write). The references themselves are thread-safe:
 copies made and dropped on any number of threads and processes at once, as `std::shared_ptr`'s
 are. A reference passes to another process as its image's offset in the region:
 @ref release gives it up without dropping it, and @ref adopt takes it over.

 The document is external: a change that needs more room than its block has returns
 @ref errc::capacity. A shared document allocates nothing; moving it invalidates its handles.

 @tparam Traits The traits of the document.
 @see "Images", in the guide.
*/
template <class Traits = default_traits>
class shared_document {
public:
    /**
     Makes a new, empty document, holding `null`, in a free block of `pool`, whose image is the
     whole block.

     @param pool The pool, whose region outlives the document.
     @return The document, its only holder; @ref errc::capacity when every block is leased, or when
     a block is too small for an empty document.
    */
    static result<shared_document> create(shared_pool pool) noexcept {
        if (pool.base_ == nullptr) {
            return errc::capacity;
        }
        std::byte* base = pool.base_;
        const std::size_t index = detail::shared_blocks::acquire(base);
        if (index == detail::shared_blocks::none) {
            return errc::capacity;
        }
        const std::span<std::byte> room = detail::shared_blocks::room(base, index);
        // Its size first, so that a block given back on a failure is zeroed whole.
        detail::shared_blocks::block(base, index).size = room.size();
        auto formatted = webcpp::document::format<Traits>(room);
        if (!formatted.has_value()) {
            detail::shared_blocks::give_back(base, index);
            return formatted.error();
        }
        return shared_document(base, index, room.size(), std::move(formatted.unsafe_value()));
    }

    /**
     Copies an image into a free block of `pool`, where it then has the whole block's room.

     A sealed image is checked against its seal first; the image grows to its block, which clears
     its seal unless it already filled one.

     @param pool The pool, whose region outlives the document.
     @param bytes The image, at any address.
     @param check Whether a sealed image's bytes are checked against its seal.
     @return The document, its only holder; the errors of the function `open`, and
     @ref errc::capacity for an image larger than a block or when every block is leased.
    */
    static result<shared_document> from_image(shared_pool pool, std::span<const std::byte> bytes,
                                              verify check = verify::seal) noexcept {
        if (const auto checked = check_image(bytes, detail::layout_tag<Traits>(), check);
            !checked.has_value()) {
            return checked.error();
        }
        std::uint64_t size = 0;
        std::memcpy(&size, bytes.data() + offsetof(image_header, size), sizeof size);
        if (pool.base_ == nullptr || size > pool.block_bytes()) {
            return errc::capacity;
        }
        std::byte* base = pool.base_;
        const std::size_t index = detail::shared_blocks::acquire(base);
        if (index == detail::shared_blocks::none) {
            return errc::capacity;
        }
        const std::span<std::byte> room = detail::shared_blocks::room(base, index);
        const auto used = static_cast<std::size_t>(size);
        detail::shared_blocks::block(base, index).size = used;
        std::memcpy(room.data(), bytes.data(), used);
        // The heap is checked before it grows into the rest of the block, which is zero.
        if (const auto opened = webcpp::document::open<Traits>(room.first(used), verify::none);
            !opened.has_value()) {
            detail::shared_blocks::give_back(base, index);
            return opened.error();
        }
        if (used < room.size()) {
            std::launder(reinterpret_cast<detail::heap*>(room.data() + detail::header_bytes))
                ->grow(room.size() - used);
            const std::uint64_t whole = room.size();
            const std::uint64_t unsealed = 0;
            std::memcpy(room.data() + offsetof(image_header, size), &whole, sizeof whole);
            std::memcpy(room.data() + offsetof(image_header, seal), &unsealed, sizeof unsealed);
        }
        return shared_document(base, index, room.size(),
                               detail::adopt<Traits, external>(detail::region::external(room)));
    }

    /**
     Takes over a reference that @ref release gave up, in this process or in another that maps the
     same region.

     @param pool The pool, whose region outlives the document.
     @param offset The image's offset from the region's start, which @ref release returned.
     @param check Whether a sealed image's bytes are checked against its seal.
     @return The document; @ref errc::range for an offset that is no block's image or a block that
     no one holds; the errors of the function `open`, which leave the reference as it was: a
     receiver that refuses it gives it back with @ref shared_pool::drop.
    */
    static result<shared_document> adopt(shared_pool pool, std::uint64_t offset,
                                         verify check = verify::seal) noexcept {
        const auto held = pool.block_at(offset);
        if (!held.has_value()) {
            return held.error();
        }
        std::byte* base = pool.base_;
        const std::size_t index = held.unsafe_value();
        const std::span<std::byte> image = detail::shared_blocks::image(base, index);
        auto opened = webcpp::document::open<Traits>(image, check);
        if (!opened.has_value()) {
            return opened.error();
        }
        return shared_document(base, index, std::move(opened.unsafe_value()));
    }

    /** A holder of no document. */
    shared_document() noexcept = default;

    /**
     Adds a reference to the document of `other`.

     @param other The shared document.
    */
    shared_document(const shared_document& other) noexcept
        : base_(other.base_), index_(other.index_), document_(other.attached()) {
        if (base_ != nullptr) {
            detail::shared_blocks::block(base_, index_)
                .count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     Takes the reference of `other`, which holds none after it.

     @param other The shared document.
    */
    shared_document(shared_document&& other) noexcept
        : base_(std::exchange(other.base_, nullptr)),
          index_(std::exchange(other.index_, 0)),
          document_(std::move(other.document_)) {}

    /**
     Adds a reference to the document of `other`, then drops this one's.

     @param other The shared document.
     @return This shared document.
    */
    shared_document& operator=(const shared_document& other) noexcept {
        if (this != &other) {
            // The other's reference first: dropping this one's may not free the block both hold.
            if (other.base_ != nullptr) {
                detail::shared_blocks::block(other.base_, other.index_)
                    .count.fetch_add(1, std::memory_order_relaxed);
            }
            drop();
            base_ = other.base_;
            index_ = other.index_;
            document_ = other.attached();
        }
        return *this;
    }

    /**
     Drops this one's reference, then takes the reference of `other`, which holds none after it.

     @param other The shared document.
     @return This shared document.
    */
    shared_document& operator=(shared_document&& other) noexcept {
        if (this != &other) {
            drop();
            base_ = std::exchange(other.base_, nullptr);
            index_ = std::exchange(other.index_, 0);
            document_ = std::move(other.document_);
        }
        return *this;
    }

    /** Drops the reference; the last one zeroes the block and gives it back. */
    ~shared_document() { drop(); }

    /**
     How many references the document has, in every process.

     @return The count, which other holders may change at any moment; 0 for a holder of no
     document.
    */
    [[nodiscard]] std::uint64_t use_count() const noexcept {
        if (base_ == nullptr) {
            return 0;
        }
        return detail::shared_blocks::block(base_, index_).count.load(std::memory_order_acquire);
    }

    /**
     Makes the document this holder's alone to change: the same block when it holds the only
     reference, else a copy of the image in a new block, this holder's only, while the other
     holders keep the document as it was.

     The copy is the image's bytes, its seal included. Reading the image to copy it races with a
     holder that writes it at the same moment, which the program's synchronisation prevents.

     @return Success; @ref errc::capacity when a copy is needed and every block is leased, the
     holder then keeping its reference; @ref errc::type for a holder of no document.
    */
    result<void> make_mutable() noexcept {
        if (base_ == nullptr) {
            return errc::type;
        }
        if (use_count() == 1) {
            return {};
        }
        const std::size_t index = detail::shared_blocks::acquire(base_);
        if (index == detail::shared_blocks::none) {
            return errc::capacity;
        }
        const std::span<std::byte> from = detail::shared_blocks::image(base_, index_);
        const std::span<std::byte> room = detail::shared_blocks::room(base_, index);
        std::memcpy(room.data(), from.data(), from.size());
        set_up(base_, index, from.size());
        // The document first, which stops reading the old block, then the old reference.
        const std::size_t old = index_;
        document_ =
            detail::adopt<Traits, external>(detail::region::external(room.first(from.size())));
        index_ = index;
        detail::shared_blocks::drop(base_, old);
        return {};
    }

    /**
     Gives up this holder's reference without dropping it, for @ref adopt to take over, in this
     process or in another that maps the same region.

     @return The image's offset from the region's start, which @ref adopt takes; 0 for a holder of
     no document. The holder holds nothing after it.
    */
    [[nodiscard]] std::uint64_t release() noexcept {
        if (base_ == nullptr) {
            return 0;
        }
        const std::uint64_t offset = this->offset();
        document_ = detail::adopt<Traits, external>(detail::region{});
        base_ = nullptr;
        index_ = 0;
        return offset;
    }

    /**
     The image's offset from the start of the pool's region, the same in every process.

     @return The offset; 0 for a holder of no document.
    */
    [[nodiscard]] std::uint64_t offset() const noexcept {
        return base_ == nullptr ? 0 : detail::shared_blocks::image_offset(base_, index_);
    }

    /**
     Seals the image, as the document's own `seal` does: the next change, through any holder,
     clears it.

     @return Success; @ref errc::type for a holder of no document.
    */
    result<void> seal() noexcept { return document_.seal(); }

    /**
     Whether the image carries a seal.

     @return Whether it is sealed; false for a holder of no document.
    */
    [[nodiscard]] bool sealed() const noexcept { return document_.sealed(); }

    /**
     The document.

     @return The document, valid while this holder neither moves, drops nor releases it.
    */
    basic_document<Traits, external>& get() noexcept { return document_; }

    /**
     The document, read only.

     @return The document, valid while this holder neither moves, drops nor releases it.
    */
    [[nodiscard]] const basic_document<Traits, external>& get() const noexcept { return document_; }

    /**
     The handle of the document's value.

     @return The handle, which reads and writes.
    */
    basic_value<Traits, external> root() noexcept { return document_.root(); }

    /**
     The handle of the document's value, read only.

     @return The handle.
    */
    [[nodiscard]] basic_const_value<Traits, external> root() const noexcept {
        return document_.root();
    }

    /**
     The member `key` of the document's value, as `root()[key]`.

     @param key The key, in UTF-8.
     @return The handle, which never fails.
    */
    basic_value<Traits, external> operator[](std::string_view key) noexcept {
        return document_[key];
    }

    /**
     The element `index` of the document's value, as `root()[index]`.

     @param index The index.
     @return The handle, which never fails.
    */
    basic_value<Traits, external> operator[](std::uint64_t index) noexcept {
        return document_[index];
    }

    /**
     The member `key` of the document's value, read only.

     @param key The key, in UTF-8.
     @return The handle, which never fails.
    */
    basic_const_value<Traits, external> operator[](std::string_view key) const noexcept {
        return document_[key];
    }

    /**
     The element `index` of the document's value, read only.

     @param index The index.
     @return The handle, which never fails.
    */
    basic_const_value<Traits, external> operator[](std::uint64_t index) const noexcept {
        return document_[index];
    }

    /**
     The document's image, in its block.

     @return Its bytes, empty for a holder of no document.
    */
    [[nodiscard]] std::span<const std::byte> image() const noexcept { return document_.image(); }

private:
    /** The only holder of block `index`, just leased, whose image of `size` bytes is `document`. */
    shared_document(std::byte* base, std::size_t index, std::size_t size,
                    basic_document<Traits, external> document) noexcept
        : base_(base), index_(index), document_(std::move(document)) {
        set_up(base, index, size);
    }

    /** A holder of block `index`, whose reference it has taken over, its image `document`. */
    shared_document(std::byte* base, std::size_t index,
                    basic_document<Traits, external> document) noexcept
        : base_(base), index_(index), document_(std::move(document)) {}

    /** Records a block just leased: its image's size, and its one reference. */
    static void set_up(std::byte* base, std::size_t index, std::size_t size) noexcept {
        detail::shared_block_header& block = detail::shared_blocks::block(base, index);
        block.size = size;
        block.count.store(1, std::memory_order_relaxed);
    }

    /** Another document object over this holder's image, attached without a second check. */
    [[nodiscard]] basic_document<Traits, external> attached() const noexcept {
        if (base_ == nullptr) {
            return detail::adopt<Traits, external>(detail::region{});
        }
        return detail::adopt<Traits, external>(
            detail::region::external(detail::shared_blocks::image(base_, index_)));
    }

    /** Drops this holder's reference, if it holds one, and holds none. */
    void drop() noexcept {
        if (base_ == nullptr) {
            return;
        }
        // The document first, which stops reading the block, then the reference.
        document_ = detail::adopt<Traits, external>(detail::region{});
        detail::shared_blocks::drop(base_, index_);
        base_ = nullptr;
        index_ = 0;
    }

    std::byte* base_ = nullptr;
    std::size_t index_ = 0;
    basic_document<Traits, external> document_ = detail::adopt<Traits, external>(detail::region{});
};

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_SHARED_HPP
