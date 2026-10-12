// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 Images on the wire: a pool of fixed blocks allocated once, a document opened in place in one of
 them that gives its block back when it goes, and a sans-IO reader that cuts a stream of images,
 one after another with nothing between them, into those blocks.

 An image frames itself: its header states its size. The reader takes a stream's first 64 bytes
 into a block, checks them as a header before it takes a byte more, takes the rest of the image
 into the same block, and opens it there: no parsing, no copy beyond the bytes' own, no allocation
 per document. Images are trusted input; untrusted data enters as JSON text.
*/

#ifndef WEBCPP_DOCUMENT_IMAGE_STREAM_HPP
#define WEBCPP_DOCUMENT_IMAGE_STREAM_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/image.hpp>
#include <webcpp/document/traits.hpp>

#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <utility>

namespace webcpp::document {

namespace detail {

/**
 The state of an image pool, at the start of the pool's one allocation, which then holds a slot per
 block and the blocks: which blocks are free, and how many bytes of each its holder may have
 written.

 Every free block is zero: a block given back has its written bytes zeroed, so the next holder finds
 none of the last one's.
*/
class pool_state {
public:
    /** The index of no block. */
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

    /**
     Makes a pool's state and its blocks, zeroed, in one allocation; @ref errc::range for blocks
     that are not a multiple of 16 bytes, smaller than a header, or none, and @ref errc::capacity
     when the allocation fails or its size overflows.
    */
    static result<pool_state*> make(std::size_t block_bytes, std::size_t blocks) noexcept {
        if (block_bytes < header_bytes || block_bytes % 16 != 0 || blocks == 0) {
            return errc::range;
        }
        constexpr std::size_t most = std::numeric_limits<std::size_t>::max();
        if (blocks > (most - 15) / sizeof(slot) || block_bytes > most / blocks) {
            return errc::capacity;
        }
        const std::size_t head = round_up(sizeof(pool_state));
        const std::size_t slots = round_up(blocks * sizeof(slot));
        const std::size_t area = block_bytes * blocks;
        if (slots > most - head || area > most - head - slots) {
            return errc::capacity;
        }
        auto* raw = static_cast<std::byte*>(
            ::operator new(head + slots + area, std::align_val_t{16}, std::nothrow));
        if (raw == nullptr) {
            return errc::capacity;
        }
        // The free blocks are a stack, the first block on top: a pool hands out its blocks in
        // order, and a block given back is the next one handed out.
        auto* first_slot = reinterpret_cast<slot*>(raw + head);
        for (std::size_t i = 0; i < blocks; ++i) {
            new (first_slot + i) slot{.next = i + 1 < blocks ? i + 1 : none, .written = 0};
        }
        std::byte* first_block = raw + head + slots;
        std::memset(first_block, 0, area);
        return new (raw) pool_state(block_bytes, blocks, first_slot, first_block);
    }

    /** Frees the allocation that `state` starts. */
    static void destroy(pool_state* state) noexcept {
        state->~pool_state();
        ::operator delete(static_cast<void*>(state), std::align_val_t{16});
    }

    /** The size of each block. */
    [[nodiscard]] std::size_t block_bytes() const noexcept { return block_bytes_; }

    /** How many blocks. */
    [[nodiscard]] std::size_t blocks() const noexcept { return blocks_; }

    /** How many blocks are free. */
    [[nodiscard]] std::size_t available() const noexcept { return available_; }

    /**
     Takes the free block on top, whose holder may write its first `written` bytes, or returns
     @ref none when every block is leased.
    */
    std::size_t acquire(std::size_t written) noexcept {
        const std::size_t index = free_;
        if (index == none) {
            return none;
        }
        free_ = slots_[index].next;
        slots_[index] = slot{.next = none, .written = written};
        --available_;
        return index;
    }

    /** Zeroes the bytes a block's holder may have written and gives the block back. */
    void release(std::size_t index) noexcept {
        BOOST_ASSERT(index < blocks_);
        std::memset(block(index).data(), 0, slots_[index].written);
        slots_[index] = slot{.next = free_, .written = 0};
        free_ = index;
        ++available_;
    }

    /** Records that a block's holder has written its first `bytes`. */
    void wrote(std::size_t index, std::size_t bytes) noexcept {
        slots_[index].written = std::max(slots_[index].written, bytes);
    }

    /** The link of a leased block, which its holder may use to queue it. */
    std::size_t& next(std::size_t index) noexcept { return slots_[index].next; }

    /** The bytes of a block. */
    [[nodiscard]] std::span<std::byte> block(std::size_t index) const noexcept {
        return {blocks_area_ + (index * block_bytes_), block_bytes_};
    }

private:
    /** A block's link, to the next free block or to its holder's next, and its written bytes. */
    struct slot {
        /** The next block of the list the block is in, or @ref none. */
        std::size_t next;

        /** How many of its first bytes its holder may have written. */
        std::size_t written;
    };

    /** A state over the slots and blocks of its allocation, every block free. */
    pool_state(std::size_t block_bytes, std::size_t blocks, slot* slots,
               std::byte* blocks_area) noexcept
        : block_bytes_(block_bytes),
          blocks_(blocks),
          available_(blocks),
          slots_(slots),
          blocks_area_(blocks_area) {}

    /** `n` rounded up to 16; `n` is at most the largest size less 15. */
    static constexpr std::size_t round_up(std::size_t n) noexcept { return (n + 15) / 16 * 16; }

    std::size_t block_bytes_;
    std::size_t blocks_;
    std::size_t available_;
    std::size_t free_ = 0;
    slot* slots_;
    std::byte* blocks_area_;
};

}  // namespace detail

/**
 A lease of one block of an @ref image_pool; the block is its holder's until the lease is destroyed
 or assigned, and then goes back to the pool, its bytes zeroed.

 A lease is moved, never copied; a moved-from lease holds no block.

 @see "Images", in the guide.
*/
class pool_block {
public:
    /** A lease of no block. */
    pool_block() noexcept = default;

    /**
     Takes the block of `other`, which holds none after it.

     @param other The lease.
    */
    pool_block(pool_block&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          index_(std::exchange(other.index_, detail::pool_state::none)) {}

    /**
     Gives this lease's block back, then takes the block of `other`, which holds none after it.

     @param other The lease.
     @return This lease.
    */
    pool_block& operator=(pool_block&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::exchange(other.state_, nullptr);
            index_ = std::exchange(other.index_, detail::pool_state::none);
        }
        return *this;
    }

    /** A lease is not copied: a block has one holder. */
    pool_block(const pool_block&) = delete;

    /** A lease is not copied: a block has one holder. */
    pool_block& operator=(const pool_block&) = delete;

    /** Gives the block back to its pool, its bytes zeroed. */
    ~pool_block() { release(); }

    /**
     The block's bytes, 16-aligned, the pool's `block_bytes` of them.

     @return The bytes, empty for a lease of no block.
    */
    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        if (state_ == nullptr) {
            return {};
        }
        return state_->block(index_);
    }

private:
    friend class image_pool;
    template <class>
    friend class basic_image_reader;

    /** The lease of the block `index` of a pool. */
    pool_block(detail::pool_state* state, std::size_t index) noexcept
        : state_(state), index_(index) {}

    /** Gives the block back, if the lease holds one, and holds none. */
    void release() noexcept {
        if (state_ != nullptr) {
            state_->release(index_);
        }
        state_ = nullptr;
        index_ = detail::pool_state::none;
    }

    detail::pool_state* state_ = nullptr;
    std::size_t index_ = detail::pool_state::none;
};

/**
 Fixed blocks for images, 16-aligned and zeroed, allocated once: `block_bytes` times `blocks`.

 A block is leased by @ref acquire, or by a @ref basic_image_reader for each image it reads, and
 goes back when its lease does. When every block is leased, the pool says @ref errc::capacity, the
 backpressure of a stream of images, whose reader takes no more bytes until a block is given
 back. The pool must outlive its leases and its readers; moving it moves neither its blocks
 nor its leases.

 @note A pool is not thread-safe: a block is leased and given back, a lease destroyed included,
 on one thread at a time.
 @see "Images", in the guide.
*/
class image_pool {
public:
    /**
     Makes a pool of `blocks` blocks of `block_bytes` bytes each, in one allocation.

     @param block_bytes The size of each block, a multiple of 16 and at least 64, the size of an
     image's header; the largest image the pool holds.
     @param blocks How many blocks, at least one.
     @return The pool; @ref errc::range for a size or a count outside those bounds, and
     @ref errc::capacity when the allocation fails.
    */
    static result<image_pool> create(std::size_t block_bytes, std::size_t blocks) noexcept {
        auto made = detail::pool_state::make(block_bytes, blocks);
        if (!made.has_value()) {
            return made.error();
        }
        return image_pool(made.unsafe_value());
    }

    /**
     Takes the blocks of `other`, which holds none after it; the leases of its blocks stay valid.

     @param other The pool.
    */
    image_pool(image_pool&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

    /**
     Frees this pool's blocks, then takes the blocks of `other`, which holds none after it.

     @param other The pool.
     @return This pool.
    */
    image_pool& operator=(image_pool&& other) noexcept {
        if (this != &other) {
            destroy();
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    /** A pool is not copied. */
    image_pool(const image_pool&) = delete;

    /** A pool is not copied. */
    image_pool& operator=(const image_pool&) = delete;

    /** Frees the blocks, every one of which must have been given back. */
    ~image_pool() { destroy(); }

    /**
     Leases a free block, whose bytes are all zero.

     @return The lease; @ref errc::capacity when every block is leased, or for a moved-from pool.
    */
    result<pool_block> acquire() noexcept {
        if (state_ == nullptr) {
            return errc::capacity;
        }
        // A block leased here may be written anywhere, so all of it is zeroed when it goes back.
        const std::size_t index = state_->acquire(state_->block_bytes());
        if (index == detail::pool_state::none) {
            return errc::capacity;
        }
        return pool_block(state_, index);
    }

    /**
     The size of each block.

     @return The size, 0 for a moved-from pool.
    */
    [[nodiscard]] std::size_t block_bytes() const noexcept {
        return state_ == nullptr ? 0 : state_->block_bytes();
    }

    /**
     How many blocks the pool has.

     @return The count, 0 for a moved-from pool.
    */
    [[nodiscard]] std::size_t blocks() const noexcept {
        return state_ == nullptr ? 0 : state_->blocks();
    }

    /**
     How many blocks are free.

     @return The count, 0 for a moved-from pool.
    */
    [[nodiscard]] std::size_t available() const noexcept {
        return state_ == nullptr ? 0 : state_->available();
    }

private:
    template <class>
    friend class basic_image_reader;

    /** The pool of a state just made. */
    explicit image_pool(detail::pool_state* state) noexcept : state_(state) {}

    /** Frees the allocation, if the pool has one. */
    void destroy() noexcept {
        if (state_ == nullptr) {
            return;
        }
        BOOST_ASSERT(state_->available() == state_->blocks());
        detail::pool_state::destroy(state_);
        state_ = nullptr;
    }

    detail::pool_state* state_ = nullptr;
};

/**
 A document opened in place in a block of an @ref image_pool, holding the block's lease: when the
 document goes, the block goes back to the pool, its bytes zeroed.

 The document is external: it reads and writes as any document does, within its image, and a
 change that needs more room than the image has returns @ref errc::capacity. A leased document is
 moved, never copied; moving it invalidates its handles, and a moved-from one holds no document.

 @tparam Traits The traits the image was written with.
 @see "Images", in the guide.
*/
template <class Traits = default_traits>
class leased_document {
public:
    /**
     The empty lease: no block and no document, the state a moved-from leased document is left in.

     An operation that fails completes with it, as a leased document of an error holds nothing.
    */
    leased_document() noexcept : document_(detail::adopt<Traits, external>(detail::region())) {}

    /**
     Opens the image in a leased block, in place.

     @param block The lease, whose block holds the image from its first byte.
     @return The document; the errors of the function `open` of an external buffer, the block then
     given back.
    */
    static result<leased_document> open(pool_block block) noexcept {
        auto opened = webcpp::document::open<Traits>(block.bytes());
        if (!opened.has_value()) {
            return opened.error();
        }
        return leased_document(std::move(block), std::move(opened.unsafe_value()));
    }

    /**
     Formats a leased block as a new, empty document, holding `null`, which may grow to the whole
     block.

     @param block The lease.
     @return The document; the errors of the function `format` of an external buffer, the block
     then given back.
    */
    static result<leased_document> format(pool_block block) noexcept {
        auto formatted = webcpp::document::format<Traits>(block.bytes());
        if (!formatted.has_value()) {
            return formatted.error();
        }
        return leased_document(std::move(block), std::move(formatted.unsafe_value()));
    }

    /**
     Takes the document and the lease of `other`, which holds neither after it.

     @param other The leased document.
    */
    leased_document(leased_document&& other) noexcept = default;

    /**
     Gives this document's block back, then takes the document and the lease of `other`, which
     holds neither after it.

     @param other The leased document.
     @return This leased document.
    */
    leased_document& operator=(leased_document&& other) noexcept {
        if (this != &other) {
            // The document first, which stops reading its block, then the block.
            document_ = std::move(other.document_);
            block_ = std::move(other.block_);
        }
        return *this;
    }

    /** A leased document is not copied: its block has one holder. */
    leased_document(const leased_document&) = delete;

    /** A leased document is not copied: its block has one holder. */
    leased_document& operator=(const leased_document&) = delete;

    /** Gives the block back, its bytes zeroed. */
    ~leased_document() = default;

    /**
     The document.

     @return The document, valid while this leased document is neither moved nor destroyed.
    */
    basic_document<Traits, external>& get() noexcept { return document_; }

    /**
     The document, read only.

     @return The document, valid while this leased document is neither moved nor destroyed.
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
     Whether the lease holds no document: an empty lease, or one moved from.

     @return Whether it holds no block; its document then reads as nothing.
    */
    [[nodiscard]] bool empty() const noexcept { return block_.bytes().empty(); }

    /**
     The document's image, at the start of its block.

     @return Its bytes, empty for a moved-from leased document.
    */
    [[nodiscard]] std::span<const std::byte> image() const noexcept { return document_.image(); }

    /**
     Seals the image, as the document's own `seal` does: the next change clears it.

     @return Success; @ref errc::type for a moved-from leased document.
    */
    result<void> seal() noexcept { return document_.seal(); }

    /**
     Whether the image carries a seal.

     @return Whether it is sealed; false for a moved-from leased document.
    */
    [[nodiscard]] bool sealed() const noexcept { return document_.sealed(); }

private:
    template <class>
    friend class basic_image_reader;

    /** The document opened in a block, with the block's lease. */
    leased_document(pool_block block, basic_document<Traits, external> document) noexcept
        : block_(std::move(block)), document_(std::move(document)) {}

    // Declared in this order so that the document goes before its block does.
    pool_block block_;
    basic_document<Traits, external> document_;
};

/**
 Reads a stream of images, one after another with nothing between them, into the blocks of an
 @ref image_pool, and opens each in place as a @ref leased_document.

 Sans-IO: the bytes come from any source, copied by @ref write, or read straight into the block by
 the source itself, through @ref prepare and @ref commit. The first 64 bytes of each image are
 checked as its header before a byte more is taken: its magic, format version, byte order, reserved
 bytes, the layout tag of `Traits`, and a size within a block. The rest of the image follows into
 the same block, and the image is opened there, a sealed one checked against its seal, and its
 heap checked. A refusal stops the reader with its error until @ref reset, since the stream has
 lost its framing; the documents completed before it are still delivered.

 The documents completed and not yet taken are queued in their own blocks, so the reader allocates
 nothing. When every block of the pool is leased, the reader takes no more bytes: that is the
 stream's backpressure.

 @tparam Traits The traits of the images; another traits' image is refused.
 @note Images are trusted input: this path is for peers the program trusts, and untrusted data
 enters as JSON text.
 @see "Images", in the guide.
*/
template <class Traits = default_traits>
class basic_image_reader {
public:
    /**
     A reader whose images go into the blocks of `pool`.

     @param pool The pool, which outlives the reader and its documents.
    */
    explicit basic_image_reader(image_pool& pool) noexcept : state_(pool.state_) {}

    /**
     Takes the stream of `other`, its documents and its block, leaving `other` a new reader on the
     same pool.

     @param other The reader.
    */
    basic_image_reader(basic_image_reader&& other) noexcept
        : state_(other.state_),
          current_(std::exchange(other.current_, none)),
          filled_(std::exchange(other.filled_, 0)),
          size_(std::exchange(other.size_, 0)),
          head_(std::exchange(other.head_, none)),
          tail_(std::exchange(other.tail_, none)),
          error_(std::exchange(other.error_, {})),
          offset_(std::exchange(other.offset_, 0)),
          documents_(std::exchange(other.documents_, 0)) {}

    /**
     Gives back this reader's blocks, then takes the stream of `other`, its documents and its
     block, leaving `other` a new reader on the same pool.

     @param other The reader.
     @return This reader.
    */
    basic_image_reader& operator=(basic_image_reader&& other) noexcept {
        if (this != &other) {
            release_all();
            state_ = other.state_;
            current_ = std::exchange(other.current_, none);
            filled_ = std::exchange(other.filled_, 0);
            size_ = std::exchange(other.size_, 0);
            head_ = std::exchange(other.head_, none);
            tail_ = std::exchange(other.tail_, none);
            error_ = std::exchange(other.error_, {});
            offset_ = std::exchange(other.offset_, 0);
            documents_ = std::exchange(other.documents_, 0);
        }
        return *this;
    }

    /** A reader is not copied. */
    basic_image_reader(const basic_image_reader&) = delete;

    /** A reader is not copied. */
    basic_image_reader& operator=(const basic_image_reader&) = delete;

    /** Gives back the blocks of the image being read and of the documents not taken. */
    ~basic_image_reader() { release_all(); }

    /**
     Copies the stream's next bytes into blocks, opening each image they complete.

     @param bytes The next bytes of the stream.
     @return How many bytes were taken: all of them, or fewer when every block of the pool is
     leased, the rest to be written again once a block is given back; the error that stops the
     reader, as @ref commit returns it.
    */
    result<std::size_t> write(std::span<const std::byte> bytes) noexcept {
        return copy_in(bytes, [] {});
    }

    /**
     Copies the stream's next bytes into blocks, and gives `on_document` each document, in order,
     the documents not yet taken first: as soon as it returns, the block of a document it let go
     is free for the next image.

     @tparam F A callable taking a `leased_document<Traits>&&`.
     @param bytes The next bytes of the stream.
     @param on_document The callable.
     @return As @ref write without a callable returns.
    */
    template <class F>
        requires std::invocable<F&, leased_document<Traits>&&>
    result<std::size_t> write(std::span<const std::byte> bytes, F&& on_document) {
        deliver(on_document);
        return copy_in(bytes, [&] { deliver(on_document); });
    }

    /**
     The oldest document completed and not yet taken.

     @return The document, or nothing when none is waiting; once the documents completed before an
     error are taken, the error that stopped the reader.
    */
    result<std::optional<leased_document<Traits>>> next() noexcept {
        if (head_ != none) {
            return std::optional<leased_document<Traits>>(take());
        }
        if (error_.failed()) {
            return error_;
        }
        return std::optional<leased_document<Traits>>();
    }

    /**
     The room the stream's next bytes go into, in a block: the rest of the image's header, or the
     rest of the image it states, so that a read never crosses an image's end.

     The source writes some of the bytes there and gives their count to @ref commit, so that a
     socket reads straight into the block, with no copy.

     @return The room, never empty; @ref errc::capacity when a new image needs a block and every
     block is leased, which changes nothing; the error that stopped the reader.
    */
    result<std::span<std::byte>> prepare() noexcept {
        if (error_.failed()) {
            return error_;
        }
        if (state_ == nullptr) {
            return errc::capacity;
        }
        if (current_ == none) {
            // Its holder writes no byte yet: the reader records each one it commits.
            current_ = state_->acquire(0);
            if (current_ == none) {
                return errc::capacity;
            }
        }
        return state_->block(current_).subspan(filled_, room());
    }

    /**
     Takes `bytes` bytes written at the start of the room @ref prepare gave: the header is checked
     once it is whole, and the image opened once it is.

     @param bytes How many bytes were written, at most the room's size.
     @return How many documents the bytes completed, 0 or 1; @ref errc::range for more bytes than
     the room holds, which changes nothing; the error that stops the reader:
     @ref errc::not_a_document for bytes that are not an image's or an image whose heap is not
     sound, @ref errc::incompatible_document for an image of another format version, byte order,
     reserved bytes that are not zero or traits, @ref errc::damaged_document for a sealed image
     whose bytes do not match its seal, and @ref errc::capacity for an image larger than a block.
    */
    result<std::size_t> commit(std::size_t bytes) noexcept {
        if (error_.failed()) {
            return error_;
        }
        if (bytes == 0) {
            return 0;
        }
        if (current_ == none || bytes > room()) {
            return errc::range;
        }
        filled_ += bytes;
        offset_ += bytes;
        state_->wrote(current_, filled_);
        if (size_ == 0 && filled_ == header_bytes) {
            if (const auto checked = check_header(); !checked.has_value()) {
                return stop(checked.error());
            }
        }
        if (size_ == 0 || filled_ < size_) {
            return 0;
        }
        return complete();
    }

    /**
     Ends the stream: an image cut short is an error, and a stream that ends between two images is
     whole.

     @return Success; @ref errc::not_a_document for an image cut short, which stops the reader; the
     error that stopped it.
    */
    result<void> finish() noexcept {
        if (error_.failed()) {
            return error_;
        }
        if (current_ != none && filled_ == 0) {
            // A block prepare took for an image that never came.
            state_->release(current_);
            current_ = none;
        }
        if (current_ != none) {
            return stop(errc::not_a_document);
        }
        return {};
    }

    /** Makes the reader new: gives back every block it holds, its documents not taken included. */
    void reset() noexcept {
        release_all();
        error_ = {};
        offset_ = 0;
        documents_ = 0;
    }

    /**
     How many bytes of the stream the reader has taken.

     @return The count, since the reader was made or reset.
    */
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

    /**
     How many images the reader has completed and opened.

     @return The count, since the reader was made or reset, the documents not yet taken included.
    */
    [[nodiscard]] std::uint64_t documents() const noexcept { return documents_; }

private:
    static constexpr std::size_t none = detail::pool_state::none;
    static constexpr std::size_t header_bytes = sizeof(image_header);

    /** How many bytes the current image still needs before its header, or its end, is whole. */
    [[nodiscard]] std::size_t room() const noexcept {
        return (size_ == 0 ? header_bytes : size_) - filled_;
    }

    /** Copies `bytes` through @ref prepare and @ref commit, calling `completed` after each image.
     */
    template <class Completed>
    result<std::size_t> copy_in(std::span<const std::byte> bytes, Completed&& completed) {
        if (error_.failed()) {
            return error_;
        }
        std::size_t taken = 0;
        while (taken < bytes.size()) {
            auto prepared = prepare();
            if (!prepared.has_value()) {
                // Every block is leased: the rest waits for one to be given back.
                return taken;
            }
            const std::span<std::byte> to = prepared.unsafe_value();
            const std::size_t n = std::min(to.size(), bytes.size() - taken);
            std::memcpy(to.data(), bytes.data() + taken, n);
            taken += n;
            const auto committed = commit(n);
            if (!committed.has_value()) {
                return committed.error();
            }
            if (committed.unsafe_value() != 0) {
                completed();
            }
        }
        return taken;
    }

    /** Gives `on_document` every document waiting, each block back before the next document. */
    template <class F>
    void deliver(F& on_document) {
        while (head_ != none) {
            on_document(take());
        }
    }

    /** Checks the current block's header and takes the size it states. */
    result<void> check_header() noexcept {
        image_header header{};
        std::memcpy(&header, state_->block(current_).data(), sizeof header);
        if (header.magic != image_magic) {
            return errc::not_a_document;
        }
        // Another version or byte order states its size in another way: it is not read.
        if (header.version != format_version || header.byte_order != byte_order_mark) {
            return errc::incompatible_document;
        }
        if (header.size < header_bytes) {
            return errc::not_a_document;
        }
        if (header.reserved_4 != 0 || header.layout != detail::layout_tag<Traits>()) {
            return errc::incompatible_document;
        }
        if (header.size > state_->block_bytes()) {
            return errc::capacity;
        }
        size_ = static_cast<std::size_t>(header.size);
        return {};
    }

    /** Opens the image the current block holds whole, and queues the block. */
    result<std::size_t> complete() noexcept {
        const std::span<std::byte> image = state_->block(current_).first(size_);
        if (const auto opened = webcpp::document::open<Traits>(image); !opened.has_value()) {
            return stop(opened.error());
        }
        if (tail_ == none) {
            head_ = current_;
        } else {
            state_->next(tail_) = current_;
        }
        tail_ = current_;
        current_ = none;
        filled_ = 0;
        size_ = 0;
        ++documents_;
        return 1;
    }

    /** Takes the oldest document waiting, opened again in its block without a second check. */
    leased_document<Traits> take() noexcept {
        const std::size_t index = head_;
        head_ = std::exchange(state_->next(index), none);
        if (head_ == none) {
            tail_ = none;
        }
        const std::span<std::byte> block = state_->block(index);
        std::uint64_t size = 0;
        std::memcpy(&size, block.data() + offsetof(image_header, size), sizeof size);
        return leased_document<Traits>(pool_block(state_, index),
                                       detail::adopt<Traits, external>(detail::region::external(
                                           block.first(static_cast<std::size_t>(size)))));
    }

    /** Gives the current block back and stops the reader with `error`. */
    boost::system::error_code stop(boost::system::error_code error) noexcept {
        if (current_ != none) {
            state_->release(current_);
        }
        current_ = none;
        filled_ = 0;
        size_ = 0;
        error_ = error;
        return error;
    }

    /** Gives back the current block and every block of a document waiting. */
    void release_all() noexcept {
        if (current_ != none) {
            state_->release(current_);
        }
        current_ = none;
        filled_ = 0;
        size_ = 0;
        while (head_ != none) {
            const std::size_t index = head_;
            head_ = state_->next(index);
            state_->release(index);
        }
        tail_ = none;
    }

    detail::pool_state* state_;
    std::size_t current_ = none;
    std::size_t filled_ = 0;
    std::size_t size_ = 0;
    std::size_t head_ = none;
    std::size_t tail_ = none;
    boost::system::error_code error_;
    std::uint64_t offset_ = 0;
    std::uint64_t documents_ = 0;
};

/** A reader of images of the default traits. */
using image_reader = basic_image_reader<>;

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_IMAGE_STREAM_HPP
