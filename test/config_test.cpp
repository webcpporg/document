// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// <webcpp/document/config.hpp> defines WEBCPP_DOCUMENT_NO_EXCEPTIONS exactly when
// webcpp has no exceptions, and keeps the developer's own definition in a build
// that has them. test/Jamfile builds this file three times: as the build asks;
// with WEBCPP_NO_EXCEPTIONS defined as a developer defines it for every webcpp
// library, which WEBCPP_TEST_DOCUMENT_ALL_OFF tells this file; and with the macro
// itself defined as a developer defines it for document alone, on the command
// line, which WEBCPP_TEST_DOCUMENT_DEVELOPER_DEFINED tells this file.

#include <webcpp/document/config.hpp>

// webcpp's configuration, which config.hpp includes, has said whether webcpp
// has exceptions: without it, WEBCPP_HAS_EXCEPTIONS would never be defined, and
// the checks below would take every build for one without exceptions.
#ifndef WEBCPP_CONFIG_HPP
#error "config.hpp does not include <webcpp/config.hpp>"
#endif

#ifdef WEBCPP_TEST_DOCUMENT_ALL_OFF
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#error "WEBCPP_NO_EXCEPTIONS does not define WEBCPP_DOCUMENT_NO_EXCEPTIONS"
#endif
#elif defined(WEBCPP_TEST_DOCUMENT_DEVELOPER_DEFINED)
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#error "config.hpp drops WEBCPP_DOCUMENT_NO_EXCEPTIONS, which the developer defined"
#endif
#elif defined(WEBCPP_HAS_EXCEPTIONS) == defined(WEBCPP_DOCUMENT_NO_EXCEPTIONS)
#error "WEBCPP_DOCUMENT_NO_EXCEPTIONS is not defined exactly when webcpp has no exceptions"
#endif

// detail::exceptions_on and the constraint of the throwing members follow the macro.
#ifdef WEBCPP_DOCUMENT_NO_EXCEPTIONS
static_assert(!webcpp::document::detail::exceptions_on);
static_assert(!webcpp::document::detail::exceptions_enabled<int>);
#else
static_assert(webcpp::document::detail::exceptions_on);
static_assert(webcpp::document::detail::exceptions_enabled<int>);
#endif
