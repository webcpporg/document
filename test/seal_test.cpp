// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Sealed images: the seal is the marker and the CRC-32C of the image, its own eight bytes zero;
// a sealed image moved to another address opens there, owned or in place; a byte changed anywhere
// in it, or a single bit of its header, is refused as damaged; verify::none opens it anyway; every
// kind of change clears the seal and a change that fails keeps it; a copy keeps it; and the image
// reader refuses a damaged image of a stream.

#include <webcpp/document/document.hpp>
#include <webcpp/document/image_stream.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/crc.hpp>

#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wd = webcpp::document;

namespace {

// The seal's place in the header and its marker.
constexpr std::size_t seal_at = 48;
constexpr std::uint64_t sealed_marker = std::uint64_t{1} << 63U;

// CRC-32C, the Castagnoli polynomial, reflected: the parameters the seal is pinned to.
using crc32c = boost::crc_optimal<32, 0x1EDC6F41, 0xFFFFFFFF, 0xFFFFFFFF, true, true>;

// Exactly `size` bytes at a 16-aligned address, so that a sanitizer sees a read past them.
class exact_buffer {
public:
    explicit exact_buffer(std::span<const std::byte> bytes, std::size_t extra = 0)
        : bytes_(
              static_cast<std::byte*>(::operator new(bytes.size() + extra, std::align_val_t{16})),
              bytes.size() + extra) {
        std::ranges::fill(bytes_, std::byte{0});
        std::ranges::copy(bytes, bytes_.begin());
    }

    exact_buffer(const exact_buffer&) = delete;
    exact_buffer& operator=(const exact_buffer&) = delete;
    exact_buffer(exact_buffer&&) = delete;
    exact_buffer& operator=(exact_buffer&&) = delete;

    ~exact_buffer() { ::operator delete(bytes_.data(), std::align_val_t{16}); }

    [[nodiscard]] std::span<std::byte> bytes() const { return bytes_; }

private:
    std::span<std::byte> bytes_;
};

// A document, shrunk to its content unless it is to keep room for changes in place.
wd::document make_document(bool shrink = true) {
    auto parsed = wd::parse(R"({"name":"sealed","list":[1,2,3,4],"nested":{"k":"value"}})");
    webcpp::test::require(BOOST_TEST(parsed.has_value()));
    if (shrink) {
        webcpp::test::require(BOOST_TEST(parsed->shrink_to_fit().has_value()));
    }
    return std::move(*parsed);
}

// The seal as the header holds it.
std::uint64_t seal_of(std::span<const std::byte> image) {
    std::uint64_t seal = 0;
    std::memcpy(&seal, image.data() + seal_at, sizeof seal);
    return seal;
}

// The CRC-32C of an image with its seal's eight bytes taken as zero, computed here independently.
std::uint32_t expected_crc(std::span<const std::byte> image) {
    std::vector<std::byte> copy(image.begin(), image.end());
    std::fill_n(copy.begin() + seal_at, 8, std::byte{0});
    crc32c crc;
    crc.process_bytes(copy.data(), copy.size());
    return crc.checksum();
}

// Whether a document's image carries no seal: its eight bytes all zero.
template <class Document>
bool unsealed(const Document& doc) {
    return !doc.sealed() && seal_of(doc.image()) == 0;
}

void the_checksum_is_crc32c() {
    // The check value of CRC-32C, which pins the parameters above to the Castagnoli CRC.
    constexpr std::string_view check = "123456789";
    crc32c crc;
    crc.process_bytes(check.data(), check.size());
    BOOST_TEST_EQ(crc.checksum(), 0xE3069283U);
}

void a_new_document_is_not_sealed() {
    const wd::document doc = make_document();
    BOOST_TEST(unsealed(doc));
    const std::span<const std::byte> image = doc.image();
    BOOST_TEST(std::ranges::all_of(image.subspan(seal_at, 16),
                                   [](std::byte b) { return b == std::byte{0}; }));
}

void seal_writes_the_marker_and_the_crc() {
    wd::document doc = make_document();
    const std::vector<std::byte> before(doc.image().begin(), doc.image().end());
    if (!BOOST_TEST(doc.seal().has_value())) {
        return;
    }
    BOOST_TEST(doc.sealed());
    const std::span<const std::byte> image = doc.image();
    BOOST_TEST_EQ(seal_of(image), sealed_marker | expected_crc(image));
    // Nothing else changed: the seal is the only difference.
    std::vector<std::byte> after(image.begin(), image.end());
    std::fill_n(after.begin() + seal_at, 8, std::byte{0});
    BOOST_TEST(after == before);

    // Sealing again writes the same seal.
    const std::uint64_t first = seal_of(image);
    BOOST_TEST(doc.seal().has_value());
    BOOST_TEST_EQ(seal_of(doc.image()), first);

    // A moved-from document has no image to seal.
    const wd::document taken = std::move(doc);
    BOOST_TEST(doc.seal().error() == wd::errc::type);  // NOLINT(bugprone-use-after-move)
    BOOST_TEST(!doc.sealed());
    BOOST_TEST(taken.sealed());
}

void a_sealed_image_moves_and_opens() {
    wd::document doc = make_document(false);
    webcpp::test::require(BOOST_TEST(doc.seal().has_value()));
    const std::span<const std::byte> image = doc.image();

    // In place, at another address, and in a larger buffer: the image is what its header says.
    const exact_buffer moved(image, 4096);
    auto opened = wd::open(moved.bytes());
    if (!BOOST_TEST(opened.has_value())) {
        return;
    }
    BOOST_TEST(opened->sealed());
    BOOST_TEST_EQ(wd::stringify(*opened), wd::stringify(doc));

    // An owned copy, and a clone, keep the seal: their bytes are the image's.
    auto copied = wd::from_image(image);
    if (!BOOST_TEST(copied.has_value())) {
        return;
    }
    BOOST_TEST(copied->sealed());
    BOOST_TEST(std::ranges::equal(copied->image(), image));
    auto cloned = opened->clone();
    if (!BOOST_TEST(cloned.has_value())) {
        return;
    }
    BOOST_TEST(cloned->sealed());
    BOOST_TEST(std::ranges::equal(cloned->image(), image));

    // A document opened in place is sealed in place, and opens again.
    BOOST_TEST((*opened)["name"].set("changed").has_value());
    BOOST_TEST(unsealed(*opened));
    BOOST_TEST(opened->seal().has_value());
    auto again = wd::open(moved.bytes());
    if (BOOST_TEST(again.has_value())) {
        BOOST_TEST(again->sealed());
        BOOST_TEST_EQ((*again)["name"].get<std::string_view>().value(), "changed");
    }
}

// Every byte of a sealed image changed, one at a time, in a buffer of exactly its size: each is
// refused as damaged, by open and by from_image, and with verify::none none is refused for the
// seal (a change may still break the heap, which the heap's own check refuses).
void every_byte_changed_is_refused() {
    wd::document doc = make_document();
    webcpp::test::require(BOOST_TEST(doc.seal().has_value()));
    const std::span<const std::byte> image = doc.image();
    const exact_buffer buffer(image);
    const std::span<std::byte> bytes = buffer.bytes();

    std::size_t refused_by_open = 0;
    std::size_t refused_by_copy = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] ^= std::byte{0xFF};
        const auto opened = wd::open(bytes);
        const auto copied = wd::from_image(std::span<const std::byte>(bytes));
        if (!opened.has_value() && opened.error() == wd::errc::damaged_document) {
            ++refused_by_open;
        } else {
            BOOST_ERROR("a changed byte not refused as damaged by open");
        }
        if (!copied.has_value() && copied.error() == wd::errc::damaged_document) {
            ++refused_by_copy;
        }
        bytes[i] ^= std::byte{0xFF};
    }
    BOOST_TEST_EQ(refused_by_open, bytes.size());
    BOOST_TEST_EQ(refused_by_copy, bytes.size());
    // Restored, it opens.
    BOOST_TEST(wd::open(bytes).has_value());
}

// Every single bit of the header flipped: refused as damaged, the seal's own bits included.
void every_header_bit_flipped_is_refused() {
    wd::document doc = make_document();
    webcpp::test::require(BOOST_TEST(doc.seal().has_value()));
    const exact_buffer buffer(doc.image());
    const std::span<std::byte> bytes = buffer.bytes();
    for (std::size_t bit = 0; bit < sizeof(wd::image_header) * 8; ++bit) {
        const auto mask = static_cast<std::byte>(1U << (bit % 8));
        bytes[bit / 8] ^= mask;
        const auto opened = wd::open(bytes);
        BOOST_TEST(!opened.has_value() && opened.error() == wd::errc::damaged_document);
        bytes[bit / 8] ^= mask;
    }
}

// A sealed image cut short is refused as damaged: it is a torn write.
void a_sealed_image_cut_short_is_damaged() {
    wd::document doc = make_document();
    webcpp::test::require(BOOST_TEST(doc.seal().has_value()));
    const std::span<const std::byte> image = doc.image();
    const exact_buffer cut(image.first(image.size() - 16));
    BOOST_TEST(wd::open(cut.bytes()).error() == wd::errc::damaged_document);
    BOOST_TEST(wd::from_image(std::span<const std::byte>(cut.bytes())).error() ==
               wd::errc::damaged_document);
    // Unsealed, the same cut is not a document, as before.
    const wd::document plain = make_document();
    const exact_buffer plain_cut(plain.image().first(plain.image().size() - 16));
    BOOST_TEST(wd::open(plain_cut.bytes()).error() == wd::errc::not_a_document);
}

// verify::none skips the pass: a damaged string opens and reads as damaged.
void verify_none_trusts_the_memory() {
    wd::document doc = make_document();
    webcpp::test::require(BOOST_TEST(doc.seal().has_value()));
    const exact_buffer buffer(doc.image());
    const std::span<std::byte> bytes = buffer.bytes();
    const auto found =
        std::ranges::search(bytes, std::as_bytes(std::span(std::string_view("value"))));
    webcpp::test::require(BOOST_TEST(!found.empty()));
    found.front() = std::byte{'V'};

    BOOST_TEST(wd::open(bytes).error() == wd::errc::damaged_document);
    BOOST_TEST(wd::from_image(std::span<const std::byte>(bytes)).error() ==
               wd::errc::damaged_document);
    BOOST_TEST(wd::check_image(bytes, wd::detail::layout_tag<wd::default_traits>()).error() ==
               wd::errc::damaged_document);

    auto opened = wd::open(bytes, wd::verify::none);
    if (BOOST_TEST(opened.has_value())) {
        BOOST_TEST_EQ((*opened)["nested"]["k"].get<std::string_view>().value(), "Value");
        // Still marked sealed: verify::none reads the seal without checking it.
        BOOST_TEST(opened->sealed());
    }
    auto copied =
        wd::from_image(std::span<const std::byte>(bytes), wd::default_max_bytes, wd::verify::none);
    if (BOOST_TEST(copied.has_value())) {
        BOOST_TEST_EQ((*copied)["nested"]["k"].get<std::string_view>().value(), "Value");
    }
    BOOST_TEST(
        wd::check_image(bytes, wd::detail::layout_tag<wd::default_traits>(), wd::verify::none)
            .has_value());
}

// Each kind of change clears the seal, and the image opens unsealed; a change that fails changes
// nothing, the seal included.
void every_change_clears_the_seal() {
    const auto changed = [](const char* what, auto change) {
        wd::document doc = make_document();
        webcpp::test::require(BOOST_TEST(doc.seal().has_value()));
        const bool done = change(doc);
        if (!BOOST_TEST(done)) {
            BOOST_ERROR(what);
            return;
        }
        if (!BOOST_TEST(unsealed(doc))) {
            BOOST_ERROR(what);
        }
        const exact_buffer buffer(doc.image());
        BOOST_TEST(wd::open(buffer.bytes()).has_value());
        // Sealed again, it opens sealed.
        BOOST_TEST(doc.seal().has_value());
        BOOST_TEST(wd::from_image(doc.image()).has_value());
    };
    changed("set a member", [](wd::document& d) { return d["name"].set("x").has_value(); });
    changed("add a member", [](wd::document& d) { return d["added"].set(1).has_value(); });
    changed("erase a member", [](wd::document& d) {
        auto root = d.root();
        return root.erase("name").has_value();
    });
    changed("set an element", [](wd::document& d) { return d["list"][1].set(9).has_value(); });
    changed("push", [](wd::document& d) { return d["list"].push(5).has_value(); });
    changed("reverse", [](wd::document& d) { return d["list"].reverse().has_value(); });
    changed("splice", [](wd::document& d) { return d["list"].splice(1, 2).has_value(); });
    changed("set_length", [](wd::document& d) { return d["list"].set_length(1).has_value(); });
    changed("set the root", [](wd::document& d) {
        auto root = d.root();
        return root.set(true).has_value();
    });
    changed("grow", [](wd::document& d) {
        // Larger than the image: the owned image grows.
        return d["big"].set(std::string(8192, 'b')).has_value();
    });
    changed("shrink_to_fit", [](wd::document& d) {
        auto root = d.root();
        return d["big"].set(std::string(8192, 'b')).has_value() && root.erase("big").has_value() &&
               d.seal().has_value() && d.shrink_to_fit().has_value();
    });

    // A change that fails for want of room, in an external image, changes nothing.
    const wd::document source = make_document();
    const exact_buffer buffer(source.image());
    auto opened = wd::open(buffer.bytes());
    webcpp::test::require(BOOST_TEST(opened.has_value()));
    webcpp::test::require(BOOST_TEST(opened->seal().has_value()));
    const std::vector<std::byte> before(opened->image().begin(), opened->image().end());
    BOOST_TEST((*opened)["big"].set(std::string(8192, 'b')).error() == wd::errc::capacity);
    BOOST_TEST(opened->sealed());
    BOOST_TEST(std::ranges::equal(opened->image(), before));
}

// The image reader verifies each sealed image of a stream: a damaged one stops it as damaged, the
// documents before it delivered.
void the_reader_verifies() {
    wd::document first = make_document(false);
    wd::document second = make_document(false);
    webcpp::test::require(BOOST_TEST(first.seal().has_value()));
    webcpp::test::require(BOOST_TEST(second.seal().has_value()));
    std::vector<std::byte> stream(first.image().begin(), first.image().end());
    stream.insert(stream.end(), second.image().begin(), second.image().end());

    const std::size_t block = (first.image().size() + 15) / 16 * 16;
    auto pool = wd::image_pool::create(block, 2);
    webcpp::test::require(BOOST_TEST(pool.has_value()));
    {
        wd::image_reader reader(*pool);
        const auto written = reader.write(stream);
        BOOST_TEST(written.has_value() && *written == stream.size());
        auto next = reader.next();
        webcpp::test::require(BOOST_TEST(next.has_value()));
        std::optional<wd::leased_document<>>& taken = *next;
        if (!taken.has_value()) {
            BOOST_ERROR("the reader holds no document");
            return;
        }
        wd::leased_document<>& lease = *taken;
        BOOST_TEST(lease.sealed());
        BOOST_TEST_EQ(wd::stringify(lease.get()), wd::stringify(first));
        // A leased document seals, and a change clears it.
        BOOST_TEST(lease["name"].set("leased").has_value());
        BOOST_TEST(!lease.sealed());
        BOOST_TEST(lease.seal().has_value());
        BOOST_TEST(lease.sealed());
    }

    // A byte of the second image's heap changed.
    stream[first.image().size() + 200] ^= std::byte{0x01};
    wd::image_reader reader(*pool);
    const auto written = reader.write(stream);
    BOOST_TEST(!written.has_value() && written.error() == wd::errc::damaged_document);
    auto delivered = reader.next();
    BOOST_TEST(delivered.has_value() && delivered->has_value());
    delivered = reader.next();
    BOOST_TEST(!delivered.has_value() && delivered.error() == wd::errc::damaged_document);
}

}  // namespace

int main() {
    the_checksum_is_crc32c();
    a_new_document_is_not_sealed();
    seal_writes_the_marker_and_the_crc();
    a_sealed_image_moves_and_opens();
    every_byte_changed_is_refused();
    every_header_bit_flipped_is_refused();
    a_sealed_image_cut_short_is_damaged();
    verify_none_trusts_the_memory();
    every_change_clears_the_seal();
    the_reader_verifies();
    return boost::report_errors();
}
