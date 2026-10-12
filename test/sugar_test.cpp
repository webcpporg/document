// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The sugar that throws, built with exceptions: assignment to a handle stores as set does and
// never rebinds it, so std::swap of two handles writes through them, as, as_string and as_bool
// read as get does, a default-constructed document
// holds null, and a copy is equal and independent. Each member that fails throws the exception of
// the code the core function returns for the same call, and changes nothing.

#include <webcpp/document/document.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>

#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

using js_document = wd::basic_document<wd::js_traits>;

constexpr std::size_t kibibyte = 1024;

// A document holding `v`, made and set through the core.
template <class T>
wd::document document_of(const T& v) {
    auto made = wd::document::create();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    webcpp::test::require(BOOST_TEST(made->root().set(v).has_value()));
    return std::move(*made);
}

// Whether `call` throws `Error` holding `expected`, the code the core returned for the same call.
template <class Error, class Call>
bool raises(const Call& call, boost::system::error_code expected) {
    try {
        call();
    } catch (const Error& e) {
        return e.code() == expected;
    } catch (const wd::error&) {
        return false;
    }
    return false;
}

// A buffer of `size` bytes at a 16-aligned address, as a browser's memory would be.
class aligned_buffer {
public:
    explicit aligned_buffer(std::size_t size) : raw_(size + 16) {
        void* p = raw_.data();
        std::size_t space = raw_.size();
        std::align(16, size, p, space);
        bytes_ = {static_cast<std::byte*>(p), size};
    }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::vector<std::byte> raw_;
    std::span<std::byte> bytes_;
};

void assignment_stores_as_set_does() {
    wd::document doc = document_of(wd::null);
    doc.root() = wd::object{};
    doc["user"] = wd::object{};
    doc["user"]["age"] = 30;
    doc["name"] = "Ana";
    doc["admin"] = true;
    doc["gone"] = 1;
    doc["gone"] = wd::undefined;
    BOOST_TEST_EQ(doc["user"]["age"].as<std::int32_t>(), 30);
    BOOST_TEST_EQ(doc["name"].as_string(), "Ana");
    BOOST_TEST_EQ(doc["name"].as<std::string>(), "Ana");
    BOOST_TEST(doc["admin"].as_bool());
    BOOST_TEST(doc == document_of(wd::object{
                          {"user", wd::object{{"age", 30}}}, {"name", "Ana"}, {"admin", true}}));
}

void assignment_through_nothing_throws_type_error() {
    wd::document doc = document_of(wd::object{{"a", 1}});
    const wd::document before = document_of(wd::object{{"a", 1}});
    const auto core = doc["x"]["y"].set(1);
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::type);
    BOOST_TEST(raises<wd::type_error>([&] { doc["x"]["y"] = 1; }, core.error()));
    BOOST_TEST(raises<wd::type_error>([&] { doc["x"]["y"] = doc["a"]; }, core.error()));
    BOOST_TEST(doc == before);
}

void as_throws_range_error_when_it_does_not_fit() {
    wd::document doc = document_of(wd::object{});
    doc["n"] = 300;
    const auto core = doc["n"].get<std::int8_t>();
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::range);
    BOOST_TEST(raises<wd::range_error>([&] { (void)doc["n"].as<std::int8_t>(); }, core.error()));
    BOOST_TEST_EQ(doc["n"].as<std::int16_t>(), 300);
}

void as_throws_type_error_for_another_kind() {
    wd::document doc = document_of(wd::object{{"n", 1}, {"s", "text"}});
    const auto core = doc["n"].get<std::string>();
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::type);
    BOOST_TEST(raises<wd::type_error>([&] { (void)doc["n"].as<std::string>(); }, core.error()));
    BOOST_TEST(raises<wd::type_error>([&] { (void)doc["s"].as<std::int64_t>(); },
                                      doc["s"].get<std::int64_t>().error()));
    BOOST_TEST(raises<wd::type_error>([&] { (void)doc["absent"].as<std::int64_t>(); },
                                      doc["absent"].get<std::int64_t>().error()));
}

void as_string_of_a_number_throws_type_error() {
    wd::document doc = document_of(wd::object{{"n", 1}});
    const auto core = doc["n"].get<std::string_view>();
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::type);
    BOOST_TEST(raises<wd::type_error>([&] { (void)doc["n"].as_string(); }, core.error()));
}

// A view cannot transcode: with UTF-16 strings, as_string throws as get<std::string_view> fails,
// and the string reads as a std::u16string_view or a std::string.
void as_string_reads_utf8_strings_only() {
    auto made = js_document::create();
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    js_document doc = std::move(*made);
    doc.root() = "Ana";
    const auto core = doc.root().get<std::string_view>();
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::type);
    BOOST_TEST(raises<wd::type_error>([&] { (void)doc.root().as_string(); }, core.error()));
    BOOST_TEST(doc.root().as<std::u16string_view>() == u"Ana");
    BOOST_TEST_EQ(doc.root().as<std::string>(), "Ana");
}

void as_bool_of_another_kind_throws_type_error() {
    wd::document doc = document_of(wd::object{{"s", "true"}, {"no", false}});
    const auto core = doc["s"].get<bool>();
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::type);
    BOOST_TEST(raises<wd::type_error>([&] { (void)doc["s"].as_bool(); }, core.error()));
    BOOST_TEST(!doc["no"].as_bool());
}

// `h = other` writes a deep copy of other's value at h's place, and h still names its place.
void assignment_never_rebinds() {
    wd::document doc = document_of(wd::object{{"a", 1}, {"b", wd::object{{"x", 2}}}});
    wd::value h = doc["a"];
    const wd::value other = doc["b"];
    h = other;
    BOOST_TEST(doc["a"] == doc["b"]);
    doc["b"]["x"] = 3;
    BOOST_TEST_EQ(doc["a"]["x"].as<std::int64_t>(), 2);
    BOOST_TEST_EQ(h["x"].as<std::int64_t>(), 2);
    h = 5;
    BOOST_TEST_EQ(doc["a"].as<std::int64_t>(), 5);
    BOOST_TEST_EQ(doc["b"]["x"].as<std::int64_t>(), 3);

    // Between temporaries too: `doc["c"] = doc["b"]` stores, it does not rebind a temporary.
    doc["c"] = doc["b"];
    BOOST_TEST(doc["c"] == doc["b"]);
    doc["b"]["x"] = 4;
    BOOST_TEST_EQ(doc["c"]["x"].as<std::int64_t>(), 3);

    // A handle assigned itself writes its own value back, as `o.b = o.b` does.
    wd::value b = doc["b"];
    const wd::value& same = b;
    b = same;
    BOOST_TEST_EQ(doc["b"]["x"].as<std::int64_t>(), 4);
    BOOST_TEST_EQ(doc.root().size(), 3U);
}

// Handles are not regular values: std::swap assigns through them, so both places end up holding
// the second value, and the handles still name their own places. std::sort would write likewise.
void swap_writes_through_handles() {
    wd::document doc = document_of(wd::object{{"a", 1}, {"b", 2}});
    wd::value x = doc["a"];
    wd::value y = doc["b"];
    std::swap(x, y);
    BOOST_TEST_EQ(wd::stringify(doc), R"({"a":2,"b":2})");
    BOOST_TEST(x == doc["a"]);
    BOOST_TEST_EQ(x.as<std::int64_t>(), 2);
    BOOST_TEST_EQ(y.as<std::int64_t>(), 2);
}

// An external document's buffer cannot grow: the assignment throws capacity_error and changes
// nothing.
void an_external_document_throws_capacity_error() {
    const aligned_buffer buffer(4 * kibibyte);
    auto made = wd::format(buffer.bytes());
    if (!BOOST_TEST(made.has_value())) {
        return;
    }
    auto& doc = *made;
    doc.root() = wd::object{{"a", 1}};
    const std::string big(10 * kibibyte, 'b');
    const auto core = doc["big"].set(big);
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::capacity);
    BOOST_TEST(raises<wd::capacity_error>([&] { doc["big"] = big; }, core.error()));
    BOOST_TEST(!doc.root().contains("big"));
    BOOST_TEST_EQ(doc["a"].as<std::int64_t>(), 1);
}

void a_default_constructed_document_holds_null() {
    const wd::document doc;
    BOOST_TEST(doc.root().is_null());
    BOOST_TEST(doc == document_of(wd::null));
    const js_document js;
    BOOST_TEST(js.root().is_null());

    wd::document written;
    written.root() = wd::object{{"a", 1}};
    BOOST_TEST_EQ(written["a"].as<std::int64_t>(), 1);
}

void a_copy_is_equal_and_independent() {
    wd::document doc = document_of(wd::object{{"a", wd::array{1, 2}}, {"b", "text"}});
    wd::document copy(doc);
    BOOST_TEST(copy == doc);
    copy["a"][0] = 9;
    copy["c"] = true;
    BOOST_TEST_EQ(doc["a"][0].as<std::int64_t>(), 1);
    BOOST_TEST(!doc.root().contains("c"));
    doc["b"] = "changed";
    BOOST_TEST_EQ(copy["b"].as_string(), "text");
}

// A moved-from document holds nothing, which clone refuses: so does the copy constructor.
void a_copy_of_a_moved_from_document_throws_type_error() {
    wd::document doc = document_of(wd::object{{"a", 1}});
    const wd::document taken = std::move(doc);
    BOOST_TEST_EQ(taken["a"].as<std::int64_t>(), 1);
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the state it leaves
    const auto core = doc.clone();
    if (!BOOST_TEST(!core.has_value())) {
        return;
    }
    BOOST_TEST(core.error() == wd::errc::type);
    BOOST_TEST(raises<wd::type_error>([&] { (void)wd::document(doc); }, core.error()));
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
}

}  // namespace

int main() {
    assignment_stores_as_set_does();
    assignment_through_nothing_throws_type_error();
    as_throws_range_error_when_it_does_not_fit();
    as_throws_type_error_for_another_kind();
    as_string_of_a_number_throws_type_error();
    as_string_reads_utf8_strings_only();
    as_bool_of_another_kind_throws_type_error();
    assignment_never_rebinds();
    swap_writes_through_handles();
    an_external_document_throws_capacity_error();
    a_default_constructed_document_holds_null();
    a_copy_is_equal_and_independent();
    a_copy_of_a_moved_from_document_throws_type_error();
    return boost::report_errors();
}
