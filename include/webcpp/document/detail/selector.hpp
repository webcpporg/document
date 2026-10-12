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

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    // The steps are held on the process heap: a failed allocation there ends the program, as
    // this function's noexcept, which the parser's on and observe rely on, intends.
    /** Parses a selector's text; @ref errc::type for text outside the grammar. */
    // NOLINTNEXTLINE(bugprone-exception-escape)
    static result<selector> parse(std::string_view text) noexcept {
        if (text.empty() || text.front() != '$') {
            return errc::type;
        }
        selector parsed;
        std::string_view rest = text.substr(1);
        while (!rest.empty()) {
            std::optional<selector_step> step = read_step(rest);
            if (!step) {
                return errc::type;
            }
            parsed.steps_.push_back(std::move(*step));
        }
        return parsed;
    }

    /** Whether a path matches: as many steps as the selector, each matched by its step. */
    [[nodiscard]] bool matches(std::span<const path_step> path) const noexcept {
        if (path.size() != steps_.size()) {
            return false;
        }
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (!step_matches(steps_[i], path[i])) {
                return false;
            }
        }
        return true;
    }

    /** The steps after `$`; none for `$` alone, which matches the root. */
    [[nodiscard]] std::span<const selector_step> steps() const noexcept { return steps_; }

private:
    /** Whether one step of a selector matches one step of a path. */
    static bool step_matches(const selector_step& step, const path_step& at) noexcept {
        switch (step.kind) {
            case step_kind::key: return !at.is_index && at.key == step.key;
            case step_kind::index: return at.is_index && at.index == step.index;
            case step_kind::any_element: return at.is_index;
            case step_kind::any_member: return !at.is_index;
        }
        return false;
    }

    /** Whether a byte may start a name after a dot: a letter, `_`, `$`, or a byte from 0x80. */
    static bool starts_name(char c) noexcept {
        const auto byte = static_cast<unsigned char>(c);
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_' ||
               byte == '$' || byte >= 0x80;
    }

    /** Whether a byte may go on a name after a dot: one that may start it, or a digit. */
    static bool continues_name(char c) noexcept { return starts_name(c) || (c >= '0' && c <= '9'); }

    /** Reads the step `rest` starts with, at its `.` or `[`, and drops it from `rest`. */
    static std::optional<selector_step> read_step(std::string_view& rest) {
        const char opening = rest.front();
        rest.remove_prefix(1);
        if (opening == '.') {
            return read_dotted(rest);
        }
        if (opening == '[') {
            return read_bracketed(rest);
        }
        return std::nullopt;
    }

    /** Reads what follows a dot, `*` or a name, and drops it from `rest`. */
    static std::optional<selector_step> read_dotted(std::string_view& rest) {
        if (rest.empty()) {
            return std::nullopt;
        }
        selector_step step;
        if (rest.front() == '*') {
            rest.remove_prefix(1);
            step.kind = step_kind::any_member;
            return step;
        }
        if (!starts_name(rest.front())) {
            return std::nullopt;
        }
        std::size_t length = 1;
        while (length < rest.size() && continues_name(rest[length])) {
            ++length;
        }
        step.key = rest.substr(0, length);
        rest.remove_prefix(length);
        return step;
    }

    /** Reads `*`, a quoted key or an index, then `]`, and drops them from `rest`. */
    static std::optional<selector_step> read_bracketed(std::string_view& rest) {
        if (rest.empty()) {
            return std::nullopt;
        }
        selector_step step;
        if (rest.front() == '*') {
            rest.remove_prefix(1);
            step.kind = step_kind::any_element;
        } else if (rest.front() == '\'') {
            rest.remove_prefix(1);
            std::optional<std::string> key = read_quoted(rest);
            if (!key) {
                return std::nullopt;
            }
            step.key = std::move(*key);
        } else {
            const std::optional<std::uint64_t> index = read_index(rest);
            if (!index) {
                return std::nullopt;
            }
            step.kind = step_kind::index;
            step.index = *index;
        }
        if (rest.empty() || rest.front() != ']') {
            return std::nullopt;
        }
        rest.remove_prefix(1);
        return step;
    }

    /**
     Reads a quoted key's characters through its closing quote, unescaped, and drops them from
     `rest`.

     `\'` and `\\` are the only escapes, so that a backslash before anything else, which another
     reader might take for an escape of its own, is refused rather than guessed.
    */
    static std::optional<std::string> read_quoted(std::string_view& rest) {
        std::string key;
        while (!rest.empty()) {
            const char c = rest.front();
            rest.remove_prefix(1);
            if (c == '\'') {
                return key;
            }
            if (c != '\\') {
                key.push_back(c);
                continue;
            }
            if (rest.empty() || (rest.front() != '\'' && rest.front() != '\\')) {
                return std::nullopt;
            }
            key.push_back(rest.front());
            rest.remove_prefix(1);
        }
        return std::nullopt;
    }

    /** Reads an index's decimal digits, without a leading zero but `0` itself, from `rest`. */
    static std::optional<std::uint64_t> read_index(std::string_view& rest) noexcept {
        if (rest.empty() || rest.front() < '0' || rest.front() > '9') {
            return std::nullopt;
        }
        if (rest.front() == '0' && rest.size() > 1 && rest[1] >= '0' && rest[1] <= '9') {
            return std::nullopt;
        }
        std::uint64_t index = 0;
        const char* const first = std::to_address(rest.begin());
        const std::from_chars_result read =
            std::from_chars(first, std::to_address(rest.end()), index);
        if (read.ec != std::errc{}) {
            return std::nullopt;
        }
        rest.remove_prefix(static_cast<std::size_t>(read.ptr - first));
        return index;
    }

    /** The steps after `$`, in order. */
    std::vector<selector_step> steps_;
};

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_SELECTOR_HPP
