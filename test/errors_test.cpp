// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The errors and the kinds, whose values are fixed. test/Jamfile builds this file twice: as the
// build asks (errors), and with exceptions, whatever the build asks, where
// WEBCPP_TEST_DOCUMENT_RAISE also checks the exception detail::raise throws for each errc
// (errors_raise).

#include <webcpp/document/errors.hpp>
#include <webcpp/document/kind.hpp>

#include <boost/core/lightweight_test.hpp>

#include <array>
#include <string>
#include <string_view>

namespace wd = webcpp::document;

namespace {

void values_are_fixed() {
    BOOST_TEST_EQ(static_cast<int>(wd::errc::syntax), 1);
    BOOST_TEST_EQ(static_cast<int>(wd::errc::type), 2);
    BOOST_TEST_EQ(static_cast<int>(wd::errc::range), 3);
    BOOST_TEST_EQ(static_cast<int>(wd::errc::capacity), 4);
    BOOST_TEST_EQ(static_cast<int>(wd::errc::not_a_document), 5);
    BOOST_TEST_EQ(static_cast<int>(wd::errc::incompatible_document), 6);
    BOOST_TEST_EQ(static_cast<int>(wd::errc::damaged_document), 7);
}

void the_category_is_webcpp_document() {
    BOOST_TEST_EQ(std::string(wd::error_category().name()), "webcpp.document");

    const boost::system::error_code code = wd::errc::capacity;
    BOOST_TEST(code.category() == wd::error_category());
    BOOST_TEST_EQ(code.message(), "no room left");

    BOOST_TEST_EQ(boost::system::error_code(wd::errc::syntax).message(), "malformed JSON text");
    BOOST_TEST_EQ(boost::system::error_code(wd::errc::type).message(), "a value of another kind");
    BOOST_TEST_EQ(boost::system::error_code(wd::errc::range).message(),
                  "a number or index out of range");
    BOOST_TEST_EQ(boost::system::error_code(wd::errc::not_a_document).message(),
                  "not a document's image");
    BOOST_TEST_EQ(boost::system::error_code(wd::errc::incompatible_document).message(),
                  "an image of another format, layout, byte order or traits");
    BOOST_TEST_EQ(boost::system::error_code(wd::errc::damaged_document).message(),
                  "a sealed image whose bytes do not match its seal");

    BOOST_TEST_EQ(wd::error_category().message(0), "unknown webcpp.document error");
    BOOST_TEST_EQ(wd::error_category().message(8), "unknown webcpp.document error");
}

/**
 The message copied into a caller's buffer: cut to fit with its terminating zero, never written
 past the size given, and returned whole whatever the buffer.
*/
void a_message_fits_the_buffer_it_is_given() {
    const boost::system::error_category& category = wd::error_category();
    const int value = static_cast<int>(wd::errc::capacity);
    constexpr std::string_view whole = "no room left";
    std::array<char, whole.size() + 2> buffer{};

    BOOST_TEST_EQ(std::string_view(category.message(value, nullptr, 8)), whole);

    buffer.fill('x');
    BOOST_TEST_EQ(std::string_view(category.message(value, buffer.data(), 0)), whole);
    BOOST_TEST_EQ(buffer[0], 'x');

    buffer.fill('x');
    BOOST_TEST_EQ(std::string_view(category.message(value, buffer.data(), 1)), whole);
    BOOST_TEST_EQ(buffer[0], '\0');
    BOOST_TEST_EQ(buffer[1], 'x');

    buffer.fill('x');
    BOOST_TEST_EQ(std::string_view(category.message(value, buffer.data(), 3)), whole);
    BOOST_TEST_EQ(std::string_view(buffer.data()), "no");
    BOOST_TEST_EQ(buffer[3], 'x');

    buffer.fill('x');
    BOOST_TEST_EQ(std::string_view(category.message(value, buffer.data(), whole.size() + 1)),
                  whole);
    BOOST_TEST_EQ(std::string_view(buffer.data()), whole);
    BOOST_TEST_EQ(buffer[whole.size() + 1], 'x');
}

void a_result_holds_an_error() {
    const wd::result<int> r = wd::errc::range;
    BOOST_TEST(r.has_error());
    BOOST_TEST(r.error() == wd::errc::range);
}

void kinds_are_fixed() {
    BOOST_TEST_EQ(static_cast<int>(wd::kind::undefined), 0);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::null), 1);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::boolean), 2);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::number), 3);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::string), 4);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::date), 5);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::bytes), 6);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::array), 7);
    BOOST_TEST_EQ(static_cast<int>(wd::kind::object), 8);
}

#ifdef WEBCPP_TEST_DOCUMENT_RAISE
// detail::raise throws the exception of each errc, carrying the code and, for a syntax error, the
// byte offset.
void raise_throws_the_type_of_its_code() {
    const auto thrown = [](wd::errc e) -> std::string {
        try {
            wd::detail::raise(e, 17);
        } catch (const wd::syntax_error& x) {
            return "syntax " + std::to_string(x.offset()) + " " + x.code().message();
        } catch (const wd::type_error& x) {
            return "type " + x.code().message();
        } catch (const wd::range_error& x) {
            return "range " + x.code().message();
        } catch (const wd::capacity_error& x) {
            return "capacity " + x.code().message();
        } catch (const wd::image_error& x) {
            return "image " + x.code().message();
        }
        // raise does not return: a statement here would be unreachable code, which MSVC refuses.
    };
    BOOST_TEST_EQ(thrown(wd::errc::syntax), "syntax 17 malformed JSON text");
    BOOST_TEST_EQ(thrown(wd::errc::type), "type a value of another kind");
    BOOST_TEST_EQ(thrown(wd::errc::range), "range a number or index out of range");
    BOOST_TEST_EQ(thrown(wd::errc::capacity), "capacity no room left");
    BOOST_TEST_EQ(thrown(wd::errc::not_a_document), "image not a document's image");
    BOOST_TEST_EQ(thrown(wd::errc::incompatible_document),
                  "image an image of another format, layout, byte order or traits");
    BOOST_TEST_EQ(thrown(wd::errc::damaged_document),
                  "image a sealed image whose bytes do not match its seal");
}
#endif

}  // namespace

int main() {
    values_are_fixed();
    the_category_is_webcpp_document();
    a_message_fits_the_buffer_it_is_given();
    a_result_holds_an_error();
    kinds_are_fixed();
#ifdef WEBCPP_TEST_DOCUMENT_RAISE
    raise_throws_the_type_of_its_code();
#endif
    return boost::report_errors();
}
