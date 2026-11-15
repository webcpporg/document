// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A document whose memory is a resource of the program's own: parsed, changed and written into a
// buffer the program gave, with the null resource past its end; cloned into a second buffer; and
// moved back across the two, which copies the image into the target's resource once.

#include <webcpp/document.hpp>

#include <cstddef>
#include <iostream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

int main() {
    // tag::resource[]
    // 64 KiB of the program's own. Past their end the null resource refuses, so nothing of the
    // document can reach the global heap.
    std::vector<std::byte> memory(std::size_t{64} * 1024);
    std::pmr::monotonic_buffer_resource arena(memory.data(), memory.size(),
                                              std::pmr::null_memory_resource());
    const std::pmr::polymorphic_allocator<std::byte> alloc(&arena);

    // JSON.parse(text): the image and the parse's working state in the arena.
    wd::result<wd::pmr::document> made = wd::parse(
        R"({"user":"Ada","langs":["Analytical"],"draft":true})", wd::parse_options(), alloc);
    if (made.has_error()) {
        std::cout << "error: " << made.error().message() << '\n';
        return 1;
    }
    wd::pmr::document& doc = *made;

    // doc.user = "Lovelace"; doc.langs.push("Note G"); delete doc.draft; their staging in the
    // arena.
    if (!doc["user"].set("Lovelace") || !doc["langs"].push("Note G") ||
        !doc.root().erase("draft")) {
        std::cout << "error: the arena is full\n";
        return 1;
    }

    // JSON.stringify(doc), into a string of the same arena.
    std::pmr::string text(&arena);
    if (!wd::stringify_to(doc, [&text](std::string_view piece) { text.append(piece); })) {
        return 1;
    }
    std::cout << text << '\n';
    // end::resource[]

    // tag::clone[]
    // structuredClone(doc), into a second buffer of the program's own.
    std::vector<std::byte> other_memory(std::size_t{64} * 1024);
    std::pmr::monotonic_buffer_resource other(other_memory.data(), other_memory.size(),
                                              std::pmr::null_memory_resource());
    wd::result<wd::pmr::document> copy =
        doc.clone(std::pmr::polymorphic_allocator<std::byte>(&other));
    if (copy.has_error() || !(*copy)["user"].set("Byron")) {
        return 1;
    }
    std::cout << std::boolalpha
              << "the copy in the second arena: " << (copy->get_allocator().resource() == &other)
              << '\n';
    // JSON.stringify(copy), piece by piece to the standard output.
    if (!wd::stringify_to(*copy, [](std::string_view piece) { std::cout << piece; })) {
        return 1;
    }
    std::cout << '\n';
    // end::clone[]

    // tag::move[]
    // The arenas differ, so the move assignment copies the copy's image into the first arena, one
    // copy of its bytes, and leaves the copy holding nothing.
    doc = std::move(*copy);
    std::cout << "doc in the first arena: " << (doc.get_allocator().resource() == &arena) << ", "
              << doc.image().size() << " bytes: " << doc["user"].value_or(std::string_view())
              << '\n';
    std::cout << "the copy: " << copy->image().size() << " bytes\n";
    // end::move[]
}
