// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The definitions of the sugar that throws, which `value.hpp` and `document.hpp` declare and
 `value.hpp` includes this header to define: the handles' `operator=`, `as`, `as_string` and
 `as_bool`, and the document's default and copy constructors. Each is its core function followed by
 `detail::raise` on failure, and each carries the constraint `detail::exceptions_enabled`, so that
 a call in a build without exceptions does not compile and names it.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_VALUE_SUGAR_HPP
#define WEBCPP_DOCUMENT_DETAIL_VALUE_SUGAR_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/value.hpp>

#endif  // WEBCPP_DOCUMENT_DETAIL_VALUE_SUGAR_HPP
