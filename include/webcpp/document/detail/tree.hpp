// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The tree of a document: its nodes, one Boost.MultiIndex container of them in the image's heap,
 with three ordered indices, the key order of the traits' object policy, and the operations that
 change it, each of which finds its room before it changes anything.

 Every mutation runs in three phases. Room: it lists the blocks it will take (@ref node_probe_bytes
 per node it creates, counting a literal's entries and a copied subtree's nodes, and each string's
 exact size) and probes the heap for them; while the probe fails, an owned region grows, and when it
 cannot, the operation returns @ref errc::capacity before anything changed. Growth moves the image,
 so no pointer into it is held across this phase. Allocate: the strings, then each node by
 `emplace`, built in place. Arrange: what allocates nothing, a `modify` that swaps a prepared
 string or scalar into a node, ordinals shifted, a subtree erased children first.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_TREE_HPP
#define WEBCPP_DOCUMENT_DETAIL_TREE_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/heap.hpp>
#include <webcpp/document/detail/utf.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/image.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/number.hpp>
#include <webcpp/document/traits.hpp>

#include <boost/assert.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/throw_exception.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace webcpp::document::detail {

/**
 The size of a node's scalar: 16, the largest size of a @ref stored_scalar, which is a multiple of
 8, so the node needs no padding after it.
*/
template <class Traits>
inline constexpr std::size_t scalar_bytes = 16;

/** How a number is held: as the traits' `integer` or as their `number`; none for other kinds. */
enum class repr : std::uint8_t {
    /** Not a number. */
    none = 0,

    /** The traits' `integer`. */
    integer = 1,

    /** The traits' `number`. */
    number = 2,
};

/** The group of an element, and of an array-index key under `javascript_order`. */
inline constexpr std::uint8_t index_group = 0;

/** The group of every other key. */
inline constexpr std::uint8_t key_group = 1;

/** The id of the root node; ids start there and are never reused. */
inline constexpr std::uint64_t root_id = 1;

/**
 The offset of the tree's container from the image's start, the same in every image: the heap's
 first block, since formatting allocates the container first.
*/
inline constexpr std::uint64_t container_offset = first_block_offset;

/**
 One value of the tree, in the image's heap: a scalar, a string, bytes, or a container whose
 children are the nodes whose `parent` is its `id`.
*/
template <class Traits>
struct node {
    /** Unique, never reused; the root is @ref root_id. */
    std::uint64_t id;

    /** The container's id; 0 for the root. */
    std::uint64_t parent;

    /**
     The key's group: @ref index_group for an element and for an array-index key under
     `javascript_order`, else @ref key_group.
    */
    std::uint8_t group;

    /**
     An element's index, an index key's value, a key's insertion number, or under `sorted` the
     node's id.
    */
    std::uint64_t ordinal;

    /** The member's key, in the traits' encoding; empty for an element and the root. */
    byte_string key;

    /** The node's kind; @ref kind::undefined only for the root of a document that holds nothing. */
    webcpp::document::kind kind;

    /** For a number, whether it is held as the traits' `integer` or `number`. */
    repr number;

    /**
     The scalar, by its codec: a boolean, an integer, a number or a date; for an array its length
     at bytes 0 to 8, for an object its next insertion number at 0 to 8 and its key count at 8
     to 16.
    */
    std::array<std::byte, scalar_bytes<Traits>> scalar;

    /** A string's code units or a bytes value's bytes. */
    byte_string text;

    /** Builds a node in place, from fields already prepared: `emplace` calls it in the heap. */
    node(std::uint64_t id, std::uint64_t parent, std::uint8_t group, std::uint64_t ordinal,
         byte_string key, webcpp::document::kind kind, repr number,
         const std::array<std::byte, scalar_bytes<Traits>>& scalar, byte_string text) noexcept
        : id(id),
          parent(parent),
          group(group),
          ordinal(ordinal),
          key(std::move(key)),
          kind(kind),
          number(number),
          scalar(scalar),
          text(std::move(text)) {}

    /** The scalar, read by the codec of `T`. */
    template <class T>
    [[nodiscard]] T load() const noexcept {
        return scalar_codec<T>::load(scalar.data());
    }
};

/** The tag of the index by id. */
struct by_id {};

/**
 The tag of the index by parent, group and ordinal: elements by index, members in the policy's
 order.
*/
struct by_order {};

/** The tag of the index by parent and key. */
struct by_key {};

/**
 `F`, an empty function object, with a byte of value, zero, for the container to keep in the image.

 Tip: an empty class's byte holds no value, and MSVC copies a temporary's without writing it first:
 each index keeps its key extractor and its comparator in the image, so an empty one put the
 stack's garbage there, at a place of its own on each run. `F` stays an empty base, so the size is
 the same.
*/
template <class F>
struct with_zero_byte : F {
    /** Zero. */
    std::uint8_t zero = 0;
};

/**
 The order of keys in the index @ref by_key, by their UTF-16 code units, whatever the encoding: the
 order the `sorted` policy writes, as JavaScript's sort and RFC 8785 order keys.
*/
template <class Traits>
struct key_less {
    /** Whether `a` comes before `b`, each the bytes of a key in the traits' encoding. */
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        if constexpr (std::same_as<typename Traits::string, utf16>) {
            // The units are in the machine's byte order, so their bytes do not sort as they do.
            const std::size_t units = std::min(a.size(), b.size()) / sizeof(char16_t);
            for (std::size_t i = 0; i < units; ++i) {
                char16_t x = 0;
                char16_t y = 0;
                std::memcpy(&x, a.data() + (i * sizeof(char16_t)), sizeof x);
                std::memcpy(&y, b.data() + (i * sizeof(char16_t)), sizeof y);
                if (x != y) {
                    return x < y;
                }
            }
            return a.size() < b.size();
        } else {
            // UTF-8's bytes sort as code points, which is UTF-16's order but where a character
            // from U+E000 to U+FFFF meets one past U+FFFF, whose surrogates UTF-16 writes from
            // 0xD800: the first characters that differ are compared by their first UTF-16 units.
            const std::size_t common = std::min(a.size(), b.size());
            std::size_t i = 0;
            while (i < common && a[i] == b[i]) {
                ++i;
            }
            if (i == common) {
                return a.size() < b.size();
            }
            // The two characters start at the same byte, the last lead byte up to the difference.
            std::size_t start = i;
            while (start > 0 && continuation(byte_at(a, start)) &&
                   continuation(byte_at(b, start))) {
                --start;
            }
            std::string_view rest_of_a = a;
            std::string_view rest_of_b = b;
            rest_of_a.remove_prefix(start);
            rest_of_b.remove_prefix(start);
            const std::optional<char16_t> x = first_utf16_unit(rest_of_a);
            const std::optional<char16_t> y = first_utf16_unit(rest_of_b);
            if (x && y && *x != *y) {
                return *x < *y;
            }
            // Two characters past U+FFFF of one high surrogate sort as their bytes do, and text
            // that is not UTF-8, which no stored key is, by its bytes, so no two keys are
            // equivalent unless they are equal.
            return byte_at(a, i) < byte_at(b, i);
        }
    }
};

/**
 Orders tuples by their first fields, as many as both have, each by `Less`: a lookup by a tuple of
 a key's first fields finds the range of keys that begin with them, as a composite key does.
*/
template <class... Less>
struct prefix_less {
    /** Whether `a` comes before `b`. */
    template <class... A, class... B>
    bool operator()(const std::tuple<A...>& a, const std::tuple<B...>& b) const noexcept {
        return before<0>(a, b);
    }

    /** Zero: see @ref with_zero_byte. */
    std::uint8_t zero = 0;

private:
    template <std::size_t I, class TA, class TB>
    static bool before(const TA& a, const TB& b) noexcept {
        if constexpr (I == std::min(std::tuple_size_v<TA>, std::tuple_size_v<TB>)) {
            return false;
        } else {
            const std::tuple_element_t<I, std::tuple<Less...>> less{};
            if (less(std::get<I>(a), std::get<I>(b))) {
                return true;
            }
            if (less(std::get<I>(b), std::get<I>(a))) {
                return false;
            }
            return before<I + 1>(a, b);
        }
    }
};

/** The key of the index @ref by_order, which is the parent, the group and the ordinal. */
template <class Traits>
struct order_key {
    /** What it extracts. */
    using result_type = std::tuple<std::uint64_t, std::uint8_t, std::uint64_t>;

    /** The key of `n`. */
    result_type operator()(const node<Traits>& n) const noexcept {
        return {n.parent, n.group, n.ordinal};
    }

    /** Zero: see @ref with_zero_byte. */
    std::uint8_t zero = 0;
};

/** The key of the index @ref by_key, which is the parent and the key's bytes as a
 * `std::string_view`. */
template <class Traits>
struct member_key {
    /** What it extracts. */
    using result_type = std::tuple<std::uint64_t, std::string_view>;

    /** The key of `n`. */
    result_type operator()(const node<Traits>& n) const noexcept {
        return {n.parent, n.key.chars()};
    }

    /** Zero: see @ref with_zero_byte. */
    std::uint8_t zero = 0;
};

/**
 The container of a tree's nodes: three ordered indices, never hashed, since keys come from
 untrusted text.

 Tip: no key extractor and no comparator of an index is empty, nor holds an empty one: see
 @ref with_zero_byte.
*/
template <class Traits>
using container = boost::multi_index_container<
    node<Traits>,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<boost::multi_index::tag<by_id>,
                                           with_zero_byte<boost::multi_index::member<
                                               node<Traits>, std::uint64_t, &node<Traits>::id>>,
                                           with_zero_byte<std::less<>>>,
        boost::multi_index::ordered_unique<boost::multi_index::tag<by_order>, order_key<Traits>,
                                           prefix_less<std::less<>, std::less<>, std::less<>>>,
        boost::multi_index::ordered_non_unique<boost::multi_index::tag<by_key>, member_key<Traits>,
                                               prefix_less<std::less<>, key_less<Traits>>>>,
    allocator<node<Traits>>>;

/**
 What a probe asks per node: an upper bound of the container's block for one node, which
 Boost.MultiIndex does not name publicly; a test pins that every block is at most this.
*/
template <class Traits>
inline constexpr std::uint64_t node_probe_bytes = sizeof(node<Traits>) + 128;

template <class Traits>
class tree;

template <class Traits>
struct literal_entry;

/**
 A value about to be stored: what `set`, the array methods and the parser hand the tree.

 It is a scalar (`kind`, `number`, `scalar`), a string or bytes (`text`, already in the traits'
 encoding), a container with the entries of a literal (`entries`), or a deep copy of a subtree of a
 tree (`from`), which may be this tree.
*/
template <class Traits>
struct item {
    /** The kind; @ref kind::undefined only for a root. */
    webcpp::document::kind kind = webcpp::document::kind::null;

    /** For a number, how it is held. */
    repr number = repr::none;

    /** The scalar, by its codec, as a node holds it. */
    std::array<std::byte, scalar_bytes<Traits>> scalar{};

    /** A string's code units in the traits' encoding, or a bytes value's bytes. */
    std::span<const std::byte> text;

    /** A literal object's or array's entries, or null for an empty container. */
    const literal_entry<Traits>* entries = nullptr;

    /** How many entries. */
    std::size_t entry_count = 0;

    /** The tree of a subtree to copy deeply, or null. */
    const tree<Traits>* from = nullptr;

    /** The id of the subtree's node in `from`. */
    std::uint64_t from_id = 0;

    /** Stores `v` in `scalar` by the codec of `T`. */
    template <class T>
    void store(const T& v) noexcept {
        scalar_codec<T>::store(v, scalar.data());
    }
};

/** One entry of a literal container: a key, in UTF-8, empty for an element, and its value. */
template <class Traits>
struct literal_entry {
    /** The member's key, in UTF-8; empty for an array's element. */
    std::string_view key;

    /** The value; a nested literal points at its own entries. */
    item<Traits> value;
};

/**
 The image's layout tag: an FNV-1a hash, 32 bits, of @ref traits_tag and of the sizes and
 alignments of the node, the container and the heap, so that an image of another build is refused.
*/
template <class Traits>
constexpr std::uint32_t layout_tag() noexcept {
    std::uint32_t hash = fnv1a_basis;
    const auto add = [&hash](std::uint64_t value, std::uint32_t bytes) {
        for (std::uint32_t i = 0; i < bytes; ++i) {
            hash = fnv1a_byte(hash, static_cast<std::uint32_t>((value >> (8U * i)) & 0xffU));
        }
    };
    add(traits_tag<Traits>(), 4);
    add(sizeof(node<Traits>), 8);
    add(alignof(node<Traits>), 8);
    add(sizeof(container<Traits>), 8);
    add(alignof(container<Traits>), 8);
    add(sizeof(heap), 8);
    add(alignof(heap), 8);
    return hash;
}

/**
 The tree of a document, a light handle on a region: it holds no pointer into the image, so it
 stays valid when the region grows and its bytes move.

 Every operation that changes the tree either completes or changes nothing, and returns
 @ref errc::capacity when there is no room. Keys are given in UTF-8; with `utf16` the tree
 transcodes them before it stores or looks them up, and a key it cannot transcode finds nothing.
*/
template <class Traits>
class tree {
public:
    // An operation ends the program where the process heap, which holds what it stages, runs
    // out, as the library tolerates, and where an allocation in the image fails after its probe
    // passed, a bug of the document's, which the allocator reports by boost::throw_exception.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     Formats a region that holds an empty heap: the header, the container, a root holding `null`.
    */
    static result<tree> format(region& r) noexcept {
        if (r.data() == nullptr || r.size() < smallest_image()) {
            return errc::capacity;
        }
        // The container's block, the header node its constructor allocates, and the root.
        const std::array<std::uint64_t, 3> sizes{
            sizeof(nodes_type),
            node_probe_bytes<Traits>,
            node_probe_bytes<Traits>,
        };
        while (!probe(heap_of(r), sizes)) {
            if (!r.grow(grown_size(r, sizes[0] + sizes[1] + sizes[2]))) {
                return errc::capacity;
            }
        }
        heap& h = heap_of(r);
        void* block = h.allocate_aligned(sizeof(nodes_type), alignof(nodes_type));
        if (block == nullptr) {
            boost::throw_exception(std::bad_alloc());
        }
        std::memset(block, 0, sizeof(nodes_type));
        // The container is the heap's first block, where attach requires it.
        BOOST_ASSERT(std::cmp_equal(static_cast<std::byte*>(block) - r.data(), container_offset));
        auto* nodes = new (block) nodes_type(allocator<node_type>(&h));
        const byte_string empty;
        const scalar_type zero{};
        nodes->emplace(root_id, std::uint64_t{0}, index_group, std::uint64_t{0}, empty,
                       kind_t::null, repr::none, zero, empty);

        image_header header{};
        header.magic = image_magic;
        header.version = format_version;
        header.byte_order = byte_order_mark;
        header.reserved_4 = 0;
        header.layout = layout_tag<Traits>();
        header.size = r.size();
        header.container = container_offset;
        header.next_id = root_id + 1;
        std::memcpy(r.data(), &header, sizeof header);
        tree t;
        t.region_ = &r;
        return t;
    }

    // NOLINTEND(bugprone-exception-escape)

    /**
     Attaches to an image already in a region: checks its header, its heap's sanity, and that the
     pointers it follows to the root lie inside the image.
    */
    static result<tree> attach(region& r) noexcept {
        // Fewer bytes than a header and a heap would have the heap's check read past them.
        if (r.data() == nullptr || r.size() < smallest_image() ||
            reinterpret_cast<std::uintptr_t>(r.data()) % 16 != 0) {
            return errc::not_a_document;
        }
        // A seal is checked by whoever opens the image, before it attaches.
        const std::span<const std::byte> bytes(r.data(), static_cast<std::size_t>(r.size()));
        if (const auto checked = check_image(bytes, layout_tag<Traits>(), verify::none);
            !checked.has_value()) {
            return checked.error();
        }
        image_header header{};
        std::memcpy(&header, r.data(), sizeof header);
        if (header.container != container_offset ||
            header.size < container_offset + sizeof(nodes_type) || header.next_id <= root_id) {
            return errc::not_a_document;
        }
        heap& h = heap_of(r);
        if (h.get_size() > header.size - header_bytes || !h.check_sanity()) {
            return errc::not_a_document;
        }
        tree t;
        t.region_ = &r;
        const node_type* root = t.first_by_id(header.size);
        if (root == nullptr || root->id != root_id || root->parent != 0) {
            return errc::not_a_document;
        }
        return t;
    }

    /** A tree attached to nothing, which reads as nothing and changes nothing. */
    tree() noexcept = default;

    /** Attaches the tree to `r`, the same image after the region that held it was moved. */
    void rebind(region& r) noexcept { region_ = &r; }

    /** The region it is attached to. */
    [[nodiscard]] region& storage() const noexcept { return *region_; }

    /** The root's id, @ref root_id. */
    [[nodiscard]] std::uint64_t root() const noexcept { return root_id; }

    /** The node of `id`, or null when there is none. */
    [[nodiscard]] const node<Traits>* find(std::uint64_t id) const noexcept {
        const nodes_type* nodes = nodes_of();
        return nodes == nullptr ? nullptr : find_in(*nodes, id);
    }

    // A lookup ends the program where the process heap cannot hold a long key's UTF-16, as the
    // library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /** The member `key`, in UTF-8, of the object `object`, or null. */
    [[nodiscard]] const node<Traits>* member(std::uint64_t object,
                                             std::string_view key) const noexcept {
        const nodes_type* nodes = nodes_of();
        if (nodes == nullptr || !is_kind(find_in(*nodes, object), kind_t::object)) {
            return nullptr;
        }
        if constexpr (utf16_text) {
            if (!valid_utf8(key)) {
                return nullptr;
            }
            // Most keys are short: they are transcoded on the stack.
            std::array<char16_t, 64> small{};
            std::vector<char16_t> large;
            const std::uint64_t units = utf16_length(key);
            char16_t* out = small.data();
            if (units > small.size()) {
                large.resize(static_cast<std::size_t>(units));
                out = large.data();
            }
            to_utf16(key, out);
            return member_in(
                *nodes, object,
                as_chars(std::as_bytes(std::span(out, static_cast<std::size_t>(units)))));
        } else {
            return member_in(*nodes, object, key);
        }
    }

    // NOLINTEND(bugprone-exception-escape)

    /** The element `index` of the array `array`, or null. */
    [[nodiscard]] const node<Traits>* element(std::uint64_t array,
                                              std::uint64_t index) const noexcept {
        const nodes_type* nodes = nodes_of();
        if (nodes == nullptr || !is_kind(find_in(*nodes, array), kind_t::array)) {
            return nullptr;
        }
        return element_in(*nodes, array, index);
    }

    /** The first child of `parent` in the policy's order, or null. */
    [[nodiscard]] const node<Traits>* first_child(std::uint64_t parent) const noexcept {
        const nodes_type* nodes = nodes_of();
        if (nodes == nullptr) {
            return nullptr;
        }
        if constexpr (sorted_keys) {
            if (is_kind(find_in(*nodes, parent), kind_t::object)) {
                return first_of(nodes->template get<by_key>(), parent);
            }
        }
        return first_of(nodes->template get<by_order>(), parent);
    }

    /** The child after `child` in its parent, in the policy's order, or null. */
    [[nodiscard]] const node<Traits>* next_sibling(const node<Traits>& child) const noexcept {
        const nodes_type* nodes = nodes_of();
        if (nodes == nullptr) {
            return nullptr;
        }
        // Under sorted, members are all in the key group and elements in the index group.
        if constexpr (sorted_keys) {
            if (child.group == key_group) {
                return next_of(nodes->template get<by_key>(), child);
            }
        }
        return next_of(nodes->template get<by_order>(), child);
    }

    /** Calls `f` with each child of `parent`, a `const node<Traits>&`, in the policy's order. */
    template <class F>
    void for_each_child(std::uint64_t parent, F&& f) const {
        for (const node_type* n = first_child(parent); n != nullptr; n = next_sibling(*n)) {
            f(*n);
        }
    }

    /** An array's length or an object's key count; 0 for any other node. */
    [[nodiscard]] std::uint64_t count(std::uint64_t container_id) const noexcept {
        const node_type* n = find(container_id);
        if (is_kind(n, kind_t::array)) {
            return word(n->scalar, length_at);
        }
        if (is_kind(n, kind_t::object)) {
            return word(n->scalar, key_count_at);
        }
        return 0;
    }

    /** How many nodes the tree holds. */
    [[nodiscard]] std::uint64_t node_count() const noexcept {
        const nodes_type* nodes = nodes_of();
        return nodes == nullptr ? 0 : nodes->size();
    }

    // An operation ends the program where the process heap, which holds what it stages, runs
    // out, as the library tolerates, and where an allocation in the image fails after its probe
    // passed, a bug of the document's, which the allocator reports by boost::throw_exception.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     Replaces the value of node `id` with `v`, its children erased after the new value is in place.
    */
    result<void> assign(std::uint64_t id, const item<Traits>& v) noexcept {
        const nodes_type* nodes = nodes_of();
        const node_type* target = nodes == nullptr ? nullptr : find_in(*nodes, id);
        if (target == nullptr) {
            return errc::type;
        }
        change c = start_change();
        add_existing(c, *target, v, id == root_id);
        if (const auto read = read_values(c); !read.has_value()) {
            return read;
        }
        return write(c, [] {});
    }

    /**
     Sets the member `key`: an existing one is assigned and keeps its place, a new one takes the
     policy's place; returns its id.
    */
    result<std::uint64_t> set_member(std::uint64_t object, std::string_view key,
                                     const item<Traits>& v) noexcept {
        const nodes_type* nodes = nodes_of();
        const node_type* o = nodes == nullptr ? nullptr : find_in(*nodes, object);
        if (!is_kind(o, kind_t::object)) {
            return errc::type;
        }
        change c = start_change();
        const auto stored = encode_key(c, key);
        if (!stored.has_value()) {
            return errc::type;
        }
        if (const node_type* existing = member_in(*nodes, object, as_chars(*stored))) {
            const std::uint64_t id = existing->id;
            if (const auto done = assign(id, v); !done.has_value()) {
                return done.error();
            }
            return id;
        }
        scalar_type counts = o->scalar;
        std::uint64_t next = word(counts, next_insertion_at);
        const std::uint64_t id = add_member(c, object, key, *stored, next, v);
        if (const auto read = read_values(c); !read.has_value()) {
            return read.error();
        }
        if (const auto done = write(c, [] {}); !done.has_value()) {
            return done.error();
        }
        set_word(counts, next_insertion_at, next);
        set_word(counts, key_count_at, word(counts, key_count_at) + 1);
        set_scalar(object, counts);
        return id;
    }

    /** Sets each entry's member in order, as one operation: `Object.assign`. */
    result<void> set_members(std::uint64_t object,
                             std::span<const literal_entry<Traits>> members) noexcept {
        const nodes_type* nodes = nodes_of();
        const node_type* o = nodes == nullptr ? nullptr : find_in(*nodes, object);
        if (!is_kind(o, kind_t::object)) {
            return errc::type;
        }
        change c = start_change();
        scalar_type counts = o->scalar;
        std::uint64_t next = word(counts, next_insertion_at);
        std::uint64_t added = 0;
        auto placed = place_members(c, object, members, next, added, true);
        if (!placed.has_value()) {
            return placed;
        }
        if (const auto read = read_values(c); !read.has_value()) {
            return read;
        }
        if (const auto done = write(c, [] {}); !done.has_value()) {
            return done;
        }
        set_word(counts, next_insertion_at, next);
        set_word(counts, key_count_at, word(counts, key_count_at) + added);
        set_scalar(object, counts);
        return {};
    }

    /** Erases the member `key` and its subtree; whether it was there. */
    result<bool> erase_member(std::uint64_t object, std::string_view key) noexcept {
        const node_type* o = find(object);
        if (!is_kind(o, kind_t::object)) {
            return errc::type;
        }
        const node_type* m = member(object, key);
        if (m == nullptr) {
            return false;
        }
        scalar_type counts = o->scalar;
        erase_subtree(m->id);
        set_word(counts, key_count_at, word(counts, key_count_at) - 1);
        set_scalar(object, counts);
        sync_header(read_header().next_id);
        return true;
    }

    /**
     Sets the element `index`: assigned when it exists, else the array filled with `null` up to it
     and `v` appended, as one operation.
    */
    result<void> set_element(std::uint64_t array, std::uint64_t index,
                             const item<Traits>& v) noexcept {
        const node_type* a = find(array);
        if (!is_kind(a, kind_t::array)) {
            return errc::type;
        }
        const std::uint64_t length = word(a->scalar, length_at);
        if (index < length) {
            return assign(element_in(*nodes_of(), array, index)->id, v);
        }
        return append(array, length, index - length, &v);
    }

    /** Inserts `v` before the element `at`, the later elements renumbered. */
    result<void> insert_elements(std::uint64_t array, std::uint64_t at,
                                 std::span<const item<Traits>> v) noexcept {
        return splice(array, at, 0, v);
    }

    /** Erases `n` elements from `at` and their subtrees, the later elements renumbered. */
    result<void> erase_elements(std::uint64_t array, std::uint64_t at, std::uint64_t n) noexcept {
        return splice(array, at, n, {});
    }

    /** Erases `remove` elements from `at` and inserts `v` there, as one operation. */
    result<void> splice(std::uint64_t array, std::uint64_t at, std::uint64_t remove,
                        std::span<const item<Traits>> v) noexcept {
        const node_type* a = find(array);
        if (!is_kind(a, kind_t::array)) {
            return errc::type;
        }
        const std::uint64_t length = word(a->scalar, length_at);
        if (at > length || remove > length - at) {
            return errc::range;
        }
        change c = start_change();
        for (std::uint64_t i = 0; i < remove; ++i) {
            c.doomed.push_back(element_in(*nodes_of(), array, at + i)->id);
        }
        for (std::size_t i = 0; i < v.size(); ++i) {
            add_element(c, array, at + i, &v[i]);
        }
        if (const auto read = read_values(c); !read.has_value()) {
            return read;
        }
        const std::uint64_t added = v.size();
        // The removed elements have left the index group: the later ones move into their place.
        auto done = write(c, [&] {
            if (added > remove) {
                for (std::uint64_t o = length; o > at + remove; --o) {
                    renumber(array, o - 1, o - 1 + (added - remove));
                }
            } else {
                for (std::uint64_t o = at + remove; o < length; ++o) {
                    renumber(array, o, o - (remove - added));
                }
            }
        });
        if (!done.has_value()) {
            return done;
        }
        set_length_word(array, length - remove + added);
        return {};
    }

    /** Reverses the elements of an array. */
    result<void> reverse(std::uint64_t array) noexcept {
        const node_type* a = find(array);
        if (!is_kind(a, kind_t::array)) {
            return errc::type;
        }
        const std::uint64_t length = word(a->scalar, length_at);
        auto& order = nodes_of()->template get<by_order>();
        for (std::uint64_t i = 0; i < length / 2; ++i) {
            const std::uint64_t j = length - 1 - i;
            const auto first = order.find(std::make_tuple(array, index_group, i));
            const auto last = order.find(std::make_tuple(array, index_group, j));
            // The first steps out of the index group while the last takes its ordinal.
            order.modify(first, [](node_type& n) noexcept { n.group = set_aside(index_group); });
            order.modify(last, [i](node_type& n) noexcept { n.ordinal = i; });
            order.modify(first, [j](node_type& n) noexcept {
                n.group = index_group;
                n.ordinal = j;
            });
        }
        sync_header(read_header().next_id);
        return {};
    }

    /** Truncates an array to `n` elements, or appends `null` up to `n`. */
    result<void> set_length(std::uint64_t array, std::uint64_t n) noexcept {
        const node_type* a = find(array);
        if (!is_kind(a, kind_t::array)) {
            return errc::type;
        }
        const std::uint64_t length = word(a->scalar, length_at);
        if (n <= length) {
            return splice(array, n, length - n, {});
        }
        return append(array, length, n - length, nullptr);
    }

    // NOLINTEND(bugprone-exception-escape)

    /**
     Gives an owned region's free end back: the heap shrunk, the region moved into a buffer of the
     image's size.
    */
    result<void> shrink_to_fit() noexcept {
        if (nodes_of() == nullptr) {
            return errc::type;
        }
        if (!region_->is_owned()) {
            return {};
        }
        heap_of(*region_).shrink_to_fit();
        const std::uint64_t size = header_bytes + heap_of(*region_).get_size();
        if (!region_->shrink_to(size)) {
            // The heap takes its end back, so that it ends where the region does.
            heap_of(*region_).grow(region_->size() - size);
            return errc::capacity;
        }
        sync_header(read_header().next_id);
        return {};
    }

    /**
     Grows an owned region, in one step, to hold at least `bytes`, as a change that needs room
     does: nothing when it already does, @ref errc::capacity when it cannot.
    */
    result<void> reserve(std::uint64_t bytes) noexcept {
        if (nodes_of() == nullptr) {
            return errc::type;
        }
        if (bytes <= region_->size()) {
            return {};
        }
        if (!region_->grow(bytes)) {
            return errc::capacity;
        }
        sync_header(read_header().next_id);
        return {};
    }

    /**
     The exact size of the image @ref compact_to builds from this tree: the header, the heap's own
     bytes, the container's two blocks, and for each node its block and those of its key and text.

     It reads every node and allocates nothing; 0 when the tree is attached to nothing.
    */
    [[nodiscard]] std::uint64_t compact_bytes() const noexcept {
        const nodes_type* nodes = nodes_of();
        if (nodes == nullptr) {
            return 0;
        }
        const costs c = measure_costs();
        std::uint64_t total = header_bytes + c.heap.overhead + c.container + c.header_node;
        for (const node_type& n : nodes->template get<by_id>()) {
            total = saturated_sum(total, c.node);
            total = saturated_sum(total, c.heap.block(n.key.size));
            total = saturated_sum(total, c.heap.block(n.text.size));
        }
        return total;
    }

    // The walk keeps its stack on the process heap, whose exhaustion ends the program, as the
    // library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     Builds into `to` a copy of `from` in document order: ids from the first, insertion numbers
     from 0, and for each node its key, its text, then the node, each block right after the last.

     `to` is a fresh region of exactly `bytes`, @ref compact_bytes of `from`, over a heap that holds
     nothing, so that its blocks fill it, with no hole and no free block.

     @return The tree over `to`; @ref errc::type when `from` is attached to nothing,
     @ref errc::capacity when `to` is not of `bytes` bytes.
    */
    static result<tree> compact_to(region& to, const tree& from, std::uint64_t bytes) noexcept {
        const nodes_type* from_nodes = from.nodes_of();
        const node_type* root = from_nodes == nullptr ? nullptr : find_in(*from_nodes, root_id);
        if (root == nullptr) {
            return errc::type;
        }
        if (to.data() == nullptr || to.size() != bytes) {
            return errc::capacity;
        }
        const costs c = measure_costs();
        heap& h = heap_of(to);
        // The blocks are taken one after another from the heap's one free block, and a block is
        // taken only when it is there, whatever the sizes say: a wrong figure is an error value.
        if (h.get_free_memory() < c.heap.slack + c.container + c.header_node) {
            return errc::capacity;
        }
        void* block = h.allocate_aligned(sizeof(nodes_type), alignof(nodes_type));
        BOOST_ASSERT(block != nullptr);
        std::memset(block, 0, sizeof(nodes_type));
        // The container is the heap's first block, where attach requires it.
        BOOST_ASSERT(std::cmp_equal(static_cast<std::byte*>(block) - to.data(), container_offset));
        auto* nodes = new (block) nodes_type(allocator<node_type>(&h));

        compaction copier{from, c, h, nodes};
        if (!copier.copy_tree(*root)) {
            return errc::capacity;
        }
        BOOST_ASSERT(h.get_free_memory() == c.heap.slack);

        image_header header{};
        header.magic = image_magic;
        header.version = format_version;
        header.byte_order = byte_order_mark;
        header.reserved_4 = 0;
        header.layout = layout_tag<Traits>();
        header.size = to.size();
        header.container = container_offset;
        header.next_id = copier.next_id;
        std::memcpy(to.data(), &header, sizeof header);
        tree t;
        t.region_ = &to;
        return t;
    }

    // NOLINTEND(bugprone-exception-escape)

    /**
     The container of the nodes, for what walks its indices itself, both ways or by a bound; null
     when the tree is attached to nothing.
    */
    [[nodiscard]] const container<Traits>* nodes() const noexcept { return nodes_of(); }

private:
    /**
     What a compact image's blocks take, measured once per call: the heap's, the container's, the
     container's header node and a node.
    */
    struct costs {
        /** The heap's own. */
        heap_costs heap;

        /** The block of the container itself. */
        std::uint64_t container = 0;

        /** The block of the container's header node, which its constructor takes. */
        std::uint64_t header_node = 0;

        /** The block of each node. */
        std::uint64_t node = 0;
    };

    // The scratch heap is sized for what is built in it, so no allocation fails; were one to, the
    // allocator would report it by boost::throw_exception, as the library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     Measures @ref costs in a scratch heap on the stack, by building in it what a tree does: a
     container and a node. The block sizes of Boost.MultiIndex's nodes are not public, and this way
     none is repeated here.
    */
    static costs measure_costs() noexcept {
        constexpr std::size_t scratch_bytes = 2048;
        alignas(16) std::array<std::byte, scratch_bytes> scratch{};
        heap* h = new (scratch.data()) heap(scratch.size(), sizeof(heap));
        costs c;
        c.heap = measure_heap_costs();
        std::uint64_t free_memory = h->get_free_memory();
        void* block = h->allocate_aligned(sizeof(nodes_type), alignof(nodes_type));
        BOOST_ASSERT(block != nullptr);
        c.container = std::exchange(free_memory, h->get_free_memory()) - free_memory;
        auto* nodes = new (block) nodes_type(allocator<node_type>(h));
        c.header_node = std::exchange(free_memory, h->get_free_memory()) - free_memory;
        const byte_string empty;
        const scalar_type zero{};
        nodes->emplace(root_id, std::uint64_t{0}, index_group, std::uint64_t{0}, empty,
                       kind_t::null, repr::none, zero, empty);
        c.node = std::exchange(free_memory, h->get_free_memory()) - free_memory;
        nodes->~nodes_type();
        return c;
    }

    // NOLINTEND(bugprone-exception-escape)

    /** The node of this tree. */
    using node_type = node<Traits>;

    /** The container of this tree's nodes. */
    using nodes_type = container<Traits>;

    /** A node's scalar. */
    using scalar_type = std::array<std::byte, scalar_bytes<Traits>>;

    /** The kinds of values, which a node's member `kind` hides inside it. */
    using kind_t = webcpp::document::kind;

    /** Whether keys and strings are stored as UTF-16 code units. */
    static constexpr bool utf16_text = std::same_as<typename Traits::string, utf16>;

    /** Whether objects walk their members by key. */
    static constexpr bool sorted_keys = std::same_as<typename Traits::object, sorted>;

    /** Where an array's length is in its scalar. */
    static constexpr std::size_t length_at = 0;

    /** Where an object's next insertion number is in its scalar. */
    static constexpr std::size_t next_insertion_at = 0;

    /** Where an object's key count is in its scalar. */
    static constexpr std::size_t key_count_at = 8;

    /**
     The group a node of `group` takes while an operation sets it aside, erased or renumbered: no
     node is ever left there, and no other node of its parent is in it.
    */
    static constexpr std::uint8_t set_aside(std::uint8_t group) noexcept {
        return static_cast<std::uint8_t>(group + 2);
    }

    // The walk keeps its stack on the process heap, whose exhaustion ends the program, as the
    // library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     What @ref compact_to builds a compact image with: the source tree, the costs of its blocks,
     the heap and the container of the image, and the id the next node takes.
    */
    struct compaction {
        /** The tree copied from. */
        const tree& from;

        /** What the blocks of the image take. */
        const costs& c;

        /** The image's heap. */
        heap& h;

        /** The image's container. */
        nodes_type* nodes;

        /** The id the next node copied takes. */
        std::uint64_t next_id = root_id;

        /**
         What walks the source: the next child to copy, where it goes, and the number the next
         member that is not an index takes.
        */
        struct frame {
            /** The next child to copy. */
            const node_type* child;

            /** The id its parent took in the image. */
            std::uint64_t parent;

            /** The number the next member that is not an index takes. */
            std::uint64_t next_insertion;
        };

        /**
         The scalar a copy of `original` takes: its own, but for an object, whose counts follow the
         members as they are copied, not as they were once set.
        */
        [[nodiscard]] scalar_type scalar_of(const node_type& original) const noexcept {
            if (original.kind != kind_t::object) {
                return original.scalar;
            }
            std::uint64_t members = 0;
            std::uint64_t inserted = 0;
            for (const node_type* m = from.first_child(original.id); m != nullptr;
                 m = from.next_sibling(*m)) {
                ++members;
                inserted += m->group == key_group ? 1 : 0;
            }
            scalar_type scalar{};
            set_word(scalar, next_insertion_at, sorted_keys ? 0 : inserted);
            set_word(scalar, key_count_at, members);
            return scalar;
        }

        /**
         The ordinal of `child`, which takes the id `id`: an element and an array-index key keep
         the index they are; the other members are numbered as they come, and under `sorted` by
         their ids.
        */
        [[nodiscard]] static std::uint64_t ordinal_of(const node_type& child, std::uint64_t id,
                                                      frame& top) noexcept {
            if (child.group == index_group) {
                return child.ordinal;
            }
            if (sorted_keys) {
                return id;
            }
            return top.next_insertion++;
        }

        /** Copies one node, its key and its text, if the heap has room for all three. */
        [[nodiscard]] bool copy(const node_type& original, std::uint64_t parent, std::uint8_t group,
                                std::uint64_t ordinal) noexcept {
            const std::uint64_t needed =
                c.node + c.heap.block(original.key.size) + c.heap.block(original.text.size);
            if (h.get_free_memory() < c.heap.slack + needed) {
                return false;
            }
            const byte_string key = make_probed(h, original.key.view());
            const byte_string text = make_probed(h, original.text.view());
            const scalar_type scalar = scalar_of(original);
            [[maybe_unused]] const bool placed =
                nodes
                    ->emplace(next_id++, parent, group, ordinal, key, original.kind,
                              original.number, scalar, text)
                    .second;
            BOOST_ASSERT(placed);
            return true;
        }

        /** Copies `root` and everything under it in document order, on a stack of its own. */
        [[nodiscard]] bool copy_tree(const node_type& root) noexcept {
            if (!copy(root, 0, index_group, 0)) {
                return false;
            }
            std::vector<frame> stack;
            if (const node_type* first = from.first_child(root_id)) {
                stack.push_back({first, root_id, 0});
            }
            while (!stack.empty()) {
                frame& top = stack.back();
                if (top.child == nullptr) {
                    stack.pop_back();
                    continue;
                }
                const node_type& child = *top.child;
                top.child = from.next_sibling(child);
                const std::uint64_t parent = top.parent;
                const std::uint64_t id = next_id;
                const std::uint64_t ordinal = ordinal_of(child, id, top);
                if (!copy(child, parent, child.group == index_group ? index_group : key_group,
                          ordinal)) {
                    return false;
                }
                if (const node_type* first = from.first_child(child.id)) {
                    stack.push_back({first, id, 0});
                }
            }
            return true;
        }
    };

    // NOLINTEND(bugprone-exception-escape)

    /** A node an operation writes: a new one, or one already there whose value it replaces. */
    struct staged {
        /** The node's id. */
        std::uint64_t id = 0;

        /** Its parent's id. */
        std::uint64_t parent = 0;

        /** Its group. */
        std::uint8_t group = index_group;

        /** Its ordinal. */
        std::uint64_t ordinal = 0;

        /**
         Its key in the traits' encoding; nothing for a node already there, which keeps its own.
        */
        std::span<const std::byte> key;

        /** Its kind. */
        kind_t kind = kind_t::null;

        /** How a number is held. */
        repr number = repr::none;

        /** Its scalar. */
        scalar_type scalar{};

        /** Its string's or bytes' bytes. */
        std::span<const std::byte> text;

        /** Whether the node is already there and only its value is replaced. */
        bool existing = false;
    };

    /** A value still to be read into a staged node: an item, or a node of a tree to copy. */
    struct pending {
        /** The staged node's place in its change. */
        std::size_t slot = 0;

        /** The item, or null for a node to copy. */
        const item<Traits>* value = nullptr;

        /** The tree of the node to copy. */
        const tree* from = nullptr;

        /** The id of the node to copy. */
        std::uint64_t from_id = 0;

        /** Whether the value may be `undefined`: only a root's may. */
        bool may_be_undefined = false;
    };

    /** What an operation writes, gathered before it changes anything. */
    struct change {
        /** The nodes it writes, in the order their blocks are probed and allocated. */
        std::vector<staged> nodes;

        /** The subtrees it erases once the new nodes are in place. */
        std::vector<std::uint64_t> doomed;

        /** Values still to be read into staged nodes. */
        std::vector<pending> work;

        /** Keys transcoded to UTF-16. */
        std::vector<std::vector<char16_t>> keys16;

        /** Copies of what pointed into the image, taken before it moves. */
        std::vector<std::vector<std::byte>> copies;

        /** The id the change's first new node takes. */
        std::uint64_t next_id = 0;

        /** How many new nodes it creates. */
        std::uint64_t created = 0;
    };

    /**
     The node of the smallest id, which is the root, read through the two pointers that reach it,
     the container's header node and the first node of the index by id, each first checked to lie
     inside the image's `size` bytes; null when either does not, or when the tree has no node.
    */
    [[nodiscard]] const node_type* first_by_id(std::uint64_t size) const noexcept {
        const auto& ids = nodes_of()->template get<by_id>();
        const auto begin = reinterpret_cast<std::uintptr_t>(region_->data());
        const auto within = [begin, size](const auto* p) {
            const auto at = reinterpret_cast<std::uintptr_t>(p);
            constexpr std::uint64_t bytes = sizeof(*p);
            return at >= begin + header_bytes && at % alignof(decltype(*p)) == 0 && bytes <= size &&
                   at - begin <= size - bytes;
        };
        // get_node gives an iterator's node without reading it, which no other member does.
        const auto end = ids.end();
        if (!within(end.get_node())) {
            return nullptr;
        }
        const auto first = ids.begin();
        if (first == end || !within(first.get_node())) {
            return nullptr;
        }
        return &*first;
    }

    /** The fewest bytes that hold a header and an empty heap. */
    static std::uint64_t smallest_image() noexcept {
        return header_bytes + heap::get_min_size(sizeof(heap));
    }

    /** `a + b`, or the largest value when that overflows. */
    static std::uint64_t saturated_sum(std::uint64_t a, std::uint64_t b) noexcept {
        return a > std::numeric_limits<std::uint64_t>::max() - b
                   ? std::numeric_limits<std::uint64_t>::max()
                   : a + b;
    }

    /** The size a region grows to for blocks of `total` bytes: its size and twice the blocks. */
    static std::uint64_t grown_size(const region& r, std::uint64_t total) noexcept {
        return saturated_sum(r.size(), saturated_sum(total, total));
    }

    /** The bytes as a `std::string_view`, as the index by key holds keys. */
    static std::string_view as_chars(std::span<const std::byte> bytes) noexcept {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    /** Eight bytes of a scalar, from `at`. */
    static std::uint64_t word(const scalar_type& s, std::size_t at) noexcept {
        return load_little(s.data() + at, 8);
    }

    /** Writes eight bytes of a scalar, from `at`. */
    static void set_word(scalar_type& s, std::size_t at, std::uint64_t value) noexcept {
        store_little(value, s.data() + at, 8);
    }

    /** Whether `n` is a node of kind `k`. */
    static bool is_kind(const node_type* n, kind_t k) noexcept {
        return n != nullptr && n->kind == k;
    }

    /** The node of `id` in `nodes`, or null. */
    static const node_type* find_in(const nodes_type& nodes, std::uint64_t id) noexcept {
        const auto& ids = nodes.template get<by_id>();
        const auto it = ids.find(id);
        return it == ids.end() ? nullptr : &*it;
    }

    /** The child of `object` whose key is `key`, in the traits' encoding, or null. */
    static const node_type* member_in(const nodes_type& nodes, std::uint64_t object,
                                      std::string_view key) noexcept {
        const auto& keys = nodes.template get<by_key>();
        const auto it = keys.find(std::make_tuple(object, key));
        return it == keys.end() ? nullptr : &*it;
    }

    /** The element `index` of `array`, or null. */
    static const node_type* element_in(const nodes_type& nodes, std::uint64_t array,
                                       std::uint64_t index) noexcept {
        const auto& order = nodes.template get<by_order>();
        const auto it = order.find(std::make_tuple(array, index_group, index));
        return it == order.end() ? nullptr : &*it;
    }

    /** The first child of `parent` in `index`, or null. */
    template <class Index>
    static const node_type* first_of(const Index& index, std::uint64_t parent) noexcept {
        const auto it = index.lower_bound(std::make_tuple(parent));
        return it == index.end() || it->parent != parent ? nullptr : &*it;
    }

    /** The node after `child` in `index`, when it has the same parent, or null. */
    template <class Index>
    static const node_type* next_of(const Index& index, const node_type& child) noexcept {
        auto it = index.iterator_to(child);
        ++it;
        return it == index.end() || it->parent != child.parent ? nullptr : &*it;
    }

    /** The container of the attached image, or null when the tree is attached to nothing. */
    [[nodiscard]] nodes_type* nodes_of() const noexcept {
        if (region_ == nullptr || region_->data() == nullptr) {
            return nullptr;
        }
        std::uint64_t offset = 0;
        std::memcpy(&offset, region_->data() + offsetof(image_header, container), sizeof offset);
        return std::launder(reinterpret_cast<nodes_type*>(region_->data() + offset));
    }

    /** The header of the attached image. */
    [[nodiscard]] image_header read_header() const noexcept {
        image_header header{};
        std::memcpy(&header, region_->data(), sizeof header);
        return header;
    }

    /**
     Writes the region's size and the next id into the header, and clears its seal: every change
     to the tree ends here, so a changed image is never sealed.
    */
    void sync_header(std::uint64_t next_id) noexcept {
        image_header header = read_header();
        header.size = region_->size();
        header.next_id = next_id;
        header.seal = 0;
        std::memcpy(region_->data(), &header, sizeof header);
    }

    /** An empty change, its new nodes numbered from the header's next id. */
    [[nodiscard]] change start_change() const noexcept {
        change c;
        c.next_id = read_header().next_id;
        return c;
    }

    /** The id of the change's next new node. */
    static std::uint64_t take_id(change& c) noexcept { return c.next_id + c.created++; }

    /**
     A key, valid UTF-8, in the traits' encoding, held by the change when transcoded; or nothing.
    */
    static std::optional<std::span<const std::byte>> encode_key(change& c, std::string_view utf8) {
        if (!valid_utf8(utf8)) {
            return std::nullopt;
        }
        if constexpr (utf16_text) {
            std::vector<char16_t>& units =
                c.keys16.emplace_back(static_cast<std::size_t>(utf16_length(utf8)));
            to_utf16(utf8, units.data());
            return std::as_bytes(std::span(units));
        } else {
            return std::as_bytes(std::span(utf8.data(), utf8.size()));
        }
    }

    /** The group and ordinal of a new member by the object policy; `next` advances when taken. */
    static std::pair<std::uint8_t, std::uint64_t> member_place(std::string_view utf8_key,
                                                               std::uint64_t id,
                                                               std::uint64_t& next) noexcept {
        if constexpr (sorted_keys) {
            return {key_group, id};
        } else {
            if constexpr (std::same_as<typename Traits::object, javascript_order>) {
                if (const auto index = array_index(utf8_key)) {
                    return {index_group, *index};
                }
            }
            return {key_group, next++};
        }
    }

    /** Stages the value of the node `target`, already there, to be replaced by `v`. */
    void add_existing(change& c, const node_type& target, const item<Traits>& v,
                      bool may_be_undefined) const {
        staged n;
        n.id = target.id;
        n.parent = target.parent;
        n.group = target.group;
        n.ordinal = target.ordinal;
        n.existing = true;
        c.nodes.push_back(n);
        c.work.push_back({c.nodes.size() - 1, &v, nullptr, 0, may_be_undefined});
        // Its children are erased once the new ones are in place.
        const auto& order = nodes_of()->template get<by_order>();
        for (auto it = order.lower_bound(std::make_tuple(target.id));
             it != order.end() && it->parent == target.id; ++it) {
            c.doomed.push_back(it->id);
        }
    }

    /** Stages a new member of `object` holding `v`; its id. */
    static std::uint64_t add_member(change& c, std::uint64_t object, std::string_view utf8_key,
                                    std::span<const std::byte> stored_key, std::uint64_t& next,
                                    const item<Traits>& v) {
        staged n;
        n.id = take_id(c);
        n.parent = object;
        std::tie(n.group, n.ordinal) = member_place(utf8_key, n.id, next);
        n.key = stored_key;
        c.nodes.push_back(n);
        c.work.push_back({c.nodes.size() - 1, &v, nullptr, 0, false});
        return n.id;
    }

    /** Stages a new element of `array` at `index` holding `v`, or `null` when `v` is null. */
    static void add_element(change& c, std::uint64_t array, std::uint64_t index,
                            const item<Traits>* v) {
        staged n;
        n.id = take_id(c);
        n.parent = array;
        n.group = index_group;
        n.ordinal = index;
        c.nodes.push_back(n);
        if (v != nullptr) {
            c.work.push_back({c.nodes.size() - 1, v, nullptr, 0, false});
        }
    }

    /**
     Stages the members of `object` from entries in UTF-8: a repeated key keeps its first place and
     its last value; a key already there is assigned when `assign_existing`, which a new object
     never needs. `added` counts the new keys.
    */
    result<void> place_members(change& c, std::uint64_t object,
                               std::span<const literal_entry<Traits>> entries, std::uint64_t& next,
                               std::uint64_t& added, bool assign_existing) const {
        constexpr std::size_t placed = std::numeric_limits<std::size_t>::max();
        std::map<std::string_view, std::size_t> last;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!valid_utf8(entries[i].key)) {
                return errc::type;
            }
            last.insert_or_assign(entries[i].key, i);
        }
        for (const literal_entry<Traits>& entry : entries) {
            const auto it = last.find(entry.key);
            if (it->second == placed) {
                continue;
            }
            const item<Traits>& value = entries[std::exchange(it->second, placed)].value;
            const auto stored = encode_key(c, entry.key);
            if (!stored.has_value()) {
                return errc::type;
            }
            const node_type* existing =
                assign_existing ? member_in(*nodes_of(), object, as_chars(*stored)) : nullptr;
            if (existing != nullptr) {
                add_existing(c, *existing, value, false);
            } else {
                add_member(c, object, entry.key, *stored, next, value);
                ++added;
            }
        }
        return {};
    }

    /** Reads every pending value into its staged node, staging the nodes of its children. */
    result<void> read_values(change& c) const {
        while (!c.work.empty()) {
            const pending w = c.work.back();
            c.work.pop_back();
            const bool copy = w.value == nullptr || w.value->from != nullptr;
            const result<void> read =
                copy ? read_copy(c, w.slot, w.value == nullptr ? w.from : w.value->from,
                                 w.value == nullptr ? w.from_id : w.value->from_id,
                                 w.may_be_undefined)
                     : read_item(c, w.slot, *w.value, w.may_be_undefined);
            if (!read.has_value()) {
                return read;
            }
        }
        return {};
    }

    /** Reads an item into the staged node `slot`, a literal's entries into new nodes. */
    result<void> read_item(change& c, std::size_t slot, const item<Traits>& v,
                           bool may_be_undefined) const {
        const bool known =
            static_cast<std::uint8_t>(v.kind) <= static_cast<std::uint8_t>(kind_t::object);
        const bool number_held =
            v.kind != kind_t::number || v.number == repr::integer || v.number == repr::number;
        const bool has_text = v.kind == kind_t::string || v.kind == kind_t::bytes;
        if (!known || !number_held || (v.kind == kind_t::undefined && !may_be_undefined) ||
            (v.entry_count != 0 && v.entries == nullptr) ||
            (utf16_text && v.kind == kind_t::string && v.text.size() % sizeof(char16_t) != 0)) {
            return errc::type;
        }
        staged& n = c.nodes[slot];
        n.kind = v.kind;
        n.number = v.kind == kind_t::number ? v.number : repr::none;
        n.scalar = has_text ? scalar_type{} : v.scalar;
        n.text = has_text ? v.text : std::span<const std::byte>{};
        const std::uint64_t id = n.id;
        const std::span<const literal_entry<Traits>> entries(v.entries, v.entry_count);
        scalar_type counts{};
        if (v.kind == kind_t::array) {
            for (std::size_t i = 0; i < entries.size(); ++i) {
                add_element(c, id, i, &entries[i].value);
            }
            set_word(counts, length_at, entries.size());
        } else if (v.kind == kind_t::object) {
            std::uint64_t next = 0;
            std::uint64_t added = 0;
            if (const auto placed = place_members(c, id, entries, next, added, false);
                !placed.has_value()) {
                return placed;
            }
            set_word(counts, next_insertion_at, next);
            set_word(counts, key_count_at, added);
        }
        if (v.kind == kind_t::array || v.kind == kind_t::object) {
            c.nodes[slot].scalar = counts;
        }
        return {};
    }

    /** Reads the node `id` of `from` into the staged node `slot`, its children into new nodes. */
    result<void> read_copy(change& c, std::size_t slot, const tree* from, std::uint64_t id,
                           bool may_be_undefined) const {
        const nodes_type* source_nodes = from == nullptr ? nullptr : from->nodes_of();
        const node_type* source_node =
            source_nodes == nullptr ? nullptr : find_in(*source_nodes, id);
        if (source_node == nullptr ||
            (source_node->kind == kind_t::undefined && !may_be_undefined)) {
            return errc::type;
        }
        staged& n = c.nodes[slot];
        n.kind = source_node->kind;
        n.number = source_node->number;
        n.scalar = source_node->scalar;
        n.text = source_node->text.view();
        const std::uint64_t parent = n.id;
        // The children keep their places: the same traits order them the same way.
        const auto& order = source_nodes->template get<by_order>();
        for (auto it = order.lower_bound(std::make_tuple(id));
             it != order.end() && it->parent == id; ++it) {
            staged child;
            child.id = take_id(c);
            child.parent = parent;
            child.group = it->group;
            child.ordinal = sorted_keys && it->group == key_group ? child.id : it->ordinal;
            child.key = it->key.view();
            c.nodes.push_back(child);
            c.work.push_back({c.nodes.size() - 1, nullptr, from, it->id, false});
        }
        return {};
    }

    /**
     Copies what the change's keys and strings view inside the image, before the image moves: a
     value of this tree, or a key or text a caller read from it.
    */
    void own_views(change& c) const {
        const std::byte* first = region_->data();
        const std::byte* last = first + region_->size();
        const std::less<> before;
        const auto own = [&](std::span<const std::byte>& view) {
            if (view.empty() || !before(view.data(), last) ||
                !before(first, view.data() + view.size())) {
                return;
            }
            view = c.copies.emplace_back(view.begin(), view.end());
        };
        for (staged& n : c.nodes) {
            own(n.key);
            own(n.text);
        }
    }

    /**
     Finds room for the change's blocks, its strings then its nodes, growing an owned region while
     they do not fit; changes nothing but the region's size when there is no room.
    */
    result<void> find_room(change& c) {
        std::vector<std::uint64_t> sizes;
        std::uint64_t total = 0;
        const auto add = [&](std::uint64_t size) {
            sizes.push_back(size);
            total = saturated_sum(total, size);
        };
        for (const staged& n : c.nodes) {
            if (!n.existing) {
                add(n.key.size());
            }
            add(n.text.size());
        }
        for (const staged& n : c.nodes) {
            if (!n.existing) {
                add(node_probe_bytes<Traits>);
            }
        }
        bool owned = false;
        while (!probe(heap_of(*region_), sizes)) {
            if (!region_->is_owned()) {
                return errc::capacity;
            }
            if (!owned) {
                own_views(c);
                owned = true;
            }
            if (!region_->grow(grown_size(*region_, total))) {
                return errc::capacity;
            }
            sync_header(c.next_id);
        }
        return {};
    }

    /** A string of exactly `bytes`, which the probe has found room for. */
    static byte_string make_probed(heap& h, std::span<const std::byte> bytes) {
        auto made = make_string(h, bytes);
        if (!made.has_value()) {
            // The probe has found its room: a failure here is the document's bug.
            boost::throw_exception(std::bad_alloc());
        }
        return made.unsafe_value();
    }

    /**
     Writes a change in its three phases: the room found, the strings then the nodes allocated, then
     what allocates nothing: the doomed set aside, `arrange`, the values replaced, the doomed
     erased.
    */
    template <class Arrange>
    result<void> write(change& c, Arrange&& arrange) {
        if (const auto room = find_room(c); !room.has_value()) {
            return room;
        }
        heap& h = heap_of(*region_);
        std::vector<std::array<byte_string, 2>> made(c.nodes.size());
        for (std::size_t i = 0; i < c.nodes.size(); ++i) {
            if (!c.nodes[i].existing) {
                made[i][0] = make_probed(h, c.nodes[i].key);
            }
            made[i][1] = make_probed(h, c.nodes[i].text);
        }
        set_aside_doomed(c.doomed);
        std::forward<Arrange>(arrange)();
        nodes_type& nodes = *nodes_of();
        for (std::size_t i = 0; i < c.nodes.size(); ++i) {
            const staged& n = c.nodes[i];
            if (!n.existing) {
                [[maybe_unused]] const bool inserted =
                    nodes
                        .emplace(n.id, n.parent, n.group, n.ordinal, made[i][0], n.kind, n.number,
                                 n.scalar, made[i][1])
                        .second;
                BOOST_ASSERT(inserted);
            }
        }
        for (std::size_t i = 0; i < c.nodes.size(); ++i) {
            if (c.nodes[i].existing) {
                replace_value(c.nodes[i], made[i][1]);
            }
        }
        for (const std::uint64_t id : c.doomed) {
            erase_subtree(id);
        }
        sync_header(c.next_id + c.created);
        return {};
    }

    /** Moves the doomed nodes out of their groups, so that new nodes may take their places. */
    void set_aside_doomed(const std::vector<std::uint64_t>& doomed) noexcept {
        auto& ids = nodes_of()->template get<by_id>();
        for (const std::uint64_t id : doomed) {
            ids.modify(ids.find(id), [](node_type& n) noexcept { n.group = set_aside(n.group); });
        }
    }

    /** Swaps a staged value into its node, the old string freed. */
    void replace_value(const staged& value, const byte_string& text) noexcept {
        auto& ids = nodes_of()->template get<by_id>();
        byte_string old;
        ids.modify(ids.find(value.id), [&](node_type& n) noexcept {
            old = n.text;
            n.kind = value.kind;
            n.number = value.number;
            n.scalar = value.scalar;
            n.text = text;
        });
        free_string(heap_of(*region_), old);
    }

    /** Moves the element `from` of `array` to the ordinal `to`, which no element holds. */
    void renumber(std::uint64_t array, std::uint64_t from, std::uint64_t to) noexcept {
        auto& order = nodes_of()->template get<by_order>();
        order.modify(order.find(std::make_tuple(array, index_group, from)),
                     [to](node_type& n) noexcept { n.ordinal = to; });
    }

    /** Replaces a container's scalar. */
    void set_scalar(std::uint64_t id, const scalar_type& s) noexcept {
        auto& ids = nodes_of()->template get<by_id>();
        ids.modify(ids.find(id), [&s](node_type& n) noexcept { n.scalar = s; });
    }

    /** Sets an array's length. */
    void set_length_word(std::uint64_t array, std::uint64_t length) noexcept {
        scalar_type s{};
        set_word(s, length_at, length);
        set_scalar(array, s);
    }

    /** Erases a node and its subtree, children first, each one's strings zeroed and freed. */
    void erase_subtree(std::uint64_t id) {
        nodes_type& nodes = *nodes_of();
        const auto& order = nodes.template get<by_order>();
        // Each node's children are listed after it, so the list read backwards is children first.
        std::vector<std::uint64_t> subtree{id};
        std::size_t listed = 0;
        while (listed < subtree.size()) {
            const std::uint64_t parent = subtree[listed++];
            for (auto it = order.lower_bound(std::make_tuple(parent));
                 it != order.end() && it->parent == parent; ++it) {
                subtree.push_back(it->id);
            }
        }
        auto& ids = nodes.template get<by_id>();
        heap& h = heap_of(*region_);
        for (const std::uint64_t victim : std::views::reverse(subtree)) {
            const auto found = ids.find(victim);
            byte_string key = found->key;
            byte_string text = found->text;
            ids.erase(found);
            free_string(h, key);
            free_string(h, text);
        }
    }

    /**
     Appends `fill` elements holding `null` to an array of `length`, then one holding `*last`
     when it is not null, as one operation.
    */
    result<void> append(std::uint64_t array, std::uint64_t length, std::uint64_t fill,
                        const item<Traits>* last) {
        // More nodes than the region could ever hold is no room, before a change takes memory.
        if (fill >= region_->max_bytes() / node_probe_bytes<Traits>) {
            return errc::capacity;
        }
        change c = start_change();
        for (std::uint64_t i = 0; i < fill; ++i) {
            add_element(c, array, length + i, nullptr);
        }
        if (last != nullptr) {
            add_element(c, array, length + fill, last);
        }
        if (const auto read = read_values(c); !read.has_value()) {
            return read;
        }
        if (const auto done = write(c, [] {}); !done.has_value()) {
            return done;
        }
        set_length_word(array, length + fill + (last != nullptr ? 1 : 0));
        return {};
    }

    region* region_ = nullptr;
};

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_TREE_HPP
