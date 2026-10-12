// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 UTF-8 validation, JavaScript's string length, and transcoding between UTF-8 and UTF-16, each a
 single pass that allocates nothing.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_UTF_HPP
#define WEBCPP_DOCUMENT_DETAIL_UTF_HPP

#include <webcpp/document/config.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace webcpp::document::detail {

/** The byte at `i` of `text`, as an unsigned value. */
inline std::uint32_t byte_at(std::string_view text, std::size_t i) noexcept {
    return static_cast<unsigned char>(text[i]);
}

/** Whether a byte is a continuation byte of UTF-8, `10xxxxxx`, within `low` to `high`. */
inline bool continuation(std::uint32_t byte, std::uint32_t low = 0x80,
                         std::uint32_t high = 0xbf) noexcept {
    return byte >= low && byte <= high;
}

/** Whether a UTF-16 code unit is a high surrogate, the first of a pair. */
inline bool high_surrogate(char16_t unit) noexcept {
    return unit >= 0xd800 && unit <= 0xdbff;
}

/** Whether a UTF-16 code unit is a low surrogate, the second of a pair. */
inline bool low_surrogate(char16_t unit) noexcept {
    return unit >= 0xdc00 && unit <= 0xdfff;
}

/**
 The length of the UTF-8 sequence a lead byte starts, 0 for a byte that starts none, and the range
 its second byte must fall in.
*/
struct utf8_lead {
    /** How many bytes the sequence takes, 1 to 4, or 0. */
    std::size_t length = 0;

    /** The smallest second byte. */
    std::uint32_t low = 0x80;

    /** The largest second byte. */
    std::uint32_t high = 0xbf;
};

/** What a lead byte says of its sequence, by RFC 3629's table. */
inline utf8_lead lead_of(std::uint32_t lead) noexcept {
    // The second byte's range depends on the first, which is what refuses overlong forms (E0, F0),
    // surrogates (ED) and code points past U+10FFFF (F4).
    if (lead < 0x80) {
        return {.length = 1};
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
        return {.length = 2};
    }
    if (lead >= 0xe0 && lead <= 0xef) {
        return {
            .length = 3,
            .low = lead == 0xe0 ? 0xa0U : 0x80U,
            .high = lead == 0xed ? 0x9fU : 0xbfU,
        };
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
        return {
            .length = 4,
            .low = lead == 0xf0 ? 0x90U : 0x80U,
            .high = lead == 0xf4 ? 0x8fU : 0xbfU,
        };
    }
    return {};
}

/** Whether text is valid UTF-8, RFC 3629: no surrogate, no overlong form, nothing past U+10FFFF. */
inline bool valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const utf8_lead lead = lead_of(byte_at(text, i));
        if (lead.length == 0 || text.size() - i < lead.length) {
            return false;
        }
        if (lead.length > 1 && !continuation(byte_at(text, i + 1), lead.low, lead.high)) {
            return false;
        }
        for (std::size_t k = 2; k < lead.length; ++k) {
            if (!continuation(byte_at(text, i + k))) {
                return false;
            }
        }
        i += lead.length;
    }
    return true;
}

/**
 The first UTF-16 code unit of the character UTF-8 text starts with: a high surrogate for one past
 U+FFFF; nothing when the text does not start with a valid character.
*/
inline std::optional<char16_t> first_utf16_unit(std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return std::nullopt;
    }
    const utf8_lead lead = lead_of(byte_at(utf8, 0));
    if (lead.length == 0 || utf8.size() < lead.length ||
        (lead.length > 1 && !continuation(byte_at(utf8, 1), lead.low, lead.high))) {
        return std::nullopt;
    }
    std::uint32_t code_point =
        byte_at(utf8, 0) & (0xffU >> (lead.length == 1 ? 1 : lead.length + 1));
    for (std::size_t k = 1; k < lead.length; ++k) {
        if (!continuation(byte_at(utf8, k))) {
            return std::nullopt;
        }
        code_point = (code_point << 6U) | (byte_at(utf8, k) & 0x3fU);
    }
    if (code_point < 0x10000) {
        return static_cast<char16_t>(code_point);
    }
    return static_cast<char16_t>(0xd800 + ((code_point - 0x10000) >> 10U));
}

/** JavaScript's `length` of valid UTF-8 text: its count of UTF-16 code units. */
inline std::uint64_t utf16_length(std::string_view utf8) noexcept {
    // Each character is one unit, counted at its first byte, and one more when it takes four
    // bytes, past the Basic Multilingual Plane, where UTF-16 needs a surrogate pair.
    std::uint64_t units = 0;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const std::uint32_t byte = byte_at(utf8, i);
        if (!continuation(byte)) {
            units += byte >= 0xf0 ? 2 : 1;
        }
    }
    return units;
}

/** Writes the UTF-16 code units of valid UTF-8 text at `out` and returns how many it wrote. */
inline std::uint64_t to_utf16(std::string_view utf8, char16_t* out) noexcept {
    std::uint64_t written = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::uint32_t lead = byte_at(utf8, i);
        std::uint32_t code = 0;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if (lead < 0xe0) {
            code = lead & 0x1fU;
            length = 2;
        } else if (lead < 0xf0) {
            code = lead & 0x0fU;
            length = 3;
        } else {
            code = lead & 0x07U;
            length = 4;
        }
        for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
            code = (code << 6U) | (byte_at(utf8, i + k) & 0x3fU);
        }
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            out[written++] = static_cast<char16_t>(0xd800 + (code >> 10U));
            out[written++] = static_cast<char16_t>(0xdc00 + (code & 0x3ffU));
        } else {
            out[written++] = static_cast<char16_t>(code);
        }
    }
    return written;
}

/** Whether UTF-16 text holds no lone surrogate. */
inline bool valid_utf16(std::u16string_view text) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (low_surrogate(text[i])) {
            return false;
        }
        if (high_surrogate(text[i])) {
            if (i + 1 == text.size() || !low_surrogate(text[i + 1])) {
                return false;
            }
            ++i;
        }
    }
    return true;
}

/** The size in bytes of the UTF-8 of UTF-16 text that holds no lone surrogate. */
inline std::uint64_t utf8_size(std::u16string_view utf16) noexcept {
    // A lone surrogate, which the caller rules out, would count three bytes, as to_utf8 writes it.
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        const char16_t unit = utf16[i];
        if (unit < 0x80) {
            bytes += 1;
        } else if (unit < 0x800) {
            bytes += 2;
        } else if (high_surrogate(unit) && i + 1 < utf16.size() && low_surrogate(utf16[i + 1])) {
            bytes += 4;
            ++i;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

/** Writes the UTF-8 of UTF-16 text that holds no lone surrogate at `out`, and returns its size. */
inline std::uint64_t to_utf8(std::u16string_view utf16, char* out) noexcept {
    std::uint64_t written = 0;
    const auto put = [&](std::uint32_t byte) { out[written++] = static_cast<char>(byte); };
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        std::uint32_t code = utf16[i];
        if (high_surrogate(utf16[i]) && i + 1 < utf16.size() && low_surrogate(utf16[i + 1])) {
            code = 0x10000 + ((code - 0xd800) << 10U) + (std::uint32_t{utf16[i + 1]} - 0xdc00);
            ++i;
        }
        if (code < 0x80) {
            put(code);
        } else if (code < 0x800) {
            put(0xc0U | (code >> 6U));
            put(0x80U | (code & 0x3fU));
        } else if (code < 0x10000) {
            put(0xe0U | (code >> 12U));
            put(0x80U | ((code >> 6U) & 0x3fU));
            put(0x80U | (code & 0x3fU));
        } else {
            put(0xf0U | (code >> 18U));
            put(0x80U | ((code >> 12U) & 0x3fU));
            put(0x80U | ((code >> 6U) & 0x3fU));
            put(0x80U | (code & 0x3fU));
        }
    }
    return written;
}

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_UTF_HPP
