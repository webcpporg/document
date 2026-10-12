// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The handler of Boost.JSON's `basic_parser` that builds a document: each completed value becomes
 an item stored by the tree's operations, and each value a selector matches is delivered when it
 completes.

 Under an object a value is set as a member, a repeated key replacing the value in place, as
 JavaScript does; under an array it is appended; at the top it is the root. The handler keeps the
 text's path of the value being read, keys and indices in the text, which the selectors match and
 `on` does not renumber.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_HANDLER_HPP
#define WEBCPP_DOCUMENT_DETAIL_HANDLER_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/convert.hpp>
#include <webcpp/document/detail/selector.hpp>
#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/parser.hpp>
#include <webcpp/document/value.hpp>

#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
#include <exception>
#endif

namespace webcpp::document::detail {

/** A container being read: its node, its kind, the next element's index in the text. */
struct frame {
    /** The container's node id. */
    std::uint64_t id = 0;

    /** Whether it is an array; else an object. */
    bool is_array = false;

    /** The next element's index in the text, which `on` does not renumber. */
    std::uint64_t next_index = 0;
};

/** The handler of Boost.JSON's `basic_parser` that builds the document of a @ref basic_parser. */
template <class Traits, class Storage>
class handler {
public:
    /** The largest object Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_object_size = static_cast<std::size_t>(-1);

    /** The largest array Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_array_size = static_cast<std::size_t>(-1);

    /** The largest key Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_key_size = static_cast<std::size_t>(-1);

    /** The largest string Boost.JSON hands the handler: no limit of its own. */
    static constexpr std::size_t max_string_size = static_cast<std::size_t>(-1);

    /** The handler of `owner`, which outlives it. */
    explicit handler(basic_parser<Traits, Storage>& owner) noexcept;

    /** The text begins: the document is made, or formatted in the external buffer. */
    bool on_document_begin(boost::system::error_code& ec);

    /** The text ends. */
    bool on_document_end(boost::system::error_code& ec);

    /** An object begins: an empty object is stored and its frame pushed. */
    bool on_object_begin(boost::system::error_code& ec);

    /** An object ends: its frame popped, and the object delivered when a selector matches it. */
    bool on_object_end(std::size_t n, boost::system::error_code& ec);

    /** An array begins: an empty array is stored and its frame pushed. */
    bool on_array_begin(boost::system::error_code& ec);

    /** An array ends: its frame popped, and the array delivered when a selector matches it. */
    bool on_array_end(std::size_t n, boost::system::error_code& ec);

    /** A part of a key, which the next part or `on_key` completes. */
    bool on_key_part(std::string_view s, std::size_t n, boost::system::error_code& ec);

    /** A key, completed by its last part. */
    bool on_key(std::string_view s, std::size_t n, boost::system::error_code& ec);

    /** A part of a string, which the next part or `on_string` completes. */
    bool on_string_part(std::string_view s, std::size_t n, boost::system::error_code& ec);

    /** A string, completed by its last part: stored, or revived as a date or bytes. */
    bool on_string(std::string_view s, std::size_t n, boost::system::error_code& ec);

    /** A part of a number's text, which the next part or the number's callback completes. */
    bool on_number_part(std::string_view s, boost::system::error_code& ec);

    /** An integer, whose text the handler stores by value; Boost.JSON's value is not used. */
    bool on_int64(std::int64_t i, std::string_view s, boost::system::error_code& ec);

    /** An integer, whose text the handler stores by value; Boost.JSON's value is not used. */
    bool on_uint64(std::uint64_t u, std::string_view s, boost::system::error_code& ec);

    /**
     A number, whose text the handler stores by value: Boost.JSON's interface names a `double`,
     which `number_precision::none` leaves uncomputed, and the handler never reads it.
    */
    bool on_double(double d, std::string_view s, boost::system::error_code& ec);

    /** `true` or `false`. */
    bool on_bool(bool b, boost::system::error_code& ec);

    /** `null`. */
    bool on_null(boost::system::error_code& ec);

    /** A part of a comment, which JSON refuses before the handler sees one. */
    bool on_comment_part(std::string_view s, boost::system::error_code& ec);

    /** A comment, which JSON refuses before the handler sees one. */
    bool on_comment(std::string_view s, boost::system::error_code& ec);

    /** Forgets the text read so far, for another text. */
    void reset() noexcept;

private:
    basic_parser<Traits, Storage>* owner_;
    std::vector<frame> frames_;
    std::vector<path_step> path_;
    std::string key_;
    std::string text_;
#ifndef WEBCPP_DOCUMENT_NO_EXCEPTIONS
    std::exception_ptr thrown_;
#endif
};

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_HANDLER_HPP
