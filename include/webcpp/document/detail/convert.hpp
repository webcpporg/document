// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The one place where C++ values become the tree's items, and the tree's nodes become C++ values:
 what `set`, the array methods and the parser store, and what `get` reads.

 Numbers are stored by value. A number is first what the traits' `number` makes of it: exact with
 `decimal128_t`, rounded as JavaScript rounds with `double`. Then it is stored as the traits'
 `integer` when it is an integer that fits, else as their `number`: `1`, `1.0` and `1e0` are one
 stored value, and `18446744073709551615` is an exact `number` by default.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_CONVERT_HPP
#define WEBCPP_DOCUMENT_DETAIL_CONVERT_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/detail/utf.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/literal.hpp>
#include <webcpp/document/number.hpp>
#include <webcpp/document/storage.hpp>
#include <webcpp/document/traits.hpp>

#include <boost/config.hpp>
#include <boost/decimal/decimal128_t.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::document::detail {

// Only the allocator's std::bad_alloc can leave `f`: the sizes it asks are of what memory already
// holds, below every container's max_size, so no std::length_error.
// NOLINTBEGIN(bugprone-exception-escape)

/**
 Calls `f`, which returns a `result`, and with exceptions turns an allocator's `std::bad_alloc`
 into @ref errc::capacity, what an operation whose working state did not fit returns before it
 changed anything. Without exceptions an allocator that fails ends the program, as it does.
*/
template <class F>
auto bad_alloc_as_capacity(F&& f) noexcept -> decltype(std::forward<F>(f)()) {
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    try {
        return std::forward<F>(f)();
    } catch (const std::bad_alloc&) {
        return errc::capacity;
    }
#else
    return std::forward<F>(f)();
#endif
}

// NOLINTEND(bugprone-exception-escape)

/** An integer type that is a number: any but `bool` and the character types. */
template <class T>
concept number_integer = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
                         !std::same_as<T, char8_t> && !std::same_as<T, char16_t> &&
                         !std::same_as<T, char32_t> && !std::same_as<T, wchar_t>;

/** A floating-point type, which a document converts only when its traits' `number` is one. */
template <class Traits, class T>
concept binary_number = std::floating_point<T> && std::floating_point<typename Traits::number>;

/**
 What @ref prepare turns into an item: the traits' scalars, every integer type, the decimal types,
 a floating-point type into binary traits, text, bytes, a date, `null` and the literals.
*/
template <class Traits, class T>
concept preparable =
    std::same_as<T, bool> || number_integer<T> || decimal_type<T> || binary_number<Traits, T> ||
    std::same_as<T, typename Traits::number> || std::convertible_to<T, std::string_view> ||
    std::convertible_to<T, std::u16string_view> ||
    std::convertible_to<T, std::span<const std::byte>> || std::same_as<T, date_ms> ||
    std::same_as<T, std::nullptr_t> || std::same_as<T, null_t> || std::same_as<T, literal> ||
    std::same_as<T, object> || std::same_as<T, array>;

/**
 What @ref read gives a node as: the traits' scalars, every integer type, the decimal types, a
 floating-point type from binary traits, text, a date and bytes.
*/
template <class Traits, class T>
concept readable =
    std::same_as<T, bool> || number_integer<T> || decimal_type<T> || binary_number<Traits, T> ||
    std::same_as<T, typename Traits::number> || std::same_as<T, std::string> ||
    std::same_as<T, std::string_view> || std::same_as<T, std::u16string> ||
    std::same_as<T, std::u16string_view> || std::same_as<T, date_ms> ||
    std::same_as<T, std::vector<std::byte>> || std::same_as<T, std::span<const std::byte>>;

template <class Traits, class Allocator = std::allocator<std::byte>>
struct converter;

/**
 A value made ready for the tree: its @ref item and what the item points at, text transcoded to the
 traits' encoding and a literal's entries, owned here until the tree has copied them.

 Text and bytes the caller gives in the traits' encoding, and a literal's keys, are not copied: the
 item points at them, which must outlive it. What it owns is allocated through `Allocator`, rebound:
 the allocator of the document the value is stored in.
*/
template <class Traits, class Allocator = std::allocator<std::byte>>
class prepared {
public:
    /** A value holding `null`, its buffers in a default-constructed allocator. */
    prepared() noexcept(std::is_nothrow_default_constructible_v<Allocator>)
        : prepared(Allocator()) {}

    /** A value holding `null`, its buffers in `alloc`. */
    explicit prepared(const Allocator& alloc) noexcept
        : alloc_(alloc), utf8_(alloc), utf16_(alloc), bytes_(alloc), entries_(alloc) {}

    /** A prepared value is not copied: its item points into it. */
    prepared(const prepared&) = delete;

    /** A prepared value is not copied: its item points into it. */
    prepared& operator=(const prepared&) = delete;

    /** Moves a prepared value, its item pointing into its new place. */
    prepared(prepared&& other) noexcept
        // A vector moved keeps its buffer, so the item still points at what it held.
        : alloc_(other.alloc_),
          item_(std::exchange(other.item_, item<Traits>{})),
          utf8_(std::move(other.utf8_)),
          utf16_(std::move(other.utf16_)),
          bytes_(std::move(other.bytes_)),
          entries_(std::move(other.entries_)) {}

    /**
     Moves a prepared value, its item pointing into its new place; both hold buffers of the same
     allocator.
    */
    prepared& operator=(prepared&& other) noexcept {
        if (this != &other) {
            item_ = std::exchange(other.item_, item<Traits>{});
            utf8_ = std::move(other.utf8_);
            utf16_ = std::move(other.utf16_);
            bytes_ = std::move(other.bytes_);
            entries_ = std::move(other.entries_);
        }
        return *this;
    }

    /** Destroys what the item points at. */
    ~prepared() = default;

    /** The item, valid while this object lives and is not changed. */
    [[nodiscard]] const item<Traits>& value() const noexcept { return item_; }

    /** The allocator of what it owns. */
    [[nodiscard]] Allocator get_allocator() const noexcept { return alloc_; }

    /**
     Makes the item a deep copy of the node `id` of `from`, which may be the tree it is stored in.
    */
    void copy_of(const tree_view<Traits>& from, std::uint64_t id) noexcept {
        clear();
        item_.from = &from;
        item_.from_id = id;
    }

private:
    template <class, class>
    friend struct converter;

    /** A buffer of `T` in the allocator. */
    template <class T>
    using buffer = std::vector<T, rebound<Allocator, T>>;

    /** A list of buffers of `T`, each in the allocator. */
    template <class T>
    using buffers = std::vector<buffer<T>, rebound<Allocator, buffer<T>>>;

    /** A new buffer of `size` values of `T` in the allocator, added to `list`. */
    template <class T>
    buffer<T>& add(buffers<T>& list, std::size_t size) {
        // Made, then moved in: a polymorphic allocator constructs an element with itself after the
        // arguments, which a size and an allocator already are.
        return list.emplace_back(buffer<T>(size, rebound<Allocator, T>(alloc_)));
    }

    /** Holds `null` again, and owns nothing. */
    void clear() noexcept {
        item_ = item<Traits>{};
        utf8_.clear();
        utf16_.clear();
        bytes_.clear();
        entries_.clear();
    }

    /** The allocator. */
    BOOST_ATTRIBUTE_NO_UNIQUE_ADDRESS Allocator alloc_;

    /** The item. */
    item<Traits> item_;

    // One buffer per value held below: a buffer never moves once the item points into it, whatever
    // is added after it.

    /** Text transcoded to UTF-8. */
    buffers<char> utf8_;

    /** Text transcoded to UTF-16. */
    buffers<char16_t> utf16_;

    /** Bytes decoded from text. */
    buffers<std::byte> bytes_;

    /** The entries of each literal container, a nested one pointing at its own. */
    buffers<literal_entry<Traits>> entries_;
};

/**
 The conversions' work, with access to what a @ref prepared value owns: C++ values into items, and
 nodes back into C++ values.
*/
template <class Traits, class Allocator>
struct converter {
    /** The traits' number. */
    using number_type = Traits::number;

    /** Its codec. */
    using codec = number_codec<number_type>;

    /** The kinds, which an item's and a node's member `kind` hide inside them. */
    using kind_t = webcpp::document::kind;

    /** A prepared value of these traits. */
    using prepared_t = prepared<Traits, Allocator>;

    /** An item of these traits. */
    using item_t = item<Traits>;

    /** A node of these traits. */
    using node_t = node<Traits>;

    /** A tree of these traits. */
    using tree_t = tree_view<Traits>;

    /** Whether strings are stored as UTF-16 code units. */
    static constexpr bool utf16_text = std::same_as<typename Traits::string, utf16>;

    /** Whether bytes take the shape `JSON.stringify` gives a Uint8Array, rather than text. */
    static constexpr bool javascript_shape = std::same_as<typename Traits::bytes, javascript_bytes>;

    /** Whether bytes are base64url text rather than base64. */
    static constexpr bool url = std::same_as<typename Traits::bytes, base64url>;

    /** A buffer of bytes in `A`, rebound. */
    template <class A>
    using bytes_in = std::vector<std::byte, rebound<A, std::byte>>;

    /**
     How many characters of UTF-16 text become ASCII at a time, on the stack, to read as base64: a
     whole number of base64's groups of four.
    */
    static constexpr std::size_t ascii_block = 256;

    /**
     Room for the longest text Date.prototype.toJSON writes, `+275760-09-13T00:00:00.000Z`, of 27
     characters: longer text is no date.
    */
    static constexpr std::size_t date_text_room = 32;

    /** Prepares `v` from nothing; on failure `out` holds `null`. */
    template <class T>
    static result<void> prepare_value(const T& v, prepared_t& out) noexcept {
        out.clear();
        result<void> held = bad_alloc_as_capacity([&] { return hold(out, v); });
        if (!held.has_value()) {
            out.clear();
        }
        return held;
    }

    /** Prepares a JSON number's text from nothing; on failure `out` holds `null`. */
    static result<void> prepare_number_text(std::string_view text, prepared_t& out) noexcept {
        out.clear();
        result<void> held = hold_number_text(out.item_, text);
        if (!held.has_value()) {
            out.clear();
        }
        return held;
    }

    /** Prepares a JSON string from nothing, revived as asked; on failure `out` holds `null`. */
    static result<void> prepare_json_string(std::string_view utf8, bool revive_dates,
                                            bool revive_bytes, prepared_t& out) noexcept {
        out.clear();
        result<void> held = bad_alloc_as_capacity(
            [&] { return hold_json_string(out, utf8, revive_dates, revive_bytes); });
        if (!held.has_value()) {
            out.clear();
        }
        return held;
    }

    /** Holds the value `v` of a type @ref prepare takes in the item of `owner`. */
    template <class T>
    static result<void> hold(prepared_t& owner, const T& v) {
        if constexpr (std::same_as<T, bool> || std::same_as<T, date_ms> ||
                      std::same_as<T, literal> || std::same_as<T, object> ||
                      std::same_as<T, array>) {
            return hold_form(owner, owner.item_, v);
        } else if constexpr (std::same_as<T, std::nullptr_t> || std::same_as<T, null_t>) {
            return {};
        } else if constexpr (number_integer<T>) {
            static_assert(sizeof(T) <= sizeof(std::int64_t),
                          "a document holds integers of at most 64 bits");
            if constexpr (std::is_signed_v<T>) {
                return hold_int64(owner.item_, static_cast<std::int64_t>(v));
            } else {
                return hold_uint64(owner.item_, static_cast<std::uint64_t>(v));
            }
        } else if constexpr (decimal_type<T>) {
            return hold_decimal(owner.item_, v);
        } else if constexpr (std::floating_point<T>) {
            return hold_binary(owner.item_, v);
        } else if constexpr (std::same_as<T, number_type>) {
            return hold_own_number(owner.item_, v);
        } else if constexpr (std::convertible_to<T, std::span<const std::byte>>) {
            return hold_form(owner, owner.item_, std::span<const std::byte>(v));
        } else {
            // A null pointer is `null`, as a literal holds it.
            if constexpr (std::is_pointer_v<T>) {
                if (v == nullptr) {
                    return {};
                }
            }
            if constexpr (std::convertible_to<T, std::string_view>) {
                return hold_form(owner, owner.item_, std::string_view(v));
            } else {
                return hold_form(owner, owner.item_, std::u16string_view(v));
            }
        }
    }

    /** Holds the integer `v` as the traits' `integer`. */
    static void hold_integer(item_t& out, std::int64_t v) noexcept {
        out.kind = kind_t::number;
        out.number = repr::integer;
        out.store(v);
    }

    /**
     Holds a value of the traits' `number` by value: as an integer when it is one that fits, else as
     itself; a NaN or an infinity only when the codec stores them.
    */
    static result<void> hold_number(item_t& out, const number_type& v) noexcept {
        if constexpr (!codec::stores_non_finite) {
            if (!codec::finite(v)) {
                return errc::range;
            }
        }
        if (const std::optional<std::int64_t> integer = codec::integral(v)) {
            hold_integer(out, *integer);
            return {};
        }
        out.kind = kind_t::number;
        out.number = repr::number;
        out.store(v);
        return {};
    }

    /** Holds a signed integer, first what the traits' `number` makes of it. */
    static result<void> hold_int64(item_t& out, std::int64_t v) noexcept {
        // decimal128_t's 34 digits hold every integer of 19, so none is built to find it exact; a
        // double holds it only when it gives it back, else the rounded value is the number.
        if constexpr (std::same_as<number_type, boost::decimal::decimal128_t>) {
            hold_integer(out, v);
            return {};
        } else {
            return hold_number(out, codec::from_int64(v));
        }
    }

    /** Holds an unsigned integer: one past `INT64_MAX` is a `number`. */
    static result<void> hold_uint64(item_t& out, std::uint64_t v) noexcept {
        if (std::in_range<std::int64_t>(v)) {
            return hold_int64(out, static_cast<std::int64_t>(v));
        }
        return hold_digits(out, webcpp::document::from_uint64(v));
    }

    /** Holds a number's digits: an integer that fits, else what the traits' `number` makes of it.
     */
    static result<void> hold_digits(item_t& out, const decimal_digits& d) noexcept {
        if (const std::optional<std::int64_t> integer = to_int64(d)) {
            return hold_int64(out, *integer);
        }
        const result<number_type> n = codec::from_digits(d);
        if (!n.has_value()) {
            return n.error();
        }
        return hold_number(out, n.unsafe_value());
    }

    /** Holds a value of a decimal type: @ref errc::range when it is not finite. */
    template <class Decimal>
    static result<void> hold_decimal(item_t& out, const Decimal& v) noexcept {
        if (!boost::decimal::isfinite(v)) {
            return errc::range;
        }
        return hold_digits(out, from_decimal(v));
    }

    /** Holds a floating-point value as the traits' binary `number`, rounded when it is wider. */
    template <class Floating>
    static result<void> hold_binary(item_t& out, Floating v) noexcept {
        // A finite value past the number's largest has no nearest value of the number.
        if constexpr (sizeof(Floating) > sizeof(number_type)) {
            if (std::isfinite(v) &&
                std::fabs(v) > static_cast<Floating>(std::numeric_limits<number_type>::max())) {
                return errc::range;
            }
        }
        return hold_number(out, static_cast<number_type>(v));
    }

    /** Holds a value of the traits' `number` given as itself, normalised through its digits. */
    static result<void> hold_own_number(item_t& out, const number_type& v) noexcept {
        if (!codec::finite(v)) {
            return hold_number(out, v);
        }
        return hold_digits(out, codec::digits(v));
    }

    /** Holds a JSON number's text, by value. */
    static result<void> hold_number_text(item_t& out, std::string_view text) noexcept {
        // For decimal128_t the codec reads exactly these digits; scanned first, an integer is held
        // without a decimal built for it.
        if constexpr (std::same_as<number_type, boost::decimal::decimal128_t>) {
            const result<decimal_digits> d = scan_number(text);
            if (!d.has_value()) {
                return d.error();
            }
            return hold_digits(out, d.unsafe_value());
        } else {
            const result<number_type> n = codec::parse(text);
            if (!n.has_value()) {
                return n.error();
            }
            return hold_number(out, n.unsafe_value());
        }
    }

    /** Holds a JSON string as a date or bytes when revived as one, else as a string. */
    static result<void> hold_json_string(prepared_t& owner, std::string_view text,
                                         bool revive_dates, [[maybe_unused]] bool revive_bytes) {
        if (!valid_utf8(text)) {
            return errc::type;
        }
        if (revive_dates) {
            if (const std::optional<date_ms> date = read_date(text)) {
                return hold_form(owner, owner.item_, *date);
            }
        }
        // Under javascript_bytes, bytes are an object's shape, which no string has.
        if constexpr (!javascript_shape) {
            if (revive_bytes) {
                if (result<bytes_in<Allocator>> decoded = decode(text, owner.alloc_);
                    decoded.has_value()) {
                    const auto& bytes =
                        owner.bytes_.emplace_back(std::move(decoded.unsafe_value()));
                    return hold_form(owner, owner.item_, std::span<const std::byte>(bytes));
                }
            }
        }
        return hold_form(owner, owner.item_, text);
    }

    // A literal's value is held by its form, one function each: hold_literal calls the one its
    // visit gives, and an object's or an array's calls hold_literal again for each value, as deep
    // as the literal is written, hence their exemptions from misc-no-recursion.

    /** Holds a literal's value in `out`, its entries owned by `owner`. */
    static result<void> hold_literal(prepared_t& owner, item_t& out,  // NOLINT(misc-no-recursion)
                                     const literal& v) {
        // NOLINTNEXTLINE(misc-no-recursion)
        const auto hold_one = [&](const auto& form) -> result<void> {
            return hold_form(owner, out, form);
        };
        return v.visit(hold_one);
    }

    /** Holds a literal's value, given as the literal. */
    static result<void> hold_form(prepared_t& owner, item_t& out,  // NOLINT(misc-no-recursion)
                                  const literal& v) {
        return hold_literal(owner, out, v);
    }

    /** Holds `null`, which `out` already holds. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, [[maybe_unused]] item_t& out,
                                  [[maybe_unused]] null_t v) noexcept {
        return {};
    }

    /** Holds a boolean. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, item_t& out,
                                  bool v) noexcept {
        out.kind = kind_t::boolean;
        out.store(v);
        return {};
    }

    /** Holds a signed integer. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, item_t& out,
                                  std::int64_t v) noexcept {
        return hold_int64(out, v);
    }

    /** Holds an unsigned integer. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, item_t& out,
                                  std::uint64_t v) noexcept {
        return hold_uint64(out, v);
    }

    /** Holds a finite decimal's digits. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, item_t& out,
                                  const decimal_digits& v) noexcept {
        return hold_digits(out, v);
    }

    /** Refuses a NaN or an infinity of a decimal type: @ref errc::range. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, [[maybe_unused]] item_t& out,
                                  [[maybe_unused]] non_finite_decimal v) noexcept {
        return errc::range;
    }

    /** Holds a binary64 value, which only a binary64 `number` takes: else @ref errc::type. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, [[maybe_unused]] item_t& out,
                                  [[maybe_unused]] binary64 v) noexcept {
        if constexpr (std::floating_point<number_type> &&
                      std::numeric_limits<number_type>::is_iec559 &&
                      sizeof(number_type) == sizeof(std::uint64_t)) {
            return hold_number(out, std::bit_cast<number_type>(v.bits));
        } else {
            return errc::type;
        }
    }

    /** Holds UTF-8 text, validated, in the traits' encoding. */
    static result<void> hold_form(prepared_t& owner, item_t& out, std::string_view v) {
        if (!valid_utf8(v)) {
            return errc::type;
        }
        out.kind = kind_t::string;
        if constexpr (utf16_text) {
            auto& units = owner.add(owner.utf16_, static_cast<std::size_t>(utf16_length(v)));
            to_utf16(v, units.data());
            out.text = std::as_bytes(std::span(units));
        } else {
            out.text = std::as_bytes(std::span(v.data(), v.size()));
        }
        return {};
    }

    /** Holds UTF-16 text in the traits' encoding: a lone surrogate kept with `utf16` only. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, item_t& out,
                                  std::u16string_view v) {
        if constexpr (utf16_text) {
            out.kind = kind_t::string;
            out.text = std::as_bytes(std::span(v.data(), v.size()));
        } else {
            if (!valid_utf16(v)) {
                return errc::type;
            }
            auto& chars = owner.add(owner.utf8_, static_cast<std::size_t>(utf8_size(v)));
            to_utf8(v, chars.data());
            out.kind = kind_t::string;
            out.text = std::as_bytes(std::span(chars));
        }
        return {};
    }

    /** Holds a date within JavaScript's range: else @ref errc::range. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, item_t& out,
                                  date_ms v) noexcept {
        const std::int64_t ms = v.time_since_epoch().count();
        if (ms > date_limit_ms || ms < -date_limit_ms) {
            return errc::range;
        }
        out.kind = kind_t::date;
        out.store(v);
        return {};
    }

    /** Holds bytes, as they are. */
    static result<void> hold_form([[maybe_unused]] prepared_t& owner, item_t& out,
                                  std::span<const std::byte> v) noexcept {
        out.kind = kind_t::bytes;
        out.text = v;
        return {};
    }

    /**
     Holds an object's members in their order, a repeated key among them: the tree keeps its last
     value in its first place.
    */
    static result<void> hold_form(prepared_t& owner, item_t& out,  // NOLINT(misc-no-recursion)
                                  const object& v) {
        const std::span<const object_member> members = v.members();
        out.kind = kind_t::object;
        if (members.empty()) {
            return {};
        }
        const std::span<literal_entry<Traits>> entries = owner.add(owner.entries_, members.size());
        out.entries = entries.data();
        out.entry_count = entries.size();
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (!valid_utf8(members[i].key)) {
                return errc::type;
            }
            entries[i].key = members[i].key;
            if (result<void> held = hold_literal(owner, entries[i].value, members[i].value);
                !held.has_value()) {
                return held;
            }
        }
        return {};
    }

    /** Holds an array's elements in order. */
    static result<void> hold_form(prepared_t& owner, item_t& out,  // NOLINT(misc-no-recursion)
                                  const array& v) {
        const std::span<const literal> elements = v.elements();
        out.kind = kind_t::array;
        if (elements.empty()) {
            return {};
        }
        const std::span<literal_entry<Traits>> entries = owner.add(owner.entries_, elements.size());
        out.entries = entries.data();
        out.entry_count = entries.size();
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (result<void> held = hold_literal(owner, entries[i].value, elements[i]);
                !held.has_value()) {
                return held;
            }
        }
        return {};
    }

    /** Room for the bytes of base64 text of `size` characters, in `alloc`. */
    template <class A>
    static bytes_in<A> room_for_base64(std::size_t size, const A& alloc) {
        // Four characters make at most three bytes, and the last group of base64url's fewer.
        return bytes_in<A>(((size / 4) * 3) + 3, rebound<A, std::byte>(alloc));
    }

    /**
     Base64 or base64url text, by the policy, decoded into a buffer of `alloc`: @ref errc::type when
     it is not.
    */
    template <class A>
    static result<bytes_in<A>> decode(std::string_view text, const A& alloc) {
        bytes_in<A> out = room_for_base64(text.size(), alloc);
        const result<std::uint64_t> size = read_base64(text, out.data(), url);
        if (!size.has_value()) {
            return errc::type;
        }
        out.resize(static_cast<std::size_t>(size.unsafe_value()));
        return out;
    }

    /** A UTF-16 unit as its ASCII character, or a zero, which no date's or base64's text holds. */
    static constexpr char ascii_of(char16_t unit) noexcept {
        return unit < 0x80 ? static_cast<char>(unit) : '\0';
    }

    /**
     UTF-16 base64 text, by the policy, decoded into a buffer of `alloc` a block at a time, made
     ASCII on the stack, so that no copy of the text is allocated: @ref errc::type when it is not.
    */
    template <class A>
    static result<bytes_in<A>> decode(std::u16string_view units, const A& alloc) {
        bytes_in<A> out = room_for_base64(units.size(), alloc);
        // Every block but the last holds whole groups, which decode alone, three bytes each; as
        // padding only ends the text, one of them that ends in '=' is not base64.
        std::array<char, ascii_block> chars{};
        std::size_t done = 0;
        std::size_t written = 0;
        do {
            const std::size_t count = std::min(units.size() - done, ascii_block);
            std::ranges::transform(units.substr(done, count), chars.begin(), ascii_of);
            done += count;
            if (done != units.size() && chars[count - 1] == '=') {
                return errc::type;
            }
            const result<std::uint64_t> size =
                read_base64(std::string_view(chars.data(), count), out.data() + written, url);
            if (!size.has_value()) {
                return errc::type;
            }
            written += static_cast<std::size_t>(size.unsafe_value());
        } while (done != units.size());
        out.resize(written);
        return out;
    }

    /** Reads a node of a kind @ref read reads, as `T`. */
    template <class T>
    static result<T> read_as(const tree_t& t, const node_t& n) {
        if constexpr (std::same_as<T, bool>) {
            if (n.kind != kind_t::boolean) {
                return errc::type;
            }
            return n.template load<bool>();
        } else if constexpr (number_integer<T>) {
            return read_integer<T>(n);
        } else if constexpr (std::floating_point<T>) {
            return read_binary<T>(n);
        } else if constexpr (decimal_type<T>) {
            return read_decimal<T>(n);
        } else if constexpr (std::same_as<T, number_type>) {
            return read_own_number(n);
        } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
            return read_utf8<T>(n);
        } else if constexpr (std::same_as<T, std::u16string> ||
                             std::same_as<T, std::u16string_view>) {
            return read_utf16<T>(n);
        } else if constexpr (std::same_as<T, date_ms>) {
            return read_date_of(n);
        } else if constexpr (std::same_as<T, std::vector<std::byte>>) {
            return read_bytes(t, n, std::allocator<std::byte>());
        } else {
            if (n.kind != kind_t::bytes) {
                return errc::type;
            }
            return n.text.view();
        }
    }

    /** An integer as `T`, when it fits: else @ref errc::range. */
    template <class T, class Integer>
    static result<T> narrow(Integer v) noexcept {
        if (!std::in_range<T>(v)) {
            return errc::range;
        }
        return static_cast<T>(v);
    }

    /** A number node as an integer type, from either representation, by a range check. */
    template <class T>
    static result<T> read_integer(const node_t& n) noexcept {
        static_assert(sizeof(T) <= sizeof(std::int64_t),
                      "a document holds integers of at most 64 bits");
        if (n.kind != kind_t::number) {
            return errc::type;
        }
        if (n.number == repr::integer) {
            return narrow<T>(n.template load<std::int64_t>());
        }
        return integer_of<T>(n.template load<number_type>());
    }

    /** A value of the traits' `number` as an integer type, when it is an integer that fits. */
    template <class T>
    static result<T> integer_of(const number_type& v) noexcept {
        if (!codec::finite(v)) {
            return errc::range;
        }
        if constexpr (std::floating_point<number_type>) {
            // A binary value's shortest digits are not its value: it converts exactly instead,
            // within 2^63 and 2^64, which it holds exactly.
            if (std::trunc(v) != v) {
                return errc::range;
            }
            if (v < 0) {
                if (v < -std::ldexp(number_type{1}, 63)) {
                    return errc::range;
                }
                return narrow<T>(static_cast<std::int64_t>(v));
            }
            if (v >= std::ldexp(number_type{1}, 64)) {
                return errc::range;
            }
            return narrow<T>(static_cast<std::uint64_t>(v));
        } else {
            const decimal_digits d = codec::digits(v);
            if (d.negative) {
                const std::optional<std::int64_t> integer = to_int64(d);
                return integer ? narrow<T>(*integer) : result<T>(errc::range);
            }
            const std::optional<std::uint64_t> integer = to_uint64(d);
            return integer ? narrow<T>(*integer) : result<T>(errc::range);
        }
    }

    /** A number node as a floating-point type, exactly: else @ref errc::range. */
    template <class T>
    static result<T> read_binary(const node_t& n) noexcept {
        if (n.kind != kind_t::number) {
            return errc::type;
        }
        if (n.number != repr::integer) {
            return binary_of<T>(n.template load<number_type>());
        }
        const auto integer = n.template load<std::int64_t>();
        const auto v = static_cast<T>(integer);
        // 2^63 is exact in T: a value that rounded up to it came from no int64 exactly.
        if (v >= std::ldexp(T{1}, 63) || static_cast<std::int64_t>(v) != integer) {
            return errc::range;
        }
        return v;
    }

    /** A value of the traits' binary `number` as a floating-point type, exactly. */
    template <class T>
    static result<T> binary_of(const number_type& v) noexcept {
        if constexpr (std::same_as<T, number_type>) {
            return v;
        } else {
            if (std::isnan(v)) {
                return std::numeric_limits<T>::quiet_NaN();
            }
            if (std::isinf(v)) {
                return v < 0 ? -std::numeric_limits<T>::infinity()
                             : std::numeric_limits<T>::infinity();
            }
            using wide = std::common_type_t<T, number_type>;
            if (static_cast<wide>(std::fabs(v)) >
                static_cast<wide>(std::numeric_limits<T>::max())) {
                return errc::range;
            }
            const auto narrowed = static_cast<T>(v);
            if (static_cast<number_type>(narrowed) != v) {
                return errc::range;
            }
            return narrowed;
        }
    }

    /** A number node as a decimal type, exactly: else @ref errc::range. */
    template <class Decimal>
    static result<Decimal> read_decimal(const node_t& n) noexcept {
        if (n.kind != kind_t::number) {
            return errc::type;
        }
        if (n.number == repr::integer) {
            return to_decimal<Decimal>(
                webcpp::document::from_int64(n.template load<std::int64_t>()));
        }
        const auto v = n.template load<number_type>();
        if constexpr (std::same_as<Decimal, number_type>) {
            return v;
        } else {
            if (!codec::finite(v)) {
                return errc::range;
            }
            return to_decimal<Decimal>(codec::digits(v));
        }
    }

    /** A number node as the traits' `number`, which neither a decimal nor a binary type is. */
    static result<number_type> read_own_number(const node_t& n) noexcept {
        if (n.kind != kind_t::number) {
            return errc::type;
        }
        if (n.number != repr::integer) {
            return n.template load<number_type>();
        }
        const auto integer = n.template load<std::int64_t>();
        const number_type v = codec::from_int64(integer);
        if (codec::integral(v) != integer) {
            return errc::range;
        }
        return v;
    }

    /** A string node as UTF-8: a view only of `utf8` strings, a copy of either. */
    template <class T>
    static result<T> read_utf8(const node_t& n) {
        if (n.kind != kind_t::string) {
            return errc::type;
        }
        if constexpr (!utf16_text) {
            return T(n.text.chars());
        } else if constexpr (std::same_as<T, std::string_view>) {
            return errc::type;
        } else {
            // A lone surrogate, which JavaScript's strings keep, has no UTF-8.
            const std::u16string_view units = n.text.units();
            if (!valid_utf16(units)) {
                return errc::type;
            }
            std::string out(static_cast<std::size_t>(utf8_size(units)), '\0');
            to_utf8(units, out.data());
            return out;
        }
    }

    /** A string node as UTF-16: a view only of `utf16` strings, a copy of either. */
    template <class T>
    static result<T> read_utf16(const node_t& n) {
        if (n.kind != kind_t::string) {
            return errc::type;
        }
        if constexpr (utf16_text) {
            return T(n.text.units());
        } else if constexpr (std::same_as<T, std::u16string_view>) {
            return errc::type;
        } else {
            const std::string_view chars = n.text.chars();
            if (!valid_utf8(chars)) {
                return errc::type;
            }
            std::u16string out(static_cast<std::size_t>(utf16_length(chars)), u'\0');
            to_utf16(chars, out.data());
            return out;
        }
    }

    /** A string node's text, as Date.prototype.toJSON's, as a date: else nothing. */
    static std::optional<date_ms> date_of_text(const node_t& n) noexcept {
        if constexpr (utf16_text) {
            // A date's text is short: longer text is none, and shorter is made ASCII on the stack.
            const std::u16string_view units = n.text.units();
            std::array<char, date_text_room> chars{};
            if (units.size() > chars.size()) {
                return std::nullopt;
            }
            std::ranges::transform(units, chars.begin(), ascii_of);
            return read_date(std::string_view(chars.data(), units.size()));
        } else {
            return read_date(n.text.chars());
        }
    }

    /** A date node, or a string that is exactly Date.prototype.toJSON's text, as a date. */
    static result<date_ms> read_date_of(const node_t& n) noexcept {
        if (n.kind == kind_t::date) {
            return n.template load<date_ms>();
        }
        if (n.kind != kind_t::string) {
            return errc::type;
        }
        const std::optional<date_ms> date = date_of_text(n);
        if (!date) {
            return errc::type;
        }
        return *date;
    }

    /** A bytes node, or what the bytes policy reads as bytes, as bytes in a buffer of `alloc`. */
    template <class A>
    static result<bytes_in<A>> read_bytes(const tree_t& t, const node_t& n, const A& alloc) {
        if (n.kind == kind_t::bytes) {
            const std::span<const std::byte> bytes = n.text.view();
            return bytes_in<A>(bytes.begin(), bytes.end(), rebound<A, std::byte>(alloc));
        }
        if constexpr (javascript_shape) {
            if (n.kind != kind_t::object) {
                return errc::type;
            }
            return read_byte_object(t, n, alloc);
        } else {
            if (n.kind != kind_t::string) {
                return errc::type;
            }
            if constexpr (utf16_text) {
                return decode(n.text.units(), alloc);
            } else {
                return decode(n.text.chars(), alloc);
            }
        }
    }

    /**
     An object as bytes in a buffer of `alloc` when it has the shape `JSON.stringify` gives a
     Uint8Array: keys exactly `"0"` to `"n-1"`, each holding an integer from 0 to 255.
    */
    template <class A>
    static result<bytes_in<A>> read_byte_object(const tree_t& t, const node_t& n, const A& alloc) {
        // As many keys as the indices, each found: the keys are exactly those.
        const std::uint64_t count = t.count(n.id);
        bytes_in<A> out{rebound<A, std::byte>(alloc)};
        out.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            std::array<char, 24> key{};
            const std::to_chars_result written =
                std::to_chars(key.data(), key.data() + key.size(), i);
            const node_t* element = t.member(
                n.id,
                std::string_view(key.data(), static_cast<std::size_t>(written.ptr - key.data())));
            const result<std::uint8_t> byte = element == nullptr
                                                  ? result<std::uint8_t>(errc::type)
                                                  : read_integer<std::uint8_t>(*element);
            if (!byte.has_value()) {
                return errc::type;
            }
            out.push_back(static_cast<std::byte>(byte.unsafe_value()));
        }
        return out;
    }

    /** Whether two nodes have the same kind and scalar, and a container the same size. */
    static bool same_value(const tree_t& a, const node_t& x, const tree_t& b,
                           const node_t& y) noexcept {
        if (x.kind != y.kind) {
            return false;
        }
        switch (x.kind) {
            case kind_t::undefined:
            case kind_t::null: return true;
            case kind_t::boolean: return x.template load<bool>() == y.template load<bool>();
            case kind_t::number: return same_number(x, y);
            case kind_t::string:
            case kind_t::bytes: return std::ranges::equal(x.text.view(), y.text.view());
            case kind_t::date: return x.template load<date_ms>() == y.template load<date_ms>();
            case kind_t::array:
            case kind_t::object: return a.count(x.id) == b.count(y.id);
        }
        return false;
    }

    /** Whether two numbers are held the same way with equal values. */
    static bool same_number(const node_t& x, const node_t& y) noexcept {
        if (x.number != y.number) {
            return false;
        }
        if (x.number == repr::integer) {
            return x.template load<std::int64_t>() == y.template load<std::int64_t>();
        }
        return x.template load<number_type>() == y.template load<number_type>();
    }

    /**
     The member of the object `object` of `t` whose key is the key of `child`, or null: the two
     trees have the same traits, so the key is looked for as it is stored.
    */
    static const node_t* member_like(const tree_t& t, std::uint64_t object,
                                     const node_t& child) noexcept {
        return t.member_stored(object, child.key.chars());
    }

    /**
     The node of `b` that stands where `child`, a child of the node `x` of `a`, stands under `y`:
     the element of the same index, the member of the same key.
    */
    static const node_t* counterpart(const tree_t& b, const node_t& x, const node_t& y,
                                     const node_t& child) noexcept {
        if (x.kind == kind_t::array) {
            return b.element(y.id, child.ordinal);
        }
        return member_like(b, y.id, child);
    }

    /**
     Whether two subtrees are equal: `x` walked in document order through its nodes' links, each
     node paired with its counterpart in `y`, so that no stack is allocated and nothing can fail.
    */
    static bool equal_trees(const tree_t& a, const node_t* x, const tree_t& b,
                            const node_t* y) noexcept {
        if (x == nullptr || y == nullptr) {
            return x == y;
        }
        const node_t* p = x;
        const node_t* q = y;
        while (true) {
            if (!same_value(a, *p, b, *q)) {
                return false;
            }
            // Down to the first child: containers of one kind and size, so `q` has as many.
            if (const node_t* child = a.first_child(p->id)) {
                q = counterpart(b, *p, *q, *child);
                p = child;
                if (q == nullptr) {
                    return false;
                }
                continue;
            }
            // Else to the next sibling of the nearest node that has one, up to `x`.
            while (true) {
                if (p == x) {
                    return true;
                }
                const node_t* parent_p = a.find(p->parent);
                const node_t* parent_q = b.find(q->parent);
                if (const node_t* sibling = a.next_sibling(*p)) {
                    q = counterpart(b, *parent_p, *parent_q, *sibling);
                    p = sibling;
                    if (q == nullptr) {
                        return false;
                    }
                    break;
                }
                p = parent_p;
                q = parent_q;
            }
        }
    }
};

/**
 Turns a C++ value into an item: @ref errc::type for text invalid in its encoding or a
 floating-point value into traits whose `number` is not binary, @ref errc::range for a non-finite
 decimal or a date out of JavaScript's range.
*/
template <class T, class Traits, class Allocator>
    requires preparable<Traits, T>
result<void> prepare(const T& v, prepared<Traits, Allocator>& out) noexcept {
    return converter<Traits, Allocator>::prepare_value(v, out);
}

/**
 Turns a JSON number's text into an item, by value: @ref errc::range when the traits' `number`
 refuses it.
*/
template <class Traits, class Allocator>
result<void> prepare_number(std::string_view json_number,
                            prepared<Traits, Allocator>& out) noexcept {
    return converter<Traits, Allocator>::prepare_number_text(json_number, out);
}

/**
 Turns a JSON string, in valid UTF-8, into an item: a string, or with `revive_dates` a date when it
 is exactly Date.prototype.toJSON's text, or with `revive_bytes` bytes when it is text of the
 traits' base64 policy; @ref errc::type for text that is not valid UTF-8 after all.
*/
template <class Traits, class Allocator>
result<void> prepare_string(std::string_view utf8, bool revive_dates, bool revive_bytes,
                            prepared<Traits, Allocator>& out) noexcept {
    return converter<Traits, Allocator>::prepare_json_string(utf8, revive_dates, revive_bytes, out);
}

/** The item of an empty container, `null`, or for a root @ref kind::undefined. */
template <class Traits>
item<Traits> empty_item(webcpp::document::kind k) noexcept {
    item<Traits> made;
    if (k == webcpp::document::kind::array || k == webcpp::document::kind::object ||
        k == webcpp::document::kind::undefined) {
        made.kind = k;
    }
    return made;
}

// A value read as a std::string, a std::u16string or a std::vector is the program's own, in the
// standard allocator, not the document's: its exhaustion ends the program, as the library
// tolerates of what it hands the program; read_bytes_with reads bytes into an allocator softly.
// NOLINTBEGIN(bugprone-exception-escape)
/**
 Reads a node as `T`: @ref errc::type for a null node (undefined) or another kind, @ref errc::range
 for a number that is not an integer, does not fit `T`, or that `T` cannot hold exactly.

 A string asked as @ref date_ms is read when it is exactly Date.prototype.toJSON's text, and as
 bytes when it is text of the traits' base64 policy; an object asked as bytes under
 `javascript_bytes` is read when its keys are exactly `"0"` to `"n-1"` with integers 0 to 255.
 `std::string_view` is read from `utf8` strings only and `std::u16string_view` from `utf16` ones,
 and a `utf16` string that holds a lone surrogate, which UTF-8 cannot hold, is no `std::string`.
*/
template <class T, class Traits>
    requires readable<Traits, T>
result<T> read(const tree_view<Traits>& t, const node<Traits>* n) noexcept {
    if (n == nullptr) {
        return errc::type;
    }
    return converter<Traits>::template read_as<T>(t, *n);
}

// NOLINTEND(bugprone-exception-escape)

/**
 Reads a node as bytes, as @ref read reads `std::vector<std::byte>`, into a buffer of `alloc`: a
 bytes node, text of the traits' base64 policy, or under `javascript_bytes` an object of a
 Uint8Array's shape; @ref errc::type for a null node or another, and with exceptions
 @ref errc::capacity when `alloc` has no room for the bytes.
*/
template <class Traits, class Allocator>
result<std::vector<std::byte, rebound<Allocator, std::byte>>> read_bytes_with(
    const tree_view<Traits>& t, const node<Traits>* n, const Allocator& alloc) noexcept {
    if (n == nullptr) {
        return errc::type;
    }
    return bad_alloc_as_capacity([&] { return converter<Traits>::read_bytes(t, *n, alloc); });
}

/**
 Whether two subtrees are equal, structurally: the same kind; numbers by value; strings, dates and
 bytes by value; arrays element by element; objects with the same keys, each with an equal value,
 in any order. A null node is undefined, equal only to another.
*/
template <class Traits>
bool equal(const tree_view<Traits>& a, const node<Traits>* x, const tree_view<Traits>& b,
           const node<Traits>* y) noexcept {
    return converter<Traits>::equal_trees(a, x, b, y);
}

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_CONVERT_HPP
