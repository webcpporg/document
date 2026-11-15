// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 `JSON.stringify` of a string alone: a string written as a JSON string literal, appended to a
 string of the caller's, with the escapes `stringify` writes, without a document.
*/

#ifndef WEBCPP_DOCUMENT_QUOTE_HPP
#define WEBCPP_DOCUMENT_QUOTE_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/utf.hpp>
#include <webcpp/document/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace webcpp::document {

namespace detail {

/** An output of @ref quote, a string that `append(data, size)` appends to. */
template <class Out>
concept appendable =
    requires(Out& out, const char* data, std::size_t size) { out.append(data, size); };

/** Writes `\\u` and the four lowercase hex digits of `unit` at `out`, and returns 6. */
inline std::size_t unicode_escape(std::uint32_t unit, char* out) noexcept {
    constexpr std::string_view hex = "0123456789abcdef";
    out[0] = '\\';
    out[1] = 'u';
    out[2] = hex[(unit >> 12U) & 0xfU];
    out[3] = hex[(unit >> 8U) & 0xfU];
    out[4] = hex[(unit >> 4U) & 0xfU];
    out[5] = hex[unit & 0xfU];
    return 6;
}

/** Hands `put` the escape of a unit below 0x20, `"` or `\\`, and returns what `put` returned. */
template <class Put>
bool escape_into(std::uint32_t unit, Put& put) {
    switch (unit) {
        case '"': return put("\\\"");
        case '\\': return put("\\\\");
        case '\b': return put("\\b");
        case '\f': return put("\\f");
        case '\n': return put("\\n");
        case '\r': return put("\\r");
        case '\t': return put("\\t");
        default: break;
    }
    std::array<char, 6> text{};
    return put(std::string_view(text.data(), unicode_escape(unit, text.data())));
}

/**
 QuoteJSONString of valid UTF-8 text, handed to `put` in pieces: the runs that need no escape are
 handed as they are.

 Tip: `put` takes a `std::string_view` and returns whether the writing goes on; the first false
 stops it, and is returned.
*/
template <class Put>
bool quote_json(std::string_view text, Put& put) {
    if (!put("\"")) {
        return false;
    }
    std::size_t run = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto unit = static_cast<unsigned char>(text[i]);
        if (unit >= 0x20 && unit != '"' && unit != '\\') {
            continue;
        }
        if (!put(text.substr(run, i - run)) || !escape_into(unit, put)) {
            return false;
        }
        run = i + 1;
    }
    return put(text.substr(run)) && put("\"");
}

/**
 QuoteJSONString of UTF-16 text, handed to `put` in pieces: transcoded to UTF-8 through a buffer,
 and a lone surrogate written as `\\u` and its four lowercase hex digits, as JavaScript writes it
 since ES2019.
*/
template <class Put>
bool quote_json(std::u16string_view units, Put& put) {
    // The longest a unit or a pair becomes: six characters, the escape of a lone surrogate.
    constexpr std::size_t longest = 6;
    std::array<char, 256> buffer{};
    std::size_t used = 0;
    if (!put("\"")) {
        return false;
    }
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (buffer.size() - used < longest) {
            if (!put(std::string_view(buffer.data(), used))) {
                return false;
            }
            used = 0;
        }
        const char16_t unit = units[i];
        const bool pair =
            high_surrogate(unit) && i + 1 < units.size() && low_surrogate(units[i + 1]);
        if (pair) {
            used += static_cast<std::size_t>(to_utf8(units.substr(i, 2), buffer.data() + used));
            ++i;
        } else if (high_surrogate(unit) || low_surrogate(unit)) {
            used += unicode_escape(unit, buffer.data() + used);
        } else if (unit >= 0x80) {
            used += static_cast<std::size_t>(to_utf8(units.substr(i, 1), buffer.data() + used));
        } else if (unit >= 0x20 && unit != u'"' && unit != u'\\') {
            buffer[used++] = static_cast<char>(unit);
        } else {
            if (!put(std::string_view(buffer.data(), used)) || !escape_into(unit, put)) {
                return false;
            }
            used = 0;
        }
    }
    return put(std::string_view(buffer.data(), used)) && put("\"");
}

}  // namespace detail

/**
 `JSON.stringify(s)` of a string `s`: appends UTF-8 text to `out` as a JSON string literal, its
 quotes included.

 The bytes are the ones @ref stringify writes of the same string: `"` and `\\` escaped with a
 backslash, `\\b`, `\\f`, `\\n`, `\\r` and `\\t` for their controls, `\\u` and four lowercase hex
 digits for every other unit below U+0020, and everything else as it is, U+007F and U+2028
 included. It allocates only through `out`, so that a JSON text can be written field by field into
 a string of the caller's allocator, a `std::pmr::string` among them.

 @tparam Out The output: `std::string`, `std::pmr::string`, or any type with
 `append(const char*, std::size_t)`.
 @param utf8 The text, in UTF-8.
 @param out The string the literal is appended to.
 @return Success; @ref errc::type when `utf8` is not valid UTF-8, which no JavaScript string is,
 with `out` unchanged. An allocation failure of `out` propagates as `out` reports it.
 @see "Writing JSON", in the guide.
*/
template <class Out>
    requires detail::appendable<Out>
result<void> quote(std::string_view utf8, Out& out) {
    if (!detail::valid_utf8(utf8)) {
        return errc::type;
    }
    const auto put = [&out](std::string_view text) {
        out.append(text.data(), text.size());
        return true;
    };
    detail::quote_json(utf8, put);
    return {};
}

/**
 `JSON.stringify(s)` of a string `s`: appends UTF-16 text to `out` as a JSON string literal, its
 quotes included, in UTF-8.

 It writes what the UTF-8 overload writes of the same characters, and a lone surrogate, which a
 JavaScript string may hold, as `\\u` and its four lowercase hex digits, as JavaScript writes it
 since ES2019 and as @ref stringify writes it under `js_traits`.

 @tparam Out The output: `std::string`, `std::pmr::string`, or any type with
 `append(const char*, std::size_t)`.
 @param utf16 The text, in UTF-16 code units, any sequence of them.
 @param out The string the literal is appended to; the only thing that allocates.
 @see "Writing JSON", in the guide.
*/
template <class Out>
    requires detail::appendable<Out>
void quote(std::u16string_view utf16, Out& out) {
    const auto put = [&out](std::string_view text) {
        out.append(text.data(), text.size());
        return true;
    };
    detail::quote_json(utf16, put);
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_QUOTE_HPP
