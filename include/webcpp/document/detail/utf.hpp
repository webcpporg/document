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

#include <cstdint>
#include <string_view>

namespace webcpp::document::detail {

/** Whether text is valid UTF-8, RFC 3629: no surrogate, no overlong form, nothing past U+10FFFF. */
inline bool valid_utf8(std::string_view text) noexcept;

/** JavaScript's `length` of valid UTF-8 text: its count of UTF-16 code units. */
inline std::uint64_t utf16_length(std::string_view utf8) noexcept;

/** Writes the UTF-16 code units of valid UTF-8 text at `out` and returns how many it wrote. */
inline std::uint64_t to_utf16(std::string_view utf8, char16_t* out) noexcept;

/** Whether UTF-16 text holds no lone surrogate. */
inline bool valid_utf16(std::u16string_view text) noexcept;

/** The size in bytes of the UTF-8 of UTF-16 text that holds no lone surrogate. */
inline std::uint64_t utf8_size(std::u16string_view utf16) noexcept;

/** Writes the UTF-8 of UTF-16 text that holds no lone surrogate at `out`, and returns its size. */
inline std::uint64_t to_utf8(std::u16string_view utf16, char* out) noexcept;

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_UTF_HPP
