// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The image format, watched: fixtures/format holds the compact image of a fixed document under
// default_traits and under js_traits, written once. Each golden image must open and write the
// expected text, and the same document built today must compact to exactly the golden bytes. The
// bytes are the layout Boost.Interprocess's rbtree_best_fit and Boost.MultiIndex's ordered indices
// give the heap and the nodes: a new Boost release that changes one of them makes the second check
// fail, and the format version must then be raised, with a migration from the previous one.
//
// The goldens are written by this program: `format_test --write <directory>` writes
// `default_traits.image` and `js_traits.image` into the directory (fixtures/format). Writing them
// again is right only together with raising `format_version`; never to make this test pass.
// The fixtures' directory is WEBCPP_TEST_DOCUMENT_FIXTURES, which test/Jamfile defines.

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>

#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wd = webcpp::document;

namespace {

// Objects, arrays, strings of every length class, numbers, nesting and keys that JavaScript puts
// in another order than they are written in: enough to exercise the heap, the nodes and all three
// indices.
constexpr std::string_view source =
    R"({"name":"format","version":1,"ratio":0.5,"big":4294967296,"negative":-7,"flag":true,)"
    R"("off":false,"nothing":null,"empty_object":{},"empty_array":[],"10":"ten","2":"two",)"
    R"("text":"a string that is longer than any small buffer a string could keep inline, so that )"
    R"(the heap hands it a block of its own","unicode":"café 世界 😀",)"
    R"("escapes":"line\nbreak \"quoted\" back\\slash","list":[1,2.5,"three",[4,[5,[6]]],)"
    R"({"seven":7,"eight":[8,{"nine":9}]},null,true],"tree":{"left":{"left":{"value":1},)"
    R"("right":{"value":2}},"right":{"left":{"value":3},"right":{"value":4}}},"many":[0,1,2,3,4,5,)"
    R"(6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31]})";

// What both traits write for the source: the keys as JavaScript orders them, integer-like keys
// first and ascending, the others as written.
constexpr std::string_view expected_text =
    R"({"2":"two","10":"ten","name":"format","version":1,"ratio":0.5,"big":4294967296,)"
    R"("negative":-7,"flag":true,"off":false,"nothing":null,"empty_object":{},"empty_array":[],)"
    R"("text":"a string that is longer than any small buffer a string could keep inline, so that )"
    R"(the heap hands it a block of its own","unicode":"café 世界 😀",)"
    R"("escapes":"line\nbreak \"quoted\" back\\slash","list":[1,2.5,"three",[4,[5,[6]]],)"
    R"({"seven":7,"eight":[8,{"nine":9}]},null,true],"tree":{"left":{"left":{"value":1},)"
    R"("right":{"value":2}},"right":{"left":{"value":3},"right":{"value":4}}},"many":[0,1,2,3,4,5,)"
    R"(6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31]})";

/** The golden image's file name for the traits. */
std::string file_name(const wd::default_traits* /*traits*/) {
    return "default_traits.image";
}

std::string file_name(const wd::js_traits* /*traits*/) {
    return "js_traits.image";
}

/** The directory of the golden images. */
std::filesystem::path golden_directory() {
    return std::filesystem::path{WEBCPP_TEST_DOCUMENT_FIXTURES} / "format";
}

/** The compact image of the source, built from its text as the library builds it today. */
template <class Traits>
std::vector<std::byte> build_image() {
    auto parsed = wd::parse<Traits>(source);
    webcpp::test::require(BOOST_TEST(parsed.has_value()));
    webcpp::test::require(BOOST_TEST(parsed->compact().has_value()));
    const std::span<const std::byte> image = parsed->image();
    return {image.begin(), image.end()};
}

/** A file's bytes; nothing is read, and the test fails, when the file does not open. */
std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!BOOST_TEST(in.is_open())) {
        std::cerr << "cannot open " << path.string() << '\n';
        return {};
    }
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

/** The offset of the first byte two images differ at; the shorter's size when one is a prefix. */
std::size_t first_difference(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    const auto [i, j] = std::ranges::mismatch(a, b);
    return static_cast<std::size_t>(i - a.begin());
}

template <class Traits>
void check() {
    const Traits* tag = nullptr;
    const std::string name = file_name(tag);
    const std::vector<std::byte> golden = read_file(golden_directory() / name);
    if (!BOOST_TEST(!golden.empty())) {
        return;
    }

    // (a) The golden image opens and says what it said when it was written.
    auto opened = wd::from_image<Traits>(golden);
    if (BOOST_TEST(opened.has_value())) {
        BOOST_TEST_EQ(opened->stringify(), std::string(expected_text));
    }

    // (b) The same document built today has exactly the golden bytes.
    const std::vector<std::byte> built = build_image<Traits>();
    const bool same = built == golden;
    BOOST_TEST(same);
    if (!same) {
        std::cerr << name << ": the image built today differs from the golden image ("
                  << built.size() << " bytes against " << golden.size()
                  << ", first difference at offset " << first_difference(built, golden)
                  << ").\nBoost.Interprocess or Boost.MultiIndex changed the layout of an image, "
                     "or the library did: raise webcpp::document::format_version, provide a "
                     "migration from the previous version, and only then write the golden images "
                     "again.\n";
    }
}

/** Writes the golden images into a directory. */
int write_goldens(const std::filesystem::path& directory) {
    const auto write = [&](const std::string& name, const std::vector<std::byte>& bytes) {
        std::ofstream out(directory / name, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        return out.good();
    };
    const wd::default_traits* d = nullptr;
    const wd::js_traits* j = nullptr;
    if (!write(file_name(d), build_image<wd::default_traits>()) ||
        !write(file_name(j), build_image<wd::js_traits>())) {
        std::cerr << "cannot write into " << directory.string() << '\n';
        return 1;
    }
    return boost::report_errors();
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() == 3 && std::string_view(arguments[1]) == "--write") {
        return write_goldens(arguments[2]);
    }
    check<wd::default_traits>();
    check<wd::js_traits>();
    return boost::report_errors();
}
