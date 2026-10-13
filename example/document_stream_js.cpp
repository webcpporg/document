// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A stream of documents parsed by C++ for a JavaScript program: the program writes the stream's
// bytes in chunks, in any size and cut anywhere, and the stream calls back into JavaScript with
// each document it completes, or with the line it dropped. The program is compiled with
// Emscripten and bound with Embind; document_stream_js.mjs is the JavaScript that sends the
// chunks, and Node runs it under webcpp.drive.

// tag::includes[]
#include <webcpp/document.hpp>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
// end::includes[]

namespace {

namespace wd = webcpp::document;

// tag::stream[]
/**
 A parser of a stream of documents that JavaScript writes to and that calls JavaScript back: the
 callback `on_document` is given each document as the text `stringify` writes, and `on_error` the
 line and the message of an error.

 The framing `lines` is NDJSON, whose bad line is dropped and reported while the stream goes
 on; any other name is a concatenated stream, whose first error ends it.
*/
class json_stream {
public:
    json_stream(const std::string& framing, emscripten::val on_document, emscripten::val on_error)
        : on_document_(std::move(on_document)),
          on_error_(std::move(on_error)),
          parser_(options_of(framing)) {}

    /** Takes a chunk of the stream, and calls back for each document it completes. */
    void write(const std::string& chunk) {
        const wd::result<std::size_t> written =
            parser_.write(chunk, [this](auto made) { deliver(std::move(made)); });
        if (written.has_error()) {
            report(written.error());
        }
    }

    /** Ends the stream, which closes a last line with no newline and a number at the end. */
    void finish() {
        const wd::result<std::size_t> finished =
            parser_.finish([this](auto made) { deliver(std::move(made)); });
        if (finished.has_error()) {
            report(finished.error());
        }
    }

private:
    static wd::stream_options options_of(std::string_view framing) {
        wd::stream_options options;
        if (framing == "lines") {
            options.framing = wd::framing::lines;
            options.on_error = wd::on_error::skip;
        }
        return options;
    }

    /** What the parser gives for each document it completes, or for each line it drops. */
    void deliver(wd::result<wd::document> made) {
        if (made.has_error()) {
            report(made.error());
            return;
        }
        on_document_(made->stringify());
    }

    void report(const boost::system::error_code& error) {
        on_error_(static_cast<unsigned>(parser_.error_line()), error.message());
    }

    emscripten::val on_document_;
    emscripten::val on_error_;
    wd::stream_parser parser_;
};

// end::stream[]

}  // namespace

// tag::bindings[]
EMSCRIPTEN_BINDINGS(document_stream_js) {
    emscripten::class_<json_stream>("JsonStream")
        .constructor<const std::string&, emscripten::val, emscripten::val>()
        .function("write", &json_stream::write)
        .function("finish", &json_stream::finish);
}

// end::bindings[]
