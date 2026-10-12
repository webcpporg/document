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
#include <webcpp/document/traits.hpp>

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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::document::detail {

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

template <class Traits>
struct converter;

/**
 A value made ready for the tree: its @ref item and what the item points at, text transcoded to the
 traits' encoding and a literal's entries, owned here until the tree has copied them.

 Text and bytes the caller gives in the traits' encoding, and a literal's keys, are not copied: the
 item points at them, which must outlive it.
*/
template <class Traits>
class prepared {
public:
    /** A value holding `null`. */
    prepared() noexcept = default;

    /** A prepared value is not copied: its item points into it. */
    prepared(const prepared&) = delete;

    /** A prepared value is not copied: its item points into it. */
    prepared& operator=(const prepared&) = delete;

    /** Moves a prepared value, its item pointing into its new place. */
    prepared(prepared&& other) noexcept
        // A vector moved keeps its buffer, so the item still points at what it held.
        : item_(std::exchange(other.item_, item<Traits>{})),
          utf8_(std::move(other.utf8_)),
          utf16_(std::move(other.utf16_)),
          bytes_(std::move(other.bytes_)),
          entries_(std::move(other.entries_)) {}

    /** Moves a prepared value, its item pointing into its new place. */
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

    /**
     Makes the item a deep copy of the node `id` of `from`, which may be the tree it is stored in.
    */
    void copy_of(const tree<Traits>& from, std::uint64_t id) noexcept {
        clear();
        item_.from = &from;
        item_.from_id = id;
    }

private:
    template <class>
    friend struct converter;

    /** Holds `null` again, and owns nothing. */
    void clear() noexcept {
        item_ = item<Traits>{};
        utf8_.clear();
        utf16_.clear();
        bytes_.clear();
        entries_.clear();
    }

    /** The item. */
    item<Traits> item_;

    // One buffer per value held below: a buffer never moves once the item points into it, whatever
    // is added after it.

    /** Text transcoded to UTF-8. */
    std::vector<std::vector<char>> utf8_;

    /** Text transcoded to UTF-16. */
    std::vector<std::vector<char16_t>> utf16_;

    /** Bytes decoded from text. */
    std::vector<std::vector<std::byte>> bytes_;

    /** The entries of each literal container, a nested one pointing at its own. */
    std::vector<std::vector<literal_entry<Traits>>> entries_;
};

/**
 The conversions' work, with access to what a @ref prepared value owns: C++ values into items, and
 nodes back into C++ values.
*/
template <class Traits>
struct converter {
    /** The traits' number. */
    using number_type = Traits::number;

    /** Its codec. */
    using codec = number_codec<number_type>;

    /** The kinds, which an item's and a node's member `kind` hide inside them. */
    using kind_t = webcpp::document::kind;

    /** A prepared value of these traits. */
    using prepared_t = prepared<Traits>;

    /** An item of these traits. */
    using item_t = item<Traits>;

    /** A node of these traits. */
    using node_t = node<Traits>;

    /** A tree of these traits. */
    using tree_t = tree<Traits>;

    /** Whether strings are stored as UTF-16 code units. */
    static constexpr bool utf16_text = std::same_as<typename Traits::string, utf16>;

    /** Whether bytes take the shape `JSON.stringify` gives a Uint8Array, rather than text. */
    static constexpr bool javascript_shape = std::same_as<typename Traits::bytes, javascript_bytes>;

    /** Whether bytes are base64url text rather than base64. */
    static constexpr bool url = std::same_as<typename Traits::bytes, base64url>;

    /** Prepares `v` from nothing; on failure `out` holds `null`. */
    template <class T>
    static result<void> prepare_value(const T& v, prepared_t& out) {
        out.clear();
        result<void> held = hold(out, v);
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
                                            bool revive_bytes, prepared_t& out) {
        out.clear();
        result<void> held = hold_json_string(out, utf8, revive_dates, revive_bytes);
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
                if (result<std::vector<std::byte>> decoded = decode(text); decoded.has_value()) {
                    const std::vector<std::byte>& bytes =
                        owner.bytes_.emplace_back(std::move(decoded).unsafe_value());
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
            std::vector<char16_t>& units =
                owner.utf16_.emplace_back(static_cast<std::size_t>(utf16_length(v)));
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
            std::vector<char>& chars =
                owner.utf8_.emplace_back(static_cast<std::size_t>(utf8_size(v)));
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
        const std::span<literal_entry<Traits>> entries =
            owner.entries_.emplace_back(members.size());
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
        const std::span<literal_entry<Traits>> entries =
            owner.entries_.emplace_back(elements.size());
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

    /** Base64 or base64url text, by the policy, decoded: @ref errc::type when it is not. */
    static result<std::vector<std::byte>> decode(std::string_view text) {
        // Four characters make at most three bytes, and the last group of base64url's fewer.
        std::vector<std::byte> out(((text.size() / 4) * 3) + 3);
        const result<std::uint64_t> size = read_base64(text, out.data(), url);
        if (!size.has_value()) {
            return errc::type;
        }
        out.resize(static_cast<std::size_t>(size.unsafe_value()));
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
            return read_bytes(t, n);
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

    /**
     Calls `f` with a string node's text as `std::string_view`: with `utf16`, its ASCII units, and
     a zero for each other, which no date's or base64's text holds.
    */
    template <class F>
    static auto with_ascii(const node_t& n, F&& f) {
        if constexpr (utf16_text) {
            const std::u16string_view units = n.text.units();
            std::string chars(units.size(), '\0');
            std::ranges::transform(units, chars.begin(), [](char16_t unit) {
                return unit < 0x80 ? static_cast<char>(unit) : '\0';
            });
            return std::forward<F>(f)(std::string_view(chars));
        } else {
            return std::forward<F>(f)(n.text.chars());
        }
    }

    /** A date node, or a string that is exactly Date.prototype.toJSON's text, as a date. */
    static result<date_ms> read_date_of(const node_t& n) {
        if (n.kind == kind_t::date) {
            return n.template load<date_ms>();
        }
        if (n.kind != kind_t::string) {
            return errc::type;
        }
        const std::optional<date_ms> date =
            with_ascii(n, [](std::string_view text) { return read_date(text); });
        if (!date) {
            return errc::type;
        }
        return *date;
    }

    /** A bytes node, or what the bytes policy reads as bytes, as bytes. */
    static result<std::vector<std::byte>> read_bytes(const tree_t& t, const node_t& n) {
        if (n.kind == kind_t::bytes) {
            const std::span<const std::byte> bytes = n.text.view();
            return std::vector<std::byte>(bytes.begin(), bytes.end());
        }
        if constexpr (javascript_shape) {
            if (n.kind != kind_t::object) {
                return errc::type;
            }
            return read_byte_object(t, n);
        } else {
            if (n.kind != kind_t::string) {
                return errc::type;
            }
            return with_ascii(n, [](std::string_view text) { return decode(text); });
        }
    }

    /**
     An object as bytes when it has the shape `JSON.stringify` gives a Uint8Array: keys exactly
     `"0"` to `"n-1"`, each holding an integer from 0 to 255.
    */
    static result<std::vector<std::byte>> read_byte_object(const tree_t& t, const node_t& n) {
        // As many keys as the indices, each found: the keys are exactly those.
        const std::uint64_t count = t.count(n.id);
        std::vector<std::byte> out;
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

    /** The member of the object `object` of `t` whose key is the key of `child`, or null. */
    static const node_t* member_like(const tree_t& t, std::uint64_t object, const node_t& child) {
        if constexpr (utf16_text) {
            // Keys enter in UTF-8, so a stored one always has its UTF-8.
            const std::u16string_view units = child.key.units();
            std::string key(static_cast<std::size_t>(utf8_size(units)), '\0');
            to_utf8(units, key.data());
            return t.member(object, key);
        } else {
            return t.member(object, child.key.chars());
        }
    }

    /**
     Pairs the children of two containers of the same kind and size to compare: elements by index,
     members by key; false when a key of `x` is not in `y`.
    */
    static bool pair_children(const tree_t& a, const node_t& x, const tree_t& b, const node_t& y,
                              std::vector<std::pair<const node_t*, const node_t*>>& pending) {
        if (x.kind == kind_t::array) {
            const node_t* d = b.first_child(y.id);
            for (const node_t* c = a.first_child(x.id); c != nullptr && d != nullptr;
                 c = a.next_sibling(*c)) {
                pending.emplace_back(c, d);
                d = b.next_sibling(*d);
            }
            return true;
        }
        for (const node_t* c = a.first_child(x.id); c != nullptr; c = a.next_sibling(*c)) {
            const node_t* d = member_like(b, y.id, *c);
            if (d == nullptr) {
                return false;
            }
            pending.emplace_back(c, d);
        }
        return true;
    }

    /** Whether two subtrees are equal, walked with a stack of their own rather than recursion. */
    static bool equal_trees(const tree_t& a, const node_t* x, const tree_t& b, const node_t* y) {
        if (x == nullptr || y == nullptr) {
            return x == y;
        }
        std::vector<std::pair<const node_t*, const node_t*>> pending{{x, y}};
        while (!pending.empty()) {
            const auto [p, q] = pending.back();
            pending.pop_back();
            if (!same_value(a, *p, b, *q)) {
                return false;
            }
            const bool container = p->kind == kind_t::array || p->kind == kind_t::object;
            if (container && !pair_children(a, *p, b, *q, pending)) {
                return false;
            }
        }
        return true;
    }
};

// A conversion ends the program where the process heap, which holds a prepared value's transcoded
// text and literal entries, a value read as a std::string or a std::vector, or equal's stack, runs
// out, as the library tolerates.
// NOLINTBEGIN(bugprone-exception-escape)

/**
 Turns a C++ value into an item: @ref errc::type for text invalid in its encoding or a
 floating-point value into traits whose `number` is not binary, @ref errc::range for a non-finite
 decimal or a date out of JavaScript's range.
*/
template <class T, class Traits>
    requires preparable<Traits, T>
result<void> prepare(const T& v, prepared<Traits>& out) noexcept {
    return converter<Traits>::prepare_value(v, out);
}

/**
 Turns a JSON number's text into an item, by value: @ref errc::range when the traits' `number`
 refuses it.
*/
template <class Traits>
result<void> prepare_number(std::string_view json_number, prepared<Traits>& out) noexcept {
    return converter<Traits>::prepare_number_text(json_number, out);
}

/**
 Turns a JSON string, in valid UTF-8, into an item: a string, or with `revive_dates` a date when it
 is exactly Date.prototype.toJSON's text, or with `revive_bytes` bytes when it is text of the
 traits' base64 policy; @ref errc::type for text that is not valid UTF-8 after all.
*/
template <class Traits>
result<void> prepare_string(std::string_view utf8, bool revive_dates, bool revive_bytes,
                            prepared<Traits>& out) noexcept {
    return converter<Traits>::prepare_json_string(utf8, revive_dates, revive_bytes, out);
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
result<T> read(const tree<Traits>& t, const node<Traits>* n) noexcept {
    if (n == nullptr) {
        return errc::type;
    }
    return converter<Traits>::template read_as<T>(t, *n);
}

/**
 Whether two subtrees are equal, structurally: the same kind; numbers by value; strings, dates and
 bytes by value; arrays element by element; objects with the same keys, each with an equal value,
 in any order. A null node is undefined, equal only to another.
*/
template <class Traits>
bool equal(const tree<Traits>& a, const node<Traits>* x, const tree<Traits>& b,
           const node<Traits>* y) noexcept {
    return converter<Traits>::equal_trees(a, x, b, y);
}

// NOLINTEND(bugprone-exception-escape)

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_CONVERT_HPP
