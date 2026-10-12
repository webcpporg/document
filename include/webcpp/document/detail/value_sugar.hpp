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

// Without exceptions the members stay declared, and their constraint refuses every call, so
// nothing needs a definition.
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS

#include <webcpp/document/errors.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

namespace webcpp::document {

namespace detail {

/** The value of `made`, or the exception of its error: what each member of the sugar returns. */
template <class T>
T value_or_raise(result<T>&& made) {
    if (!made.has_value()) {
        raise(made.error());
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(made.unsafe_value());
    }
}

}  // namespace detail

template <class Traits, class Storage>
basic_document<Traits, Storage>::basic_document()
    requires detail::exceptions_enabled<Traits> && std::same_as<Storage, owned>
    : basic_document(detail::value_or_raise(create())) {}

template <class Traits, class Storage>
basic_document<Traits, Storage>::basic_document(const basic_document& other)
    requires detail::exceptions_enabled<Traits> && std::same_as<Storage, owned>
    : basic_document(detail::value_or_raise(other.clone())) {}

template <class Traits, class Storage>
template <class T>
T basic_const_value<Traits, Storage>::as() const
    requires detail::exceptions_enabled<T> && detail::readable<Traits, T>
{
    return detail::value_or_raise(get<T>());
}

template <class Traits, class Storage>
template <class C>
std::string_view basic_const_value<Traits, Storage>::as_string() const
    requires detail::exceptions_enabled<C>
{
    return detail::value_or_raise(get<std::string_view>());
}

template <class Traits, class Storage>
template <class B>
bool basic_const_value<Traits, Storage>::as_bool() const
    requires detail::exceptions_enabled<B>
{
    return detail::value_or_raise(get<bool>());
}

template <class Traits, class Storage>
template <class T>
basic_value<Traits, Storage>& basic_value<Traits, Storage>::operator=(const T& v)
    requires detail::exceptions_enabled<T> &&
             (detail::settable<Traits, T> || std::same_as<T, undefined_t>)
{
    detail::value_or_raise(set(v));
    return *this;
}

// A deep copy, as set makes of any handle: the place is written and the handle is not rebound. A
// handle assigned itself needs no check: set reads its source before it changes anything, so the
// place gets its own value back, and a handle of nothing still throws, as set fails.
template <class Traits, class Storage>
// NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp)
basic_value<Traits, Storage>& basic_value<Traits, Storage>::operator=(const basic_value& v)
    requires detail::exceptions_enabled<Traits>
{
    detail::value_or_raise(set(v));
    return *this;
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_NO_EXCEPTIONS

#endif  // WEBCPP_DOCUMENT_DETAIL_VALUE_SUGAR_HPP
