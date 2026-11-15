// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The replacements of the global operator new and operator delete that counting.hpp's counts read,
// for the program that links this file alone: every form a program calls directly, the array and
// the nothrow forms included, each counted once, however the standard library implements one form
// through another; and the measure of what a standard container allocates for itself.

#include "counting.hpp"

#include <boost/config.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <vector>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace {

// What the replacements below counted. They are mutable global state, which no library holds:
// this program's own instrument, the one place where the replaced operator new can record what it
// sees.
std::uint64_t news = 0;     // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::uint64_t deletes = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Memory of `size` bytes, or null.
void* plain_memory(std::size_t size) noexcept {
    ++news;
    return std::malloc(size == 0 ? 1 : size);
}

// Memory of `size` bytes aligned to `alignment`, or null; the UCRT of MSVC has no aligned_alloc,
// and memory from its _aligned_malloc is given back with _aligned_free. The aligned operator new
// takes any power of two, as an allocator of char asks 1; aligned_alloc may refuse one below the
// fundamental alignment, as macOS's does, so that one is asked at least that.
void* aligned_memory(std::size_t size, std::align_val_t alignment) noexcept {
    ++news;
    const std::size_t align =
        std::max(static_cast<std::size_t>(alignment), alignof(std::max_align_t));
    const std::size_t rounded = size == 0 ? align : (size + align - 1) / align * align;
#ifdef _MSC_VER
    return _aligned_malloc(rounded, align);
#else
    return std::aligned_alloc(align, rounded);
#endif
}

// Gives back what plain_memory took.
void free_plain(void* memory) noexcept {
    if (memory != nullptr) {
        ++deletes;
    }
    std::free(memory);
}

// Gives back what aligned_memory took.
void free_aligned(void* memory) noexcept {
    if (memory != nullptr) {
        ++deletes;
    }
#ifdef _MSC_VER
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

// A failure of a form that throws: a test built without exceptions has no std::bad_alloc to throw,
// and none of these tests runs out of the global heap.
void* or_abort(void* memory) noexcept {
    if (memory == nullptr) {
        std::abort();
    }
    return memory;
}

// A resource that records the blocks it is asked for, with the size and the alignment of the last,
// and takes them from `upstream`.
class recording_resource final : public std::pmr::memory_resource {
public:
    explicit recording_resource(std::pmr::memory_resource* upstream) noexcept
        : upstream_(upstream) {}

    [[nodiscard]] webcpp::test::container_bookkeeping recorded() const noexcept {
        return recorded_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++recorded_.blocks;
        recorded_.bytes = bytes;
        recorded_.alignment = alignment;
        return upstream_->allocate(bytes, alignment);
    }

    void do_deallocate(void* memory, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(memory, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    webcpp::test::container_bookkeeping recorded_;
};

// What an empty std::pmr::vector allocates when it is made, in a buffer on the stack, so that the
// measure reaches no heap and no resource a test counts.
webcpp::test::container_bookkeeping measure() {
    alignas(std::max_align_t) std::array<std::byte, 256> room{};
    std::pmr::monotonic_buffer_resource buffer(room.data(), room.size(),
                                               std::pmr::null_memory_resource());
    recording_resource recorder(&buffer);
    {
        const std::pmr::vector<std::byte> empty(&recorder);
    }
    return recorder.recorded();
}

}  // namespace

namespace webcpp::test {

std::uint64_t global_news() noexcept {
    return news;
}

std::uint64_t global_deletes() noexcept {
    return deletes;
}

const container_bookkeeping& bookkeeping() {
    static const container_bookkeeping measured = measure();
    return measured;
}

bool is_bookkeeping(std::size_t bytes, std::size_t alignment) {
    const container_bookkeeping& b = bookkeeping();
    return b.blocks != 0 && bytes == b.bytes && alignment == b.alignment;
}

}  // namespace webcpp::test

// The standard library declares these with parameter names reserved to the implementation, hence
// the exemption. None is ever inlined: GCC, seeing the malloc or the free inside one, would pair it
// with the other's call and warn of a mismatch.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)

BOOST_NOINLINE void* operator new(std::size_t size) {
    return or_abort(plain_memory(size));
}

BOOST_NOINLINE void* operator new[](std::size_t size) {
    return or_abort(plain_memory(size));
}

BOOST_NOINLINE void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return plain_memory(size);
}

BOOST_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return plain_memory(size);
}

BOOST_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment) {
    return or_abort(aligned_memory(size, alignment));
}

BOOST_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment) {
    return or_abort(aligned_memory(size, alignment));
}

BOOST_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment,
                                  const std::nothrow_t& /*tag*/) noexcept {
    return aligned_memory(size, alignment);
}

BOOST_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment,
                                    const std::nothrow_t& /*tag*/) noexcept {
    return aligned_memory(size, alignment);
}

BOOST_NOINLINE void operator delete(void* memory) noexcept {
    free_plain(memory);
}

BOOST_NOINLINE void operator delete[](void* memory) noexcept {
    free_plain(memory);
}

BOOST_NOINLINE void operator delete(void* memory, std::size_t /*size*/) noexcept {
    free_plain(memory);
}

BOOST_NOINLINE void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    free_plain(memory);
}

BOOST_NOINLINE void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept {
    free_plain(memory);
}

BOOST_NOINLINE void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept {
    free_plain(memory);
}

BOOST_NOINLINE void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}

BOOST_NOINLINE void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}

BOOST_NOINLINE void operator delete(void* memory, std::size_t /*size*/,
                                    std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}

BOOST_NOINLINE void operator delete[](void* memory, std::size_t /*size*/,
                                      std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}

BOOST_NOINLINE void operator delete(void* memory, std::align_val_t /*alignment*/,
                                    const std::nothrow_t& /*tag*/) noexcept {
    free_aligned(memory);
}

BOOST_NOINLINE void operator delete[](void* memory, std::align_val_t /*alignment*/,
                                      const std::nothrow_t& /*tag*/) noexcept {
    free_aligned(memory);
}

// NOLINTEND(readability-inconsistent-declaration-parameter-name)
