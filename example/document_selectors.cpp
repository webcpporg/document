// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Selectors: values delivered while the text streams, taken from the document by on or kept by
// observe; a value inside another delivered first; the root and a scalar at the top; a callback
// that fails the parse; and a long array streamed in the memory of one element.

#include <webcpp/document.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

// tag::print[]
/** A callback that prints what it receives and where. */
constexpr auto print = [](const wd::const_value& v, wd::path_view where) {
    std::cout << "  " << where.to_string() << ": " << v.stringify() << '\n';
};
// end::print[]

/** Prints the document a finished parser gives, or its error. */
void finished(wd::parser& p) {
    const wd::result<wd::document> made = p.finish();
    if (made.has_error()) {
        std::cout << "finish: " << made.error().message() << '\n';
        return;
    }
    std::cout << "finish: " << (made->root().is_undefined() ? "undefined" : made->stringify())
              << '\n';
}

}  // namespace

int main() {
    // tag::on[]
    constexpr std::string_view text =
        R"({"meta":{"page":1},"items":[{"id":"a","qty":2},{"id":"b","qty":3}],"done":true})";
    wd::parser p;
    std::int64_t total = 0;
    const wd::result<void> taken =
        p.on("$.items[*]", [&total](const wd::const_value& item, wd::path_view where) {
            print(item, where);
            total += item["qty"].value_or(std::int64_t{0});
        });
    const wd::result<void> kept = p.observe("$.meta", print);
    if (taken.has_error() || kept.has_error() || p.write(text).has_error()) {
        return 1;
    }
    std::cout << "total " << total << '\n';
    finished(p);
    // end::on[]

    // tag::nested[]
    wd::parser rows;
    if (rows.on("$.rows[*][1]", print).has_error() ||
        rows.observe("$.rows[*]", print).has_error() ||
        rows.write(R"({"rows":[[1,2,3],[4,5,6]]})").has_error()) {
        return 1;
    }
    finished(rows);
    // end::nested[]

    // tag::root[]
    wd::parser whole;
    if (whole.on("$", print).has_error() || whole.write("[1,2]").has_error()) {
        return 1;
    }
    finished(whole);  // the root was taken
    whole.reset();    // the selectors stay
    if (whole.write("12").has_error()) {
        return 1;
    }
    std::cout << "12 written\n";  // a number at the top ends only with the text
    finished(whole);
    // end::root[]

    // tag::failing[]
    wd::parser strict;
    const wd::result<void> subscribed =
        strict.on("$[*]", [](const wd::const_value& v, wd::path_view) -> wd::result<void> {
            if (!v.is_number()) {
                return wd::errc::type;
            }
            return {};
        });
    if (subscribed.has_error()) {
        return 1;
    }
    std::cout << "write: " << strict.write(R"([1,"two",3])").error().message() << '\n';
    finished(strict);  // a failed parser stays failed until reset
    // end::failing[]

    // tag::bounded[]
    wd::parser stream;
    std::uint64_t count = 0;
    if (stream.on("$[*]", [&count](const wd::const_value&, wd::path_view) { ++count; })
            .has_error() ||
        stream.write("[").has_error()) {
        return 1;
    }
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        const std::string element =
            std::string(i == 0 ? "" : ",") + R"({"id":)" + std::to_string(i) + "}";
        if (stream.write(element).has_error()) {
            return 1;
        }
    }
    if (stream.write("]").has_error()) {
        return 1;
    }
    const wd::result<wd::document> streamed = stream.finish();
    if (streamed.has_error()) {
        return 1;
    }
    std::cout << count << " delivered, " << streamed->stringify()
              << " left, image under 64 KiB: " << std::boolalpha
              << (streamed->image().size() < 65'536) << '\n';
    // end::bounded[]
}
