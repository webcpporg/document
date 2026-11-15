// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The instruments of the allocation tests: the calls of the global operator new that
// counting.cpp's replacements count, what a standard container allocates for itself, a memory
// resource that counts what it hands out and takes back, and an arena, a monotonic resource over
// memory taken before anything is counted, whose upstream is the null resource, so that an
// allocation past its end fails rather than reach the global heap. A program that includes this
// header links counting.cpp.

#ifndef WEBCPP_TEST_DOCUMENT_COUNTING_HPP
#define WEBCPP_TEST_DOCUMENT_COUNTING_HPP

#include <boost/interprocess/exceptions.hpp>
#include <boost/throw_exception.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <vector>

namespace webcpp::test {

/** The calls of the global operator new, of every form, the program has made so far. */
std::uint64_t global_news() noexcept;

/** The calls of the global operator delete, of every form, the program has made so far. */
std::uint64_t global_deletes() noexcept;

/** The calls of the global operator new made since it was built: what a block of code made. */
class global_count {
public:
    /** Counts from now. */
    global_count() noexcept : news_(global_news()), deletes_(global_deletes()) {}

    /** The calls of operator new since it was built. */
    [[nodiscard]] std::uint64_t news() const noexcept { return global_news() - news_; }

    /** The calls of operator delete since it was built. */
    [[nodiscard]] std::uint64_t deletes() const noexcept { return global_deletes() - deletes_; }

private:
    std::uint64_t news_;
    std::uint64_t deletes_;
};

/**
 What a standard container allocates for itself through its allocator, rebound, whatever it holds:
 the MSVC STL's checked iterators (`_ITERATOR_DEBUG_LEVEL` above 0, the default of a debug build)
 allocate a proxy for each container when it is constructed and when it is moved, in its noexcept
 constructors as well, where an allocator's failure ends the program; the other standard libraries
 allocate nothing so. Measured once, on an empty `std::pmr::vector`.
*/
struct container_bookkeeping {
    /** The blocks one container allocates for itself: none, or its proxy. */
    std::uint64_t blocks = 0;

    /** The size of such a block; 0 when there is none. */
    std::size_t bytes = 0;

    /** The alignment of such a block; 0 when there is none. */
    std::size_t alignment = 0;
};

/** What a standard container allocates for itself on this standard library. */
const container_bookkeeping& bookkeeping();

/**
 Whether a block of `bytes` at `alignment` may be a standard container's own, as @ref bookkeeping
 measures it; never on a standard library that allocates none. Where one does, a resource cannot
 tell such a block from one of the document's of the same size and alignment, as the two 64-bit
 words a change's plan may reserve: the resources here neither count nor refuse either. No operation
 whose allocations a test counts exactly makes such a block of its own (keys, texts and pieces are
 of `char`, stacks of 16 frames and more, images 16-aligned), and a failure at one that other
 operations make is proven on the other standard libraries.
*/
bool is_bookkeeping(std::size_t bytes, std::size_t alignment);

/** A memory resource that counts what it hands out and takes back, from `upstream`. */
class counting_resource final : public std::pmr::memory_resource {
public:
    /** A resource over `upstream`, which outlives it. */
    explicit counting_resource(std::pmr::memory_resource* upstream) noexcept
        : upstream_(upstream) {}

    /** The allocations it made, a standard container's own (@ref is_bookkeeping) left out. */
    [[nodiscard]] std::uint64_t allocations() const noexcept { return allocations_; }

    /** The deallocations it made, a standard container's own left out. */
    [[nodiscard]] std::uint64_t deallocations() const noexcept { return deallocations_; }

    /**
     Every allocation it made, a standard container's own among them: what an operation that is
     to take nothing at all of a monotonic resource, which never reuses a block, must leave as it
     was.
    */
    [[nodiscard]] std::uint64_t all_allocations() const noexcept { return all_allocations_; }

    /**
     The bytes handed out and not yet taken back, a standard container's own among them: all of
     it comes back.
    */
    [[nodiscard]] std::uint64_t bytes_in_use() const noexcept { return in_use_; }

    /** The largest alignment asked of it. */
    [[nodiscard]] std::size_t largest_alignment() const noexcept { return largest_alignment_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* memory = upstream_->allocate(bytes, alignment);
        if (!is_bookkeeping(bytes, alignment)) {
            ++allocations_;
        }
        ++all_allocations_;
        in_use_ += bytes;
        largest_alignment_ = alignment > largest_alignment_ ? alignment : largest_alignment_;
        return memory;
    }

    void do_deallocate(void* memory, std::size_t bytes, std::size_t alignment) override {
        if (!is_bookkeeping(bytes, alignment)) {
            ++deallocations_;
        }
        in_use_ -= bytes;
        upstream_->deallocate(memory, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::uint64_t allocations_ = 0;
    std::uint64_t deallocations_ = 0;
    std::uint64_t all_allocations_ = 0;
    std::uint64_t in_use_ = 0;
    std::size_t largest_alignment_ = 0;
};

/**
 A memory resource that hands out what `upstream` gives for its first `budget` allocations and fails
 every later one with `Failure`, through `boost::throw_exception`: an allocator that runs out at a
 chosen step of an operation, for tests built with exceptions.

 A standard container's own block (@ref is_bookkeeping) is handed out whatever the budget, and
 counts for none of it: the standard library allocates it in noexcept constructors, where a refusal
 ends the program, so that no operation could make that refusal errc::capacity.

 @tparam Failure What a refusal throws: `std::bad_alloc`, or Boost.Interprocess's `bad_alloc`, which
 an allocator of Boost.Interprocess throws and which derives from `std::exception` alone.
*/
template <class Failure>
class basic_failing_resource final : public std::pmr::memory_resource {
public:
    /** A resource over `upstream`, which outlives it, that allows `budget` allocations. */
    basic_failing_resource(std::pmr::memory_resource* upstream, std::uint64_t budget) noexcept
        : upstream_(upstream), budget_(budget) {}

    /** Allows `budget` more allocations from now. */
    void allow(std::uint64_t budget) noexcept { budget_ = budget; }

    /** The allocations it refused. */
    [[nodiscard]] std::uint64_t refusals() const noexcept { return refusals_; }

    /** The bytes handed out and not yet taken back. */
    [[nodiscard]] std::uint64_t bytes_in_use() const noexcept { return in_use_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!is_bookkeeping(bytes, alignment)) {
            if (budget_ == 0) {
                ++refusals_;
                boost::throw_exception(Failure());
            }
            --budget_;
        }
        void* memory = upstream_->allocate(bytes, alignment);
        in_use_ += bytes;
        return memory;
    }

    void do_deallocate(void* memory, std::size_t bytes, std::size_t alignment) override {
        in_use_ -= bytes;
        upstream_->deallocate(memory, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::uint64_t budget_;
    std::uint64_t refusals_ = 0;
    std::uint64_t in_use_ = 0;
};

/** A resource that fails with `std::bad_alloc`, as every standard allocator does. */
using failing_resource = basic_failing_resource<std::bad_alloc>;

/** A resource that fails with Boost.Interprocess's `bad_alloc`, as its allocators do. */
using interprocess_failing_resource = basic_failing_resource<boost::interprocess::bad_alloc>;

/**
 A monotonic resource over `bytes` bytes taken when it is built, whose upstream is the null
 resource, counted: what passes its end fails with `std::bad_alloc`, and nothing reaches the
 global heap once it is built.
*/
class arena {
public:
    /** An arena of `bytes` bytes. */
    explicit arena(std::size_t bytes)
        : buffer_(bytes),
          monotonic_(buffer_.data(), buffer_.size(), std::pmr::null_memory_resource()),
          counted_(&monotonic_) {}

    /** An arena is not copied: its resources refer to its buffer. */
    arena(const arena&) = delete;

    /** An arena is not copied: its resources refer to its buffer. */
    arena& operator=(const arena&) = delete;

    /** An arena is not moved: its resources refer to its buffer. */
    arena(arena&&) = delete;

    /** An arena is not moved: its resources refer to its buffer. */
    arena& operator=(arena&&) = delete;

    /** Gives its buffer back. */
    ~arena() = default;

    /** The counted resource, which allocates in the buffer. */
    [[nodiscard]] counting_resource& resource() noexcept { return counted_; }

    /** A polymorphic allocator of the counted resource. */
    [[nodiscard]] std::pmr::polymorphic_allocator<std::byte> allocator() noexcept {
        return {&counted_};
    }

private:
    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource monotonic_;
    counting_resource counted_;
};

}  // namespace webcpp::test

#endif  // WEBCPP_TEST_DOCUMENT_COUNTING_HPP
