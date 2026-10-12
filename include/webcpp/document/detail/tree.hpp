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

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/tag.hpp>
#include <boost/multi_index_container.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

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
         const std::array<std::byte, scalar_bytes<Traits>>& scalar, byte_string text) noexcept;

    /** The scalar, read by the codec of `T`. */
    template <class T>
    [[nodiscard]] T load() const noexcept;
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
 The key extractor of the index @ref by_key, which gives a node's key bytes as a `std::string_view`.
*/
template <class Traits>
struct key_bytes {
    /** What it extracts. */
    using result_type = std::string_view;

    /** The key's bytes of `n`. */
    std::string_view operator()(const node<Traits>& n) const noexcept;
};

/**
 The order of keys in the index @ref by_key, by the code units of the traits' encoding: the order
 the `sorted` policy writes, which for `utf16` is not the order of the bytes.
*/
template <class Traits>
struct key_less {
    /** Whether `a` comes before `b`, each the bytes of a key in the traits' encoding. */
    bool operator()(std::string_view a, std::string_view b) const noexcept;
};

/**
 The container of a tree's nodes: three ordered indices, never hashed, since keys come from
 untrusted text.
*/
template <class Traits>
using container = boost::multi_index_container<
    node<Traits>,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<by_id>,
            boost::multi_index::member<node<Traits>, std::uint64_t, &node<Traits>::id>>,
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<by_order>,
            boost::multi_index::composite_key<
                node<Traits>,
                boost::multi_index::member<node<Traits>, std::uint64_t, &node<Traits>::parent>,
                boost::multi_index::member<node<Traits>, std::uint8_t, &node<Traits>::group>,
                boost::multi_index::member<node<Traits>, std::uint64_t, &node<Traits>::ordinal>>>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<by_key>,
            boost::multi_index::composite_key<
                node<Traits>,
                boost::multi_index::member<node<Traits>, std::uint64_t, &node<Traits>::parent>,
                key_bytes<Traits>>,
            boost::multi_index::composite_key_compare<std::less<std::uint64_t>, key_less<Traits>>>>,
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
    void store(const T& v) noexcept;
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
constexpr std::uint32_t layout_tag() noexcept;

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
    /**
     Formats a region that holds an empty heap: the header, the container, a root holding `null`.
    */
    static result<tree> format(region& r) noexcept;

    /** Attaches to an image already in a region: checks its header and its heap's sanity. */
    static result<tree> attach(region& r) noexcept;

    /** A tree attached to nothing, which reads as nothing and changes nothing. */
    tree() noexcept = default;

    /** Attaches the tree to `r`, the same image after the region that held it was moved. */
    void rebind(region& r) noexcept;

    /** The region it is attached to. */
    [[nodiscard]] region& storage() const noexcept;

    /** The root's id, @ref root_id. */
    [[nodiscard]] std::uint64_t root() const noexcept;

    /** The node of `id`, or null when there is none. */
    [[nodiscard]] const node<Traits>* find(std::uint64_t id) const noexcept;

    /** The member `key`, in UTF-8, of the object `object`, or null. */
    [[nodiscard]] const node<Traits>* member(std::uint64_t object,
                                             std::string_view key) const noexcept;

    /** The element `index` of the array `array`, or null. */
    [[nodiscard]] const node<Traits>* element(std::uint64_t array,
                                              std::uint64_t index) const noexcept;

    /** The first child of `parent` in the policy's order, or null. */
    [[nodiscard]] const node<Traits>* first_child(std::uint64_t parent) const noexcept;

    /** The child after `child` in its parent, in the policy's order, or null. */
    [[nodiscard]] const node<Traits>* next_sibling(const node<Traits>& child) const noexcept;

    /** Calls `f` with each child of `parent`, a `const node<Traits>&`, in the policy's order. */
    template <class F>
    void for_each_child(std::uint64_t parent, F&& f) const;

    /** An array's length or an object's key count; 0 for any other node. */
    [[nodiscard]] std::uint64_t count(std::uint64_t container_id) const noexcept;

    /** How many nodes the tree holds. */
    [[nodiscard]] std::uint64_t node_count() const noexcept;

    /**
     Replaces the value of node `id` with `v`, its children erased after the new value is in place.
    */
    result<void> assign(std::uint64_t id, const item<Traits>& v) noexcept;

    /**
     Sets the member `key`: an existing one is assigned and keeps its place, a new one takes the
     policy's place; returns its id.
    */
    result<std::uint64_t> set_member(std::uint64_t object, std::string_view key,
                                     const item<Traits>& v) noexcept;

    /** Sets each entry's member in order, as one operation: `Object.assign`. */
    result<void> set_members(std::uint64_t object,
                             std::span<const literal_entry<Traits>> members) noexcept;

    /** Erases the member `key` and its subtree; whether it was there. */
    result<bool> erase_member(std::uint64_t object, std::string_view key) noexcept;

    /**
     Sets the element `index`: assigned when it exists, else the array filled with `null` up to it
     and `v` appended, as one operation.
    */
    result<void> set_element(std::uint64_t array, std::uint64_t index,
                             const item<Traits>& v) noexcept;

    /** Inserts `v` before the element `at`, the later elements renumbered. */
    result<void> insert_elements(std::uint64_t array, std::uint64_t at,
                                 std::span<const item<Traits>> v) noexcept;

    /** Erases `n` elements from `at` and their subtrees, the later elements renumbered. */
    result<void> erase_elements(std::uint64_t array, std::uint64_t at, std::uint64_t n) noexcept;

    /** Erases `remove` elements from `at` and inserts `v` there, as one operation. */
    result<void> splice(std::uint64_t array, std::uint64_t at, std::uint64_t remove,
                        std::span<const item<Traits>> v) noexcept;

    /** Reverses the elements of an array. */
    result<void> reverse(std::uint64_t array) noexcept;

    /** Truncates an array to `n` elements, or appends `null` up to `n`. */
    result<void> set_length(std::uint64_t array, std::uint64_t n) noexcept;

    /**
     Gives an owned region's free end back: the heap shrunk, the region moved into a buffer of the
     image's size.
    */
    result<void> shrink_to_fit() noexcept;

private:
    region* region_ = nullptr;
};

}  // namespace webcpp::document::detail

#endif  // WEBCPP_DOCUMENT_DETAIL_TREE_HPP
