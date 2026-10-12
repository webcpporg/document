// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// An NDJSON stream read inside an actor system: a parser actor takes the bytes in chunks cut inside
// strings and numbers, sends each document it completes to a consumer actor, which prints it, and
// reports a line that is not JSON and goes on. The host delivers one chunk at a time and runs the
// actors on xactor's fifo driver, so the output is the same on every run.

// tag::includes[]
#include <webcpp/document.hpp>
#include <webcpp/xactor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

namespace wd = webcpp::document;
namespace xactor = webcpp::xactor;
// end::includes[]

namespace {

// tag::messages[]
/** The bytes of the stream, as the host sends them. */
struct chunk {
    std::string text;
};

/** The end of the stream, which the parser closes. */
struct end_of_stream {};

/** A document the parser completed, moved to the consumer: its image stays where it is. */
struct parsed {
    wd::document value;
};

/** A line the parser dropped, and the line it was on. */
struct rejected {
    std::string reason;
    std::uint64_t line = 0;
};

using message = std::variant<chunk, end_of_stream, parsed, rejected>;
// end::messages[]

/** NDJSON, with a bad line dropped and reported: the stream goes on after it. */
wd::stream_options lines_skipping() {
    wd::stream_options options;
    options.framing = wd::framing::lines;
    options.on_error = wd::on_error::skip;
    return options;
}

// tag::parser[]
/** Parses the stream, and sends each document, or each line it drops, to the consumer. */
class parser final : public xactor::actor_logic<message> {
public:
    explicit parser(xactor::actor_ref consumer) : consumer_(consumer), stream_(lines_skipping()) {}

    /** The callback the parser hands each document to, and each dropped line's error. */
    auto hand(xactor::turn<message>& turn) {
        return
            [this, &turn](wd::result<wd::document>&& made) { return relay(turn, std::move(made)); };
    }

    xactor::result<void> handle(xactor::turn<message>& turn,
                                const xactor::envelope<message>& cause) override {
        if (const auto* bytes = std::get_if<chunk>(&cause.payload)) {
            const xactor::result<std::size_t> written = stream_.write(bytes->text, hand(turn));
            return as_void(written);
        }
        if (std::holds_alternative<end_of_stream>(cause.payload)) {
            const xactor::result<std::size_t> finished = stream_.finish(hand(turn));
            return as_void(finished);
        }
        return {};
    }

private:
    /** Sends a completed document, or the reason a line was dropped, to the consumer. */
    xactor::result<void> relay(xactor::turn<message>& turn, wd::result<wd::document>&& made) {
        if (made.has_value()) {
            return turn.send(consumer_, parsed{.value = std::move(*made)});
        }
        return turn.send(consumer_,
                         rejected{.reason = made.error().message(), .line = stream_.error_line()});
    }

    /** Keeps the failure of a call of the parser; the documents it completed went on already. */
    static xactor::result<void> as_void(const xactor::result<std::size_t>& counted) {
        if (!counted.has_value()) {
            return {boost::system::in_place_error, counted.error()};
        }
        return {};
    }

    xactor::actor_ref consumer_;
    wd::stream_parser stream_;
};

// end::parser[]

// tag::consumer[]
/** Prints each document with stringify, and each line the parser dropped. */
class consumer final : public xactor::actor_logic<message> {
public:
    xactor::result<void> handle([[maybe_unused]] xactor::turn<message>& turn,
                                const xactor::envelope<message>& cause) override {
        if (const auto* got = std::get_if<parsed>(&cause.payload)) {
            std::cout << got->value.stringify() << '\n';
        }
        if (const auto* dropped = std::get_if<rejected>(&cause.payload)) {
            std::cout << "line " << dropped->line << " dropped: " << dropped->reason << '\n';
        }
        return {};
    }
};

// end::consumer[]

}  // namespace

int main() {
    // tag::host[]
    xactor::scheduler<message> system(xactor::budgets{.fuel = 100});
    const xactor::result<xactor::actor_ref> printing = xactor::create_actor<consumer>(system);
    if (!printing.has_value()) {
        return 1;
    }
    const xactor::result<xactor::actor_ref> reading =
        xactor::create_actor<parser>(system, *printing);
    if (!reading.has_value()) {
        return 1;
    }

    // Two documents and a line that is not JSON, in chunks cut inside a string, a number and a
    // line; a third document with an exponent, and a last one with no newline after it.
    const std::array<std::string_view, 7> pieces{
        R"({"name":"Ad)",
        R"(a","born":181)",
        "5}\n{\"name\":\"Gr",
        R"(ace","langs":["COB)",
        "OL\"]}\nnot j",
        "son\n{\"name\":\"Alan\",\"n\":1.5",
        "e3}\n{\"name\":\"Edsger\"}",
    };

    xactor::fifo_driver<message> driver(system);
    std::uint64_t delivery = 0;
    for (const std::string_view piece : pieces) {
        const xactor::correlation_id execution{.value = ++delivery};
        if (!system.deliver(*reading, chunk{.text = std::string(piece)}, execution).has_value()) {
            return 1;
        }
        if (!driver.run_until_idle(nullptr).has_value()) {
            return 1;
        }
    }
    const xactor::correlation_id execution{.value = ++delivery};
    if (!system.deliver(*reading, end_of_stream{}, execution).has_value()) {
        return 1;
    }
    if (!driver.run_until_idle(nullptr).has_value()) {
        return 1;
    }
    // end::host[]
    return 0;
}
