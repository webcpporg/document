// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The definitions of the handles' array methods, which `value.hpp` declares and includes this header
 to define: `at`, `slice`, `concat`, `push`, `pop`, `shift`, `unshift`, `splice`, `reverse` and
 `set_length`, with JavaScript's index rules, written once in @ref relative_index.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_VALUE_ARRAY_HPP
#define WEBCPP_DOCUMENT_DETAIL_VALUE_ARRAY_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/value.hpp>

#include <cstdint>

namespace webcpp::document::detail {

/**
 ECMA-262's relative index, after ToIntegerOrInfinity: a negative index counts from the end, and
 the result is clamped to `[0, length]`, with no overflow at `INT64_MIN`.
*/
constexpr std::uint64_t relative_index(std::int64_t i, std::uint64_t length) noexcept;

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_VALUE_ARRAY_HPP
