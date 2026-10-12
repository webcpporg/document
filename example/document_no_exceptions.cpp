// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The core in a build without exceptions, which example/Jamfile asks whatever the build asks: the
// sugar that throws is not there, and every failure is a value, one of each errc printed with its
// category and message.

#include <webcpp/document.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace wd = webcpp::document;

namespace {

// tag::absent[]
/** Whether a handle of type `H` has the sugar `as<T>()`, which throws. */
template <class H>
concept has_as = requires(const H& h) { h.template as<std::int64_t>(); };

/** Whether a handle of type `H` can be assigned a value, `h = 1`, which throws on failure. */
template <class H>
concept assignable = requires(H h) { h = 1; };

static_assert(!has_as<wd::const_value>, "as<T>() needs exceptions");
static_assert(!assignable<wd::value>, "h = x needs exceptions");
static_assert(!std::is_default_constructible_v<wd::document>, "wd::document d; needs exceptions");
static_assert(!std::is_copy_constructible_v<wd::document>, "copying a document needs exceptions");
// end::absent[]

// tag::print[]
/** Prints what failed, its code's value and category, and its message. */
void print(std::string_view what, boost::system::error_code e) {
    std::cout << what << ": " << e.category().name() << ':' << e.value() << ", " << e.message()
              << '\n';
}

// end::print[]

}  // namespace

int main() {
    // tag::errors[]
    // errc::syntax: malformed text, and the parser's offset where it was found.
    wd::parser p;
    const wd::result<void> written = p.write("[1,]");
    print("[1,]", written.error());
    std::cout << "  at byte " << p.offset() << ", Boost.JSON's cause: " << p.cause().message()
              << '\n';

    wd::result<wd::document> made = wd::parse(R"({"n":300})");
    if (made.has_error()) {
        return 1;
    }
    wd::document& doc = *made;
    // errc::type: a write through nothing, or a read of another kind.
    print("doc.n.x.y = 1", doc["n"]["x"]["y"].set(1).error());
    print("doc.n as a string", doc["n"].get<std::string>().error());
    // errc::range: a number that does not fit, or a date out of JavaScript's range.
    print("doc.n as int8_t", doc["n"].get<std::int8_t>().error());
    // errc::capacity: a limit, here of depth.
    wd::parse_options shallow;
    shallow.max_depth = 1;
    print("[[1]], max_depth 1", wd::parse("[[1]]", shallow).error());
    // errc::not_a_document and errc::incompatible_document: bytes that are no image of these
    // traits.
    print("64 zero bytes", wd::from_image(std::array<std::byte, 64>{}).error());
    print("another traits' image", wd::from_image<wd::js_traits>(doc.image()).error());
    // end::errors[]

    // tag::compare[]
    // An error compares with an errc, and the document is as it was before the call that failed.
    if (const wd::result<void> set = doc["n"]["x"].set(1); set.error() == wd::errc::type) {
        std::cout << "still " << doc.stringify() << '\n';
    }
    // end::compare[]
}
