// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The selectors' grammar and matcher, apart from any parser: which texts parse and into which
// steps, which are refused with errc::type, and which paths of the text each selector matches.

#include <webcpp/document/detail/selector.hpp>

#include "require.hpp"

#include <boost/core/lightweight_test.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

using namespace std::string_view_literals;

namespace {

using wd::detail::path_step;
using wd::detail::selector;
using wd::detail::selector_step;
using wd::detail::step_kind;

// What parse makes of a text, as a word a failed check prints beside the text on its line.
std::string_view verdict(std::string_view text) {
    const wd::result<selector> parsed = selector::parse(text);
    if (parsed.has_value()) {
        return "accepted";
    }
    if (parsed.error() == wd::errc::type) {
        return "refused";
    }
    return "refused with another error";
}

// The steps of a text the grammar accepts.
std::vector<selector_step> steps_of(std::string_view text) {
    const wd::result<selector> parsed = selector::parse(text);
    webcpp::test::require(BOOST_TEST(parsed.has_value()));
    const std::span<const selector_step> steps = parsed->steps();
    return {steps.begin(), steps.end()};
}

path_step member(std::string_view key) {
    path_step step;
    step.key = key;
    return step;
}

path_step element(std::uint64_t index) {
    path_step step;
    step.index = index;
    step.is_index = true;
    return step;
}

// Whether the selector a text the grammar accepts matches a path.
bool matches(std::string_view text, const std::vector<path_step>& path) {
    const wd::result<selector> parsed = selector::parse(text);
    webcpp::test::require(BOOST_TEST(parsed.has_value()));
    return parsed->matches(path);
}

void grammar() {
    BOOST_TEST_EQ(verdict("$"), "accepted");
    BOOST_TEST_EQ(verdict("$.a"), "accepted");
    BOOST_TEST_EQ(verdict("$['a b']"), "accepted");
    BOOST_TEST_EQ(verdict(R"($['it\'s'])"), "accepted");
    BOOST_TEST_EQ(verdict("$[0]"), "accepted");
    BOOST_TEST_EQ(verdict("$[*]"), "accepted");
    BOOST_TEST_EQ(verdict("$.*"), "accepted");
    BOOST_TEST_EQ(verdict("$.items[*].id"), "accepted");
    BOOST_TEST_EQ(verdict(R"($['a\\b'])"), "accepted");
    BOOST_TEST_EQ(verdict("$.é"), "accepted");

    BOOST_TEST_EQ(verdict("a"), "refused");
    BOOST_TEST_EQ(verdict("$."), "refused");
    BOOST_TEST_EQ(verdict("$["), "refused");
    BOOST_TEST_EQ(verdict("$[-1]"), "refused");
    BOOST_TEST_EQ(verdict("$['a"), "refused");
    BOOST_TEST_EQ(verdict("$..a"), "refused");
    BOOST_TEST_EQ(verdict(R"($['a\b'])"), "refused");
    BOOST_TEST_EQ(verdict("$[00]"), "refused");
    BOOST_TEST_EQ(verdict("$[18446744073709551616]"), "refused");
}

void the_text_starts_with_the_root_and_holds_nothing_else() {
    BOOST_TEST_EQ(verdict(""), "refused");
    BOOST_TEST_EQ(verdict(" $"), "refused");
    BOOST_TEST_EQ(verdict("$ "), "refused");
    BOOST_TEST_EQ(verdict("$$"), "refused");
    BOOST_TEST_EQ(verdict("$a"), "refused");
    BOOST_TEST_EQ(verdict("$*"), "refused");
    BOOST_TEST_EQ(verdict("$.a."), "refused");
    BOOST_TEST_EQ(verdict("$.a b"), "refused");
    BOOST_TEST_EQ(verdict("$['a']x"), "refused");
    BOOST_TEST_EQ(verdict("$['a']]"), "refused");
    BOOST_TEST_EQ(verdict("$.a\0"sv), "refused");

    BOOST_TEST(steps_of("$").empty());
}

void a_name_starts_with_a_letter_an_underscore_a_dollar_or_a_byte_above_ascii() {
    BOOST_TEST_EQ(verdict("$.A"), "accepted");
    BOOST_TEST_EQ(verdict("$.z"), "accepted");
    BOOST_TEST_EQ(verdict("$._"), "accepted");
    BOOST_TEST_EQ(verdict("$.$"), "accepted");
    BOOST_TEST_EQ(verdict("$.\x80"), "accepted");
    BOOST_TEST_EQ(verdict("$.\xff"), "accepted");
    BOOST_TEST_EQ(verdict("$.a1_$Z\xc3\xa9"), "accepted");

    BOOST_TEST_EQ(verdict("$.1a"), "refused");
    BOOST_TEST_EQ(verdict("$.-"), "refused");
    BOOST_TEST_EQ(verdict("$.a-b"), "refused");
    BOOST_TEST_EQ(verdict("$.'a'"), "refused");
    BOOST_TEST_EQ(verdict("$.\x7f"), "refused");

    const std::vector<selector_step> steps = steps_of("$.a1_$Z.é");
    if (!BOOST_TEST_EQ(steps.size(), 2u)) {
        return;
    }
    BOOST_TEST(steps[0].kind == step_kind::key);
    BOOST_TEST_EQ(steps[0].key, "a1_$Z");
    BOOST_TEST(steps[1].kind == step_kind::key);
    BOOST_TEST_EQ(steps[1].key, "\xc3\xa9");
}

void a_quoted_key_holds_any_character_with_two_escapes() {
    BOOST_TEST_EQ(verdict("$['']"), "accepted");
    BOOST_TEST_EQ(verdict("$['.']"), "accepted");
    BOOST_TEST_EQ(verdict("$['[*]']"), "accepted");
    BOOST_TEST_EQ(verdict("$['\"']"), "accepted");
    BOOST_TEST_EQ(verdict("$['a\0b']"sv), "accepted");

    BOOST_TEST_EQ(verdict(R"($["a"])"), "refused");
    BOOST_TEST_EQ(verdict("$['a'"), "refused");
    BOOST_TEST_EQ(verdict(R"($['a\'])"), "refused");
    BOOST_TEST_EQ(verdict(R"($['a\)"), "refused");
    BOOST_TEST_EQ(verdict(R"($['\n'])"), "refused");
    BOOST_TEST_EQ(verdict(R"($['\"'])"), "refused");
    BOOST_TEST_EQ(verdict(R"($['\x41'])"), "refused");
    BOOST_TEST_EQ(verdict("$['\\u0041']"), "refused");
    BOOST_TEST_EQ(verdict("$['a' ]"), "refused");

    const std::vector<selector_step> steps = steps_of(R"($['it\'s']['a\\b']['']['a b']['\\\''])");
    if (!BOOST_TEST_EQ(steps.size(), 5u)) {
        return;
    }
    BOOST_TEST(steps[0].kind == step_kind::key);
    BOOST_TEST_EQ(steps[0].key, "it's");
    BOOST_TEST_EQ(steps[1].key, R"(a\b)");
    BOOST_TEST(steps[2].kind == step_kind::key);
    BOOST_TEST_EQ(steps[2].key, "");
    BOOST_TEST_EQ(steps[3].key, "a b");
    BOOST_TEST_EQ(steps[4].key, R"(\')");

    const std::vector<selector_step> zero = steps_of("$['a\0b']"sv);
    if (!BOOST_TEST_EQ(zero.size(), 1u)) {
        return;
    }
    BOOST_TEST_EQ(zero[0].key, "a\0b"sv);
}

void an_index_is_decimal_without_leading_zeros_and_fits_64_bits() {
    BOOST_TEST_EQ(verdict("$[7]"), "accepted");
    BOOST_TEST_EQ(verdict("$[10]"), "accepted");
    BOOST_TEST_EQ(verdict("$[18446744073709551615]"), "accepted");

    BOOST_TEST_EQ(verdict("$[]"), "refused");
    BOOST_TEST_EQ(verdict("$[01]"), "refused");
    BOOST_TEST_EQ(verdict("$[+1]"), "refused");
    BOOST_TEST_EQ(verdict("$[1.5]"), "refused");
    BOOST_TEST_EQ(verdict("$[1e3]"), "refused");
    BOOST_TEST_EQ(verdict("$[ 1]"), "refused");
    BOOST_TEST_EQ(verdict("$[1 ]"), "refused");
    BOOST_TEST_EQ(verdict("$[1"), "refused");
    BOOST_TEST_EQ(verdict("$[99999999999999999999]"), "refused");
    BOOST_TEST_EQ(verdict("$[184467440737095516150]"), "refused");

    const std::vector<selector_step> steps = steps_of("$[0][42][18446744073709551615]");
    if (!BOOST_TEST_EQ(steps.size(), 3u)) {
        return;
    }
    BOOST_TEST(steps[0].kind == step_kind::index);
    BOOST_TEST_EQ(steps[0].index, 0u);
    BOOST_TEST(steps[1].kind == step_kind::index);
    BOOST_TEST_EQ(steps[1].index, 42u);
    BOOST_TEST(steps[2].kind == step_kind::index);
    BOOST_TEST_EQ(steps[2].index, std::numeric_limits<std::uint64_t>::max());
}

void wildcards_are_brackets_star_and_dot_star() {
    BOOST_TEST_EQ(verdict("$.*.*"), "accepted");
    BOOST_TEST_EQ(verdict("$[*][*]"), "accepted");

    BOOST_TEST_EQ(verdict("$[*"), "refused");
    BOOST_TEST_EQ(verdict("$[**]"), "refused");
    BOOST_TEST_EQ(verdict("$.*a"), "refused");
    BOOST_TEST_EQ(verdict("$.**"), "refused");
    BOOST_TEST_EQ(verdict("$['*'"), "refused");

    const std::vector<selector_step> steps = steps_of("$.items[*].*['*']");
    if (!BOOST_TEST_EQ(steps.size(), 4u)) {
        return;
    }
    BOOST_TEST(steps[0].kind == step_kind::key);
    BOOST_TEST_EQ(steps[0].key, "items");
    BOOST_TEST(steps[1].kind == step_kind::any_element);
    BOOST_TEST(steps[2].kind == step_kind::any_member);
    BOOST_TEST(steps[3].kind == step_kind::key);
    BOOST_TEST_EQ(steps[3].key, "*");
}

void matching() {
    BOOST_TEST(matches("$", {}));
    BOOST_TEST(!matches("$", {member("a")}));
    BOOST_TEST(!matches("$", {element(0)}));

    BOOST_TEST(matches("$.items[*].id", {member("items"), element(3), member("id")}));
    BOOST_TEST(!matches("$.items[*].id", {member("items"), member("id"), member("id")}));
    BOOST_TEST(!matches("$.items[*].id", {member("items"), element(3), member("ids")}));
    BOOST_TEST(!matches("$.items[*].id", {member("item"), element(3), member("id")}));

    BOOST_TEST(matches("$.*", {member("a")}));
    BOOST_TEST(matches("$.*", {member("")}));
    BOOST_TEST(!matches("$.*", {element(0)}));
    BOOST_TEST(matches("$[*]", {element(0)}));
    BOOST_TEST(matches("$[*]", {element(std::numeric_limits<std::uint64_t>::max())}));
    BOOST_TEST(!matches("$[*]", {member("a")}));

    BOOST_TEST(matches("$['a b']", {member("a b")}));
    BOOST_TEST(!matches("$['a b']", {member("a")}));

    BOOST_TEST(matches("$[3]", {element(3)}));
    BOOST_TEST(!matches("$[3]", {element(4)}));
    BOOST_TEST(!matches("$[3]", {member("3")}));
    BOOST_TEST(!matches("$['3']", {element(3)}));
    BOOST_TEST(matches("$['3']", {member("3")}));
    BOOST_TEST(!matches("$['']", {element(0)}));
    BOOST_TEST(matches("$['']", {member("")}));
}

void the_escapes_of_a_quoted_key_match_the_key_they_stand_for() {
    BOOST_TEST(matches(R"($['it\'s'])", {member("it's")}));
    BOOST_TEST(!matches(R"($['it\'s'])", {member(R"(it\'s)")}));
    BOOST_TEST(matches(R"($['a\\b'])", {member(R"(a\b)")}));
    BOOST_TEST(!matches(R"($['a\\b'])", {member(R"(a\\b)")}));
    BOOST_TEST(matches("$.é", {member("é")}));
    BOOST_TEST(matches("$['é']", {member("é")}));
}

void a_key_may_hold_any_byte() {
    BOOST_TEST(matches("$['a\0b']"sv, {member("a\0b"sv)}));
    BOOST_TEST(!matches("$['a\0b']"sv, {member("a")}));
    BOOST_TEST(!matches("$['a\0b']"sv, {member("a\0c"sv)}));
    BOOST_TEST(!matches("$['a']"sv, {member("a\0"sv)}));
    BOOST_TEST(matches("$['\0']"sv, {member("\0"sv)}));
    BOOST_TEST(!matches("$['']"sv, {member("\0"sv)}));
    BOOST_TEST(matches("$.*", {member("\0\xff\n"sv)}));
    BOOST_TEST(matches("$['\n\x01\xff']", {member("\n\x01\xff")}));
}

void a_path_of_another_length_does_not_match() {
    BOOST_TEST(!matches("$.a", {}));
    BOOST_TEST(!matches("$.a", {member("a"), member("a")}));
    BOOST_TEST(!matches("$.*", {}));
    BOOST_TEST(!matches("$.*", {member("a"), member("b")}));
    BOOST_TEST(!matches("$[*]", {element(0), element(1)}));
    BOOST_TEST(!matches("$[*][*]", {element(0)}));
    BOOST_TEST(!matches("$.a.b", {member("a")}));
    BOOST_TEST(!matches("$.items[*]", {member("items"), element(0), member("id")}));
}

}  // namespace

int main() {
    grammar();
    the_text_starts_with_the_root_and_holds_nothing_else();
    a_name_starts_with_a_letter_an_underscore_a_dollar_or_a_byte_above_ascii();
    a_quoted_key_holds_any_character_with_two_escapes();
    an_index_is_decimal_without_leading_zeros_and_fits_64_bits();
    wildcards_are_brackets_star_and_dot_star();
    matching();
    the_escapes_of_a_quoted_key_match_the_key_they_stand_for();
    a_key_may_hold_any_byte();
    a_path_of_another_length_does_not_match();
    return boost::report_errors();
}
