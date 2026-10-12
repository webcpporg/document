// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The declarations the public headers share before their definitions, with the default template
 arguments, given here once: the document, its handles, its parsers, the views of its containers,
 and what reaches their insides.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_FWD_HPP
#define WEBCPP_DOCUMENT_DETAIL_FWD_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/traits.hpp>

namespace webcpp::document {

struct owned;
struct external;
struct parse_options;
class literal;
class object;
class array;

template <class Traits = default_traits, class Storage = owned>
class basic_document;

template <class Traits = default_traits, class Storage = owned>
class basic_const_value;

template <class Traits = default_traits, class Storage = owned>
class basic_value;

template <class Traits = default_traits, class Storage = owned>
class basic_parser;

template <class Traits = default_traits>
class basic_stream_parser;

template <class Handle>
class basic_object_ref;

template <class Handle>
class basic_array_ref;

namespace detail {

struct access;

template <class Traits, class Storage>
class handler;

}  // namespace detail

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_DETAIL_FWD_HPP
