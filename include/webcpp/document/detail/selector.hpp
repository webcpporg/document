// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The selectors of the streaming parser: a subset of JSONPath, parsed into steps, and matched
 against the path of a value in the text, which knows nothing of the document being built.

 The grammar: `$`, then any sequence of `.name`, `['name']` (any key, with the escapes `\'` and
 `\\`), `[n]` (an index, n >= 0, without leading zeros), `[*]` (every element of an array) and `.*`
 (every member of an object). A `name` after a dot is a letter, `_` or `$`, then letters, digits,
 `_` or `$`, and any byte of 0x80 or more, so that a name in UTF-8 needs no quotes.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_SELECTOR_HPP
#define WEBCPP_DOCUMENT_DETAIL_SELECTOR_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/errors.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::document::detail {

/** One step of a value's path in the text: a member's key, in UTF-8, or an element's index. */
struct path_step {
    /** The member's key, in UTF-8; empty for an element. */
    std::string key;

    /** The element's index in the text; 0 for a member. */
    std::uint64_t index = 0;

    /** Whether the step is an element's. */
    bool is_index = false;
};

/** What a step of a selector matches. */
enum class step_kind : std::uint8_t {
    /** A member of this key. */
    key,

    /** An element of this index. */
    index,

    /** Every element of an array, `[*]`. */
    any_element,

    /** Every member of an object, `.*`. */
    any_member,
};

/** One step of a selector. */
struct selector_step {
    /** What the step matches. */
    step_kind kind = step_kind::key;

    /** The key of a @ref step_kind::key step, unescaped, in UTF-8. */
    std::string key;

    /** The index of a @ref step_kind::index step. */
    std::uint64_t index = 0;
};

/** A parsed selector: the steps after `$`, which match a path of the same length step by step. */
class selector {
public:
    /** Parses a selector's text; @ref errc::type for text outside the grammar. */
    static result<selector> parse(std::string_view text) noexcept;

    /** Whether a path matches: as many steps as the selector, each matched by its step. */
    [[nodiscard]] bool matches(std::span<const path_step> path) const noexcept;

    /** The steps after `$`; none for `$` alone, which matches the root. */
    [[nodiscard]] std::span<const selector_step> steps() const noexcept;

private:
    std::vector<selector_step> steps_;
};

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_SELECTOR_HPP
