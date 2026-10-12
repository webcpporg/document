// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The texts the streaming and the selectors' tests split at every byte: JSON.parse's accepted
// rows, multi-byte characters raw and escaped, escapes, numbers, literals, keys of every length
// from 0 to 40, keys that look like array indices, nesting, and repeated keys.

#ifndef WEBCPP_TEST_DOCUMENT_PARSE_CORPUS_HPP
#define WEBCPP_TEST_DOCUMENT_PARSE_CORPUS_HPP

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::test {

// An object whose keys have every length from 0 to 40, "", "a", "aa", ..., each holding its length.
inline std::string keys_of_every_length() {
    std::string text = "{";
    for (std::size_t length = 0; length <= 40; ++length) {
        if (length != 0) {
            text += ',';
        }
        text += '"';
        text.append(length, 'a');
        text += "\":";
        text += std::to_string(length);
    }
    text += '}';
    return text;
}

// A string of 300 characters, escapes and multi-byte characters among plain ones.
inline std::string a_long_string() {
    std::string text = "\"";
    for (std::size_t i = 0; i < 30; ++i) {
        text += "abc\\n\\u00e9\xc3\xa9\xe2\x82\xac";
    }
    text += '"';
    return text;
}

// The corpus, 40 texts, each accepted by JSON.parse.
inline std::vector<std::string> parse_corpus() {
    constexpr std::array<std::string_view, 38> fixed{
        " [1 , 2] ",
        R"({"b":1,"2":2,"a":3,"1":4})",
        R"({"a":1,"b":2,"a":3})",
        R"({"a":[1],"a":{"x":1}})",
        R"("é😀")",
        "-0",
        "1.0",
        "1e400",
        R"(" ")",
        " \t\n\r1 \t\n\r",
        R"("ሴ")",
        "12.5e3",
        "true",
        "false",
        "null",
        "[true,false,null]",
        "\"h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80\"",
        R"({"é":"😀","k\"ey":"\\\/\b\f\n\r\t"})",
        "[-1.5e-3,0.1,1234567890123456789012345678901234,9223372036854775808,"
        "-9223372036854775809]",
        R"({"":0,"a":{"b":{"c":[1,[2,[3]]]}}})",
        R"([[],{},[{}],{"x":[]}])",
        R"({"a\u0000b":1})",
        "[0,-0,0e0,1E400,1e-400,0.0000001,1e21]",
        R"({"01":1,"-0":2,"1e2":3,"4294967295":4,"4294967294":5,"":6,"0":7})",
        "[[[[[[[[[[1]]]]]]]]]]",
        R"({"items":[{"id":1,"tags":["a","b"]},{"id":2,"tags":[]}],"meta":{"n":2}})",
        R"([1,"x",true,null])",
        R"("\"\\\/\b\f\n\r\t")",
        R"({"a":1,"b":2,"a":{"c":3}})",
        "[1e-7,1.5e+300,-0.0]",
        R"({"date":"2026-10-12T03:04:05.678Z","b64":"Zm9v"})",
        R"("😀😀")",
        "123456789",
        "-12345.678e-9",
        R"( { "spaced" : [ 1 , 2 , { "x" : null } ] } )",
        R"([""])",
        R"({"x":"é€𝄞"})",
        R"([[1,2],[3,4],{"a":[5,6]}])",
    };
    std::vector<std::string> corpus(fixed.begin(), fixed.end());
    corpus.push_back(keys_of_every_length());
    corpus.push_back(a_long_string());
    return corpus;
}

}  // namespace webcpp::test

#endif  // WEBCPP_TEST_DOCUMENT_PARSE_CORPUS_HPP
