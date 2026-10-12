// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The Emscripten side of webcpp-document-shared.mjs: the documents of a shared_pool that every
// worker joined to the module's memory reads in place. It uses the helpers of js/binding.hpp and
// exports native_shared and native_space to JavaScript, as NativeShared and NativeSpace (and the
// outcome they answer with), and none of the classes of the single-threaded module: every document
// of this module is in the pool. The thread that starts the module lays out the memory every thread
// shares (native_space::create); each thread then reaches it through a native_space of its own,
// made from the addresses of that layout, and nothing here is a global.

#include "../binding.hpp"

#include <webcpp/document.hpp>
#include <webcpp/document/shared.hpp>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <emscripten/wasm_worker.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace wd = webcpp::document;
using wd::binding::decimal_outcome;
using wd::binding::failed;
using wd::binding::length_outcome;
using wd::binding::outcome;
using wd::binding::outcome_of;
using wd::binding::read_outcome;
using wd::binding::steps_of;
using wd::binding::succeeded;
using wd::binding::val;
using wd::binding::view_of;
using wd::binding::write_outcome;
using shared_document = wd::shared_document<wd::default_traits>;

/** A reference that share() gave up and adopt() has not taken over: the token JavaScript holds. */
struct ticket {
    std::size_t index = 0;
    std::uint32_t generation = 0;
    std::uint32_t offset = 0;

    [[nodiscard]] std::uint64_t key() const {
        return (static_cast<std::uint64_t>(generation) << 32U) | offset;
    }
};

/**
 The tickets of the references in flight between threads: a generation counter, then one slot per
 ticket holding its key (its generation in the high 32 bits, its image's offset in the low ones), or
 0 when free. A ticket is redeemed once, by the thread that wins its slot's exchange; a spent or
 stale one finds another key there and is refused.

 Tip: an offset alone names a block, not a document: once its document is gone, the pool hands the
 block to the next one, which an old offset would then adopt. The generation tells them apart.
*/
class tickets {
public:
    static std::size_t bytes(std::size_t count) {
        return (count + 1) * sizeof(std::atomic<std::uint64_t>);
    }

    /** Lays out `count` free tickets at `base`. */
    static void format(std::byte* base, std::size_t count) {
        for (std::size_t i = 0; i <= count; ++i) {
            new (base + (i * sizeof(std::atomic<std::uint64_t>))) std::atomic<std::uint64_t>(0);
        }
    }

    tickets(std::byte* base, std::size_t count) : base_(base), count_(count) {}

    /** A ticket for the reference to the image at `offset`; none when every ticket is out. */
    [[nodiscard]] std::optional<ticket> issue(std::uint32_t offset) const {
        if (count_ == 0) {
            return std::nullopt;
        }
        std::uint32_t generation = 0;
        while (generation == 0) {
            generation =
                static_cast<std::uint32_t>(slot(0).fetch_add(1, std::memory_order_relaxed));
        }
        for (std::size_t i = 0; i < count_; ++i) {
            const ticket issued{
                .index = (generation + i) % count_,
                .generation = generation,
                .offset = offset,
            };
            std::uint64_t free = 0;
            // Release: whoever redeems the ticket sees the reference counted before it.
            if (slot(issued.index + 1)
                    .compare_exchange_strong(free, issued.key(), std::memory_order_release,
                                             std::memory_order_relaxed)) {
                return issued;
            }
        }
        return std::nullopt;
    }

    /** Takes the ticket's reference over: false for a ticket spent, stale or never issued. */
    [[nodiscard]] bool redeem(const ticket& held) const {
        if (held.index >= count_ || held.generation == 0) {
            return false;
        }
        std::uint64_t expected = held.key();
        return slot(held.index + 1)
            .compare_exchange_strong(expected, 0, std::memory_order_acquire,
                                     std::memory_order_relaxed);
    }

private:
    [[nodiscard]] std::atomic<std::uint64_t>& slot(std::size_t index) const {
        return *std::launder(reinterpret_cast<std::atomic<std::uint64_t>*>(
            base_ + (index * sizeof(std::atomic<std::uint64_t>))));
    }

    std::byte* base_;
    std::size_t count_;
};

/** The ticket a JavaScript token names, checked against what a ticket can hold. */
std::optional<ticket> ticket_of(const val& token) {
    constexpr double most = 4294967295.0;
    if (token.isNull() || token.typeOf().as<std::string>() != "object") {
        return std::nullopt;
    }
    const val index = token["ticket"];
    const val generation = token["generation"];
    const val offset = token["offset"];
    if (!index.isNumber() || !generation.isNumber() || !offset.isNumber()) {
        return std::nullopt;
    }
    const auto number = [](const val& field) { return field.as<double>(); };
    if (number(index) < 0 || number(index) > most || number(generation) < 1 ||
        number(generation) > most || number(offset) < 0 || number(offset) > most) {
        return std::nullopt;
    }
    return ticket{
        .index = static_cast<std::size_t>(number(index)),
        .generation = static_cast<std::uint32_t>(number(generation)),
        .offset = static_cast<std::uint32_t>(number(offset)),
    };
}

val token_of(const ticket& issued) {
    val token = val::object();
    token.set("offset", val(static_cast<double>(issued.offset)));
    token.set("ticket", val(static_cast<double>(issued.index)));
    token.set("generation", val(static_cast<double>(issued.generation)));
    return token;
}

/** One reference to a document of the pool, which this object holds until JavaScript deletes it. */
class native_shared {
public:
    explicit native_shared(boost::system::error_code error)
        : code_(error.value()), message_(error.message()) {}

    explicit native_shared(wd::errc error) : native_shared(wd::make_error_code(error)) {}

    explicit native_shared(shared_document document) : document_(std::move(document)) {}

    native_shared(const native_shared&) = delete;
    native_shared& operator=(const native_shared&) = delete;

    [[nodiscard]] int code() const { return code_; }

    [[nodiscard]] std::string message() const { return message_; }

    [[nodiscard]] bool held() const { return document_.use_count() != 0; }

    [[nodiscard]] const shared_document& document() const { return document_; }

    [[nodiscard]] double use_count() const { return static_cast<double>(document_.use_count()); }

    [[nodiscard]] double byte_length() const {
        return static_cast<double>(document_.image().size());
    }

    [[nodiscard]] val image() const { return view_of(document_.image()); }

    [[nodiscard]] outcome get(const val& path) const {
        if (!held()) {
            return failed(wd::errc::type);
        }
        return read_outcome(document_.root(), steps_of(path));
    }

    [[nodiscard]] outcome get_decimal(const val& path) const {
        if (!held()) {
            return failed(wd::errc::type);
        }
        return decimal_outcome(document_.root(), steps_of(path));
    }

    [[nodiscard]] outcome length(const val& path) const {
        if (!held()) {
            return failed(wd::errc::type);
        }
        return length_outcome(document_.root(), steps_of(path));
    }

    [[nodiscard]] std::string stringify(std::uint32_t indent) const {
        return held() ? document_.get().stringify(indent) : std::string();
    }

    // Writes in place, where every holder reads: make_mutable first for a copy of one's own.
    outcome set(const val& path, const val& json) {
        if (!held()) {
            return failed(wd::errc::type);
        }
        return write_outcome(document_.root(), steps_of(path), json);
    }

    outcome make_mutable() { return outcome_of(document_.make_mutable()); }

private:
    int code_ = 0;
    std::string message_;
    shared_document document_;
};

/**
 This thread's view of the memory every thread shares: the pool, attached at its address, and the
 tickets. The layout itself is made once, by create(), on the thread that starts the module.
*/
class native_space {
public:
    native_space(std::uintptr_t pool, std::size_t pool_bytes, std::uintptr_t tickets_address,
                 std::size_t ticket_count)
        : tickets_(wd::binding::memory_at(tickets_address), ticket_count) {
        auto attached = wd::shared_pool::attach({wd::binding::memory_at(pool), pool_bytes});
        if (attached.has_value()) {
            pool_ = attached.unsafe_value();
        } else {
            code_ = attached.error().value();
        }
    }

    native_space(const native_space&) = delete;
    native_space& operator=(const native_space&) = delete;

    [[nodiscard]] int code() const { return code_; }

    [[nodiscard]] double available() const { return static_cast<double>(pool_.available()); }

    [[nodiscard]] double blocks() const { return static_cast<double>(pool_.blocks()); }

    [[nodiscard]] double block_bytes() const { return static_cast<double>(pool_.block_bytes()); }

    // JSON.parse into a block of the pool: the text is parsed in this thread's heap, and its image
    // copied into the block, where it then has the block's whole room.
    [[nodiscard]] native_shared* parse(const std::string& text) const {
        auto parsed = wd::parse(text);
        if (!parsed.has_value()) {
            return new native_shared(parsed.error());
        }
        auto made =
            shared_document::from_image(pool_, parsed.unsafe_value().image(), wd::verify::none);
        if (!made.has_value()) {
            return new native_shared(made.error());
        }
        return new native_shared(std::move(made.unsafe_value()));
    }

    // A reference more to the document, given up for a ticket: the token another thread adopts.
    [[nodiscard]] outcome share(const native_shared& shared) const {
        if (!shared.held()) {
            return failed(wd::errc::type);
        }
        shared_document copy = shared.document();
        const auto offset = static_cast<std::uint32_t>(copy.release());
        const auto issued = tickets_.issue(offset);
        if (!issued.has_value()) {
            static_cast<void>(pool_.drop(offset));
            return failed(wd::errc::capacity);
        }
        return succeeded(token_of(*issued));
    }

    // Takes the token's reference over; errc::range for a token spent, stale or no token at all.
    [[nodiscard]] native_shared* adopt(const val& token) const {
        const auto held = ticket_of(token);
        if (!held.has_value() || !tickets_.redeem(*held)) {
            return new native_shared(wd::errc::range);
        }
        auto adopted = shared_document::adopt(pool_, held->offset);
        if (!adopted.has_value()) {
            // The reference of an image refused is given back, so that its block does not leak.
            static_cast<void>(pool_.drop(held->offset));
            return new native_shared(adopted.error());
        }
        return new native_shared(std::move(adopted.unsafe_value()));
    }

    // Gives the token's reference back without adopting it.
    [[nodiscard]] outcome drop(const val& token) const {
        const auto held = ticket_of(token);
        if (!held.has_value() || !tickets_.redeem(*held)) {
            return failed(wd::errc::range);
        }
        return outcome_of(pool_.drop(held->offset));
    }

    /**
     Lays out the memory every thread shares, once, and gives its addresses: the pool of `blocks`
     blocks of `block_bytes`, `ticket_count` tickets, and `workers` words, each the claim of a
     worker's slot. The slots themselves, each a stack of `stack_bytes` with its thread-local
     storage and its pthread, are prepared as Emscripten prepares a Wasm Worker's, and join.js
     records them.
    */
    static outcome create(std::size_t block_bytes, std::size_t blocks, std::size_t ticket_count,
                          std::size_t workers, std::size_t stack_bytes) {
        const auto pool_bytes = wd::shared_pool::region_bytes(block_bytes, blocks);
        if (!pool_bytes.has_value()) {
            return failed(pool_bytes.error());
        }
        if (workers == 0 || stack_bytes == 0 || stack_bytes % 16 != 0) {
            return failed(wd::errc::range);
        }
        const std::size_t ticket_bytes = round_up(tickets::bytes(ticket_count));
        const std::size_t bytes = pool_bytes.unsafe_value() + ticket_bytes +
                                  round_up(workers * sizeof(std::atomic<std::int32_t>));
        auto* base = static_cast<std::byte*>(std::aligned_alloc(16, bytes));
        if (base == nullptr) {
            return failed(wd::errc::capacity);
        }
        std::memset(base, 0, bytes);
        std::byte* const pool = base;
        std::byte* const ticket_slots = pool + pool_bytes.unsafe_value();
        std::byte* const states = ticket_slots + ticket_bytes;
        if (const auto formatted =
                wd::shared_pool::format({pool, pool_bytes.unsafe_value()}, block_bytes);
            !formatted.has_value()) {
            std::free(base);
            return failed(formatted.error());
        }
        tickets::format(ticket_slots, ticket_count);
        for (std::size_t i = 0; i < workers; ++i) {
            new (states + (i * sizeof(std::atomic<std::int32_t>))) std::atomic<std::int32_t>(0);
            if (emscripten_malloc_wasm_worker(stack_bytes) == 0) {
                std::free(base);
                return failed(wd::errc::capacity);
            }
        }

        val layout = val::object();
        layout.set("pool", region(pool, pool_bytes.unsafe_value()));
        val ticket_area = region(ticket_slots, tickets::bytes(ticket_count));
        ticket_area.set("count", val(static_cast<double>(ticket_count)));
        layout.set("tickets", ticket_area);
        layout.set("states", address_of(states));
        return succeeded(layout);
    }

private:
    static constexpr std::size_t round_up(std::size_t bytes) { return (bytes + 15) / 16 * 16; }

    static val address_of(const std::byte* address) {
        return val(static_cast<double>(reinterpret_cast<std::uintptr_t>(address)));
    }

    static val region(const std::byte* address, std::size_t bytes) {
        val out = val::object();
        out.set("address", address_of(address));
        out.set("bytes", val(static_cast<double>(bytes)));
        return out;
    }

    int code_ = 0;
    wd::shared_pool pool_;
    tickets tickets_;
};

}  // namespace

EMSCRIPTEN_BINDINGS(webcpp_document_shared) {
    wd::binding::register_outcome();

    emscripten::class_<native_shared>("NativeShared")
        .function("code", &native_shared::code)
        .function("message", &native_shared::message)
        .function("useCount", &native_shared::use_count)
        .function("byteLength", &native_shared::byte_length)
        .function("image", &native_shared::image)
        .function("get", &native_shared::get)
        .function("getDecimal", &native_shared::get_decimal)
        .function("length", &native_shared::length)
        .function("stringify", &native_shared::stringify)
        .function("set", &native_shared::set)
        .function("makeMutable", &native_shared::make_mutable);

    emscripten::class_<native_space>("NativeSpace")
        .constructor<std::uintptr_t, std::size_t, std::uintptr_t, std::size_t>()
        .function("code", &native_space::code)
        .function("available", &native_space::available)
        .function("blocks", &native_space::blocks)
        .function("blockBytes", &native_space::block_bytes)
        .function("parse", &native_space::parse, emscripten::return_value_policy::take_ownership())
        .function("share", &native_space::share)
        .function("adopt", &native_space::adopt, emscripten::return_value_policy::take_ownership())
        .function("drop", &native_space::drop)
        .class_function("create", &native_space::create);
}
