// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The containers' views: an object as a sorted-map-like @ref basic_object_ref, in the policy's key
 order and, through `by_key()`, in lexicographic key order, and an array as a vector-like
 @ref basic_array_ref, each a `std::ranges::bidirectional_range` whose iterators hold node ids.

 `value.hpp` declares `as_object` and `as_array`, which make them, and includes this header to
 define them. What a change leaves valid, which is more than the standard containers leave:

 | Change | Iterators and handles | A `std::string_view` read from a value |
 | --- | --- | --- |
 | An insertion, and any growth of the image | all stay valid | invalid |
 | An object's member erased | those to it and its subtree | invalid |
 | An element inserted or erased | an erased one's; the others follow their elements | invalid |
 | A value replaced by `set` | those to its old subtree | invalid |
 | The document moved from or destroyed | all | invalid |
*/

#ifndef WEBCPP_DOCUMENT_CONTAINERS_HPP
#define WEBCPP_DOCUMENT_CONTAINERS_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/detail/utf.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/traits.hpp>
#include <webcpp/document/value.hpp>

#include <boost/assert.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::document {

namespace detail {

/** The traits and storage of a handle type, and whether it writes. */
template <class Handle>
struct handle_parts {};

/** A read-only handle's parts. */
template <class Traits, class Storage>
struct handle_parts<basic_const_value<Traits, Storage>> {
    /** Its document's traits. */
    using traits = Traits;

    /** Its document's storage. */
    using storage = Storage;

    /** It only reads. */
    static constexpr bool writable = false;
};

/** A handle's parts. */
template <class Traits, class Storage>
struct handle_parts<basic_value<Traits, Storage>> {
    /** Its document's traits. */
    using traits = Traits;

    /** Its document's storage. */
    using storage = Storage;

    /** It writes. */
    static constexpr bool writable = true;
};

/** A handle type, @ref basic_value or @ref basic_const_value, whose views write. */
template <class Handle>
concept writable_handle = handle_parts<Handle>::writable;

/** The order an iterator walks a container's children in. */
enum class walk : std::uint8_t {
    /** The order of the traits' policy: an array's indices, an object's key order. */
    policy,

    /** The index by key's order: by UTF-16 code units, whatever the encoding. */
    keys,
};

/**
 Walks a container's children both ways, and finds the bounds of a key, through the indices of the
 tree's container.

 The tree walks children forward only (`first_child`, `next_sibling`), so a bidirectional iterator
 and a bound walk the indices of the container the tree gives.
*/
template <class Traits>
struct walker {
    /** A node of the tree. */
    using node_type = node<Traits>;

    /** Whether the policy walks an object's members by key. */
    static constexpr bool sorted_keys = std::same_as<typename Traits::object, sorted>;

    /** The container of an attached tree's nodes. */
    static const container<Traits>& nodes_of(const tree<Traits>& t) noexcept { return *t.nodes(); }

    /** Whether a walk of `w` over the children of a node of kind `k` is by key. */
    static bool by_keys(walk w, webcpp::document::kind k) noexcept {
        return w == walk::keys || (sorted_keys && k == webcpp::document::kind::object);
    }

    /** The first child of `parent` in the walk's order, or null. */
    static const node_type* first(const tree<Traits>& t, std::uint64_t parent, walk w) noexcept {
        const node_type* p = t.find(parent);
        if (p == nullptr) {
            return nullptr;
        }
        const container<Traits>& nodes = nodes_of(t);
        return by_keys(w, p->kind) ? first_in(nodes.template get<by_key>(), parent)
                                   : first_in(nodes.template get<by_order>(), parent);
    }

    /** The last child of `parent` in the walk's order, or null. */
    static const node_type* last(const tree<Traits>& t, std::uint64_t parent, walk w) noexcept {
        const node_type* p = t.find(parent);
        if (p == nullptr) {
            return nullptr;
        }
        const container<Traits>& nodes = nodes_of(t);
        return by_keys(w, p->kind) ? last_in(nodes.template get<by_key>(), parent)
                                   : last_in(nodes.template get<by_order>(), parent);
    }

    /** The child after (`forward`) or before `child` in the walk's order, or null. */
    static const node_type* step(const tree<Traits>& t, const node_type& child, walk w,
                                 bool forward) noexcept {
        const container<Traits>& nodes = nodes_of(t);
        // Under sorted, members are all in the key group and elements in the index group.
        const bool keyed = w == walk::keys || (sorted_keys && child.group == key_group);
        return keyed ? step_in(nodes.template get<by_key>(), child, forward)
                     : step_in(nodes.template get<by_order>(), child, forward);
    }

    // A key is transcoded on the process heap with utf16, whose exhaustion ends the program, as
    // the library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     The first member of `object` whose key is not before (`upper` false) or is after (`upper`
     true) `utf8` in key order, or null; null for a key that is not valid UTF-8.
    */
    static const node_type* bound(const tree<Traits>& t, std::uint64_t object,
                                  std::string_view utf8, bool upper) noexcept {
        if (!valid_utf8(utf8)) {
            return nullptr;
        }
        const auto& keys = nodes_of(t).template get<by_key>();
        if constexpr (std::same_as<typename Traits::string, utf16>) {
            std::vector<char16_t> units(static_cast<std::size_t>(utf16_length(utf8)));
            to_utf16(utf8, units.data());
            const std::span<const std::byte> bytes = std::as_bytes(std::span(units));
            return bound_in(keys, object,
                            {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, upper);
        } else {
            return bound_in(keys, object, utf8, upper);
        }
    }

    // NOLINTEND(bugprone-exception-escape)

    /** The first child of `parent` in `index`, or null. */
    template <class Index>
    static const node_type* first_in(const Index& index, std::uint64_t parent) noexcept {
        const auto it = index.lower_bound(std::make_tuple(parent));
        return it == index.end() || it->parent != parent ? nullptr : &*it;
    }

    /** The last child of `parent` in `index`, or null. */
    template <class Index>
    static const node_type* last_in(const Index& index, std::uint64_t parent) noexcept {
        auto it = index.upper_bound(std::make_tuple(parent));
        if (it == index.begin()) {
            return nullptr;
        }
        --it;
        return it->parent != parent ? nullptr : &*it;
    }

    /** The node after or before `child` in `index`, when it has the same parent, or null. */
    template <class Index>
    static const node_type* step_in(const Index& index, const node_type& child,
                                    bool forward) noexcept {
        auto it = index.iterator_to(child);
        if (forward) {
            ++it;
            return it == index.end() || it->parent != child.parent ? nullptr : &*it;
        }
        if (it == index.begin()) {
            return nullptr;
        }
        --it;
        return it->parent != child.parent ? nullptr : &*it;
    }

    /** The bound of the key `stored`, in the traits' encoding, among the members of `object`. */
    template <class Index>
    static const node_type* bound_in(const Index& keys, std::uint64_t object,
                                     std::string_view stored, bool upper) noexcept {
        const auto probe = std::make_tuple(object, stored);
        const auto it = upper ? keys.upper_bound(probe) : keys.lower_bound(probe);
        return it == keys.end() || it->parent != object ? nullptr : &*it;
    }
};

/** The node `id` of `doc` when it is of kind `k`, or null. */
template <class Traits, class Storage>
const node<Traits>* container_node(basic_document<Traits, Storage>* doc, std::uint64_t id,
                                   webcpp::document::kind k) noexcept {
    const tree<Traits>* t = doc == nullptr ? nullptr : access::tree_of(std::as_const(*doc));
    const node<Traits>* n = t == nullptr ? nullptr : t->find(id);
    return n == nullptr || n->kind != k ? nullptr : n;
}

/** Whether `T` is a document, of any traits and storage. */
template <class T>
inline constexpr bool is_document = false;

/** A document is one. */
template <class Traits, class Storage>
inline constexpr bool is_document<basic_document<Traits, Storage>> = true;

/**
 Makes `out` the item of a value a view inserts: a C++ value prepared, or a deep copy of a source's
 value; for a source that reads `undefined`, `null` when `undefined_is_null`, as an array stores
 it, else @ref errc::type, since no member holds it.

 It reads a source through @ref access alone, not through `detail/value_array.hpp`'s helpers,
 which are not yet declared when that header is the one a program includes first.
*/
template <class Traits, class T>
    requires settable<Traits, T>
result<void> prepare_for_view(const T& v, prepared<Traits>& out, bool undefined_is_null) noexcept {
    if constexpr (copy_source<Traits, T>) {
        const tree<Traits>* from = access::tree_of(v);
        if (from == nullptr) {
            return errc::type;
        }
        const node<Traits>* n = nullptr;
        if constexpr (is_document<T>) {
            n = access::node_of(v.root());
        } else {
            n = access::node_of(v);
        }
        if (n == nullptr) {
            if (undefined_is_null) {
                // A prepared value holds null until it is made a copy.
                return {};
            }
            return errc::type;
        }
        out.copy_of(*from, n->id);
        return {};
    } else {
        return prepare(v, out);
    }
}

/**
 An iterator over a container's children, bidirectional, which holds the document's address, the
 container's id and the child's id, and finds the child by its id at each step.
*/
template <class Handle, walk Walk, bool Entries>
class WEBCPP_DOCUMENT_NO_DANGLING child_iterator {
    /** The document's traits. */
    using traits = handle_parts<Handle>::traits;

    /** The document. */
    using document_type = basic_document<traits, typename handle_parts<Handle>::storage>;

public:
    /** What it yields: a pair of the key and the member's handle, or the element's handle. */
    using value_type = std::conditional_t<Entries, std::pair<text_view_t<traits>, Handle>, Handle>;

    /** It yields values, made at each step. */
    using reference = value_type;

    /** The difference of two positions. */
    using difference_type = std::ptrdiff_t;

    /** Bidirectional: a jump of `n` would cost `n` steps of O(log n) each, never O(1). */
    using iterator_concept = std::bidirectional_iterator_tag;

    /** An input iterator to the standard's older algorithms, since it yields values. */
    using iterator_category = std::input_iterator_tag;

    /** What `operator->` returns: the value, held, so that `it->second` reaches it. */
    class arrow {
    public:
        /** Holds `v`. */
        explicit arrow(value_type v) noexcept : v_(std::move(v)) {}

        /** The value held. */
        value_type* operator->() noexcept { return &v_; }

    private:
        value_type v_;
    };

    /** What `operator->` returns. */
    using pointer = arrow;

    /** The end of every range. */
    child_iterator() noexcept = default;

    /** The child `child` of the container `parent` of `doc`, or its end when `child` is 0. */
    child_iterator(document_type* doc, std::uint64_t parent, std::uint64_t child) noexcept
        : doc_(doc), parent_(parent), child_(child) {}

    // A handle of a node holds no key, so making one allocates nothing; were it to, the process
    // heap's exhaustion would end the program, as the library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /** The current child. */
    reference operator*() const noexcept {
        auto h = access::handle_at<Handle>(doc_, child_);
        if constexpr (Entries) {
            // A key is a view of the image, valid until the document's next change.
            const node<traits>* n = tree_ptr() == nullptr ? nullptr : tree_ptr()->find(child_);
            text_view_t<traits> key;
            if (n != nullptr) {
                if constexpr (std::same_as<typename traits::string, utf16>) {
                    key = n->key.units();
                } else {
                    key = n->key.chars();
                }
            }
            return value_type(key, std::move(h));
        } else {
            return h;
        }
    }

    /** The current child, held, for `it->second` and `it->get<T>()`. */
    pointer operator->() const noexcept { return pointer(**this); }

    // NOLINTEND(bugprone-exception-escape)

    /** Moves to the next child, or to the end after the last. */
    child_iterator& operator++() noexcept {
        const tree<traits>* t = tree_ptr();
        const node<traits>* n = t == nullptr ? nullptr : t->find(child_);
        const node<traits>* next =
            n == nullptr ? nullptr : walker<traits>::step(*t, *n, Walk, true);
        child_ = next == nullptr ? 0 : next->id;
        return *this;
    }

    /** Moves to the next child, and returns where it was. */
    child_iterator operator++(int) noexcept {
        child_iterator was = *this;
        ++*this;
        return was;
    }

    /** Moves to the previous child, or to the last from the end. */
    child_iterator& operator--() noexcept {
        const tree<traits>* t = tree_ptr();
        const node<traits>* previous = nullptr;
        if (t != nullptr && child_ == 0) {
            previous = walker<traits>::last(*t, parent_, Walk);
        } else if (const node<traits>* n = t == nullptr ? nullptr : t->find(child_)) {
            previous = walker<traits>::step(*t, *n, Walk, false);
        }
        child_ = previous == nullptr ? 0 : previous->id;
        return *this;
    }

    /** Moves to the previous child, and returns where it was. */
    child_iterator operator--(int) noexcept {
        child_iterator was = *this;
        --*this;
        return was;
    }

    /** Whether two iterators are at the same child of the same container. */
    friend bool operator==(const child_iterator& a, const child_iterator& b) noexcept = default;

    /** The container's id. */
    [[nodiscard]] std::uint64_t parent() const noexcept { return parent_; }

    /** The child's id; 0 at the end. */
    [[nodiscard]] std::uint64_t child() const noexcept { return child_; }

private:
    /** The tree of the document, or null when there is none. */
    [[nodiscard]] const tree<traits>* tree_ptr() const noexcept {
        return doc_ == nullptr ? nullptr : access::tree_of(std::as_const(*doc_));
    }

    document_type* doc_ = nullptr;
    std::uint64_t parent_ = 0;
    std::uint64_t child_ = 0;
};

}  // namespace detail

/**
 The members of an object in lexicographic key order, by UTF-16 code units whatever the encoding,
 with `find`, `lower_bound`, `upper_bound` and `equal_range`: prefix and range queries over keys.

 It reads the tree's index by key, so each step and each bound costs O(log n). Under the `sorted`
 policy it is also the object's own order. It is a `std::ranges::bidirectional_range` and a
 borrowed one, made by @ref basic_object_ref::by_key; its iterators are invalidated as
 @ref basic_object_ref's are.

 @tparam Handle The handle its members are read through, @ref basic_value or
 @ref basic_const_value.
 @see "Key order", in the guide.
*/
template <class Handle>
class WEBCPP_DOCUMENT_NO_DANGLING basic_object_by_key {
public:
    /** The document's traits. */
    using traits_type = detail::handle_parts<Handle>::traits;

    /** The document's storage. */
    using storage_type = detail::handle_parts<Handle>::storage;

    /** A member: its key, a view of the image valid until the next change, and its handle. */
    using value_type = std::pair<detail::text_view_t<traits_type>, Handle>;

    /** What dereferencing an iterator gives: a value, made at each step. */
    using reference = value_type;

    /** A count of members. */
    using size_type = std::uint64_t;

    /** The difference of two positions. */
    using difference_type = std::ptrdiff_t;

    /** A bidirectional iterator in key order. */
    using iterator = detail::child_iterator<Handle, detail::walk::keys, true>;

    /** A reverse iterator in key order. */
    using reverse_iterator = std::reverse_iterator<iterator>;

    /** A view of nothing, which is empty. */
    basic_object_by_key() noexcept = default;

    /**
     How many members the object has.

     @return The count; 0 once it is no object.
    */
    [[nodiscard]] size_type size() const noexcept {
        return detail::container_node(doc_, id_, webcpp::document::kind::object) == nullptr
                   ? 0
                   : detail::access::tree_of(std::as_const(*doc_))->count(id_);
    }

    /**
     Whether the object has no member.

     @return Whether @ref size is 0.
    */
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /**
     The member whose key is first in key order.

     @return The iterator; the end when there is none.
    */
    [[nodiscard]] iterator begin() const noexcept {
        if (detail::container_node(doc_, id_, webcpp::document::kind::object) == nullptr) {
            return end();
        }
        return at(detail::walker<traits_type>::first(*tree_ptr(), id_, detail::walk::keys));
    }

    /**
     The end of the members.

     @return The iterator.
    */
    [[nodiscard]] iterator end() const noexcept { return iterator(doc_, id_, 0); }

    /**
     The member whose key is last in key order, first in reverse.

     @return The reverse iterator.
    */
    [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }

    /**
     The end of the members in reverse.

     @return The reverse iterator.
    */
    [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

    /**
     The member `key`, in O(log n).

     @param key The key, in UTF-8.
     @return Its iterator; the end when it is not there.
    */
    [[nodiscard]] iterator find(std::string_view key) const noexcept {
        if (detail::container_node(doc_, id_, webcpp::document::kind::object) == nullptr) {
            return end();
        }
        return at(tree_ptr()->member(id_, key));
    }

    /**
     The first member whose key is not before `key`, as `std::map::lower_bound`: with `upper_bound`
     or another `lower_bound`, the bounds of a range of keys, such as those with a prefix.

     @param key The key, in UTF-8; it need not be a member's.
     @return The iterator; the end when every key is before `key`, or when `key` is not valid UTF-8.
    */
    [[nodiscard]] iterator lower_bound(std::string_view key) const noexcept {
        return bound(key, false);
    }

    /**
     The first member whose key is after `key`, as `std::map::upper_bound`.

     @param key The key, in UTF-8; it need not be a member's.
     @return The iterator; the end when no key is after `key`, or when `key` is not valid UTF-8.
    */
    [[nodiscard]] iterator upper_bound(std::string_view key) const noexcept {
        return bound(key, true);
    }

    /**
     The members whose key is `key`, as `std::map::equal_range`: one member or none.

     @param key The key, in UTF-8.
     @return The pair of @ref lower_bound and @ref upper_bound.
    */
    [[nodiscard]] std::pair<iterator, iterator> equal_range(std::string_view key) const noexcept {
        return {lower_bound(key), upper_bound(key)};
    }

private:
    template <class>
    friend class basic_object_ref;

    /** The members of the object `id` of `doc`. */
    basic_object_by_key(basic_document<traits_type, storage_type>* doc, std::uint64_t id) noexcept
        : doc_(doc), id_(id) {}

    /** The tree of the document; the object's node must be there. */
    [[nodiscard]] const detail::tree<traits_type>* tree_ptr() const noexcept {
        return detail::access::tree_of(std::as_const(*doc_));
    }

    /** The iterator at `n`, or the end when it is null. */
    [[nodiscard]] iterator at(const detail::node<traits_type>* n) const noexcept {
        return iterator(doc_, id_, n == nullptr ? 0 : n->id);
    }

    /** The lower or upper bound of `key`. */
    [[nodiscard]] iterator bound(std::string_view key, bool upper) const noexcept {
        if (detail::container_node(doc_, id_, webcpp::document::kind::object) == nullptr) {
            return end();
        }
        return at(detail::walker<traits_type>::bound(*tree_ptr(), id_, key, upper));
    }

    basic_document<traits_type, storage_type>* doc_ = nullptr;
    std::uint64_t id_ = 0;
};

/**
 An object as a sorted-map-like container, in the traits' key order (JavaScript's by default):
 `find`, `insert`, `insert_or_assign`, `try_emplace`, `erase`, iteration both ways, and `by_key()`
 for prefix and range queries in lexicographic key order.

 Made by `as_object()` on a handle, it holds the document's address and the object's id, and is
 cheap to copy: a reference, like `std::span`, whose constness is its own, so a `const` view still
 writes; @ref const_object_ref is the view that only reads. It is a
 `std::ranges::bidirectional_range`, a view and a borrowed range, so `<algorithm>` and `<ranges>`
 work on it, and an iterator outlives the view that made it. Its iterators are bidirectional, not
 random access: each step finds the next member in O(log n), so a jump of `n` costs `n` steps.

 Iterators and handles hold ids, which no change reuses:

 | Change | Iterators and handles | A `std::string_view` read from a value |
 | --- | --- | --- |
 | A member inserted, and any growth of the image | all stay valid | invalid |
 | A member erased | those to it and its subtree, which read `undefined` | invalid |
 | A member's value replaced | those to its old subtree | invalid |
 | The document moved from or destroyed | all | invalid |

 What can fail returns `result`: an insertion, which needs room in the image. A lookup returns an
 iterator, the end when nothing is found. There are no throwing forms: they would differ from
 these only in their return type, which C++ does not overload on.

 @tparam Handle The handle its members are read through: @ref basic_value, to write, or
 @ref basic_const_value.
 @see "Handles", in the guide.
 @see "Key order", in the guide.
*/
template <class Handle>
class WEBCPP_DOCUMENT_NO_DANGLING basic_object_ref {
public:
    /** The document's traits. */
    using traits_type = detail::handle_parts<Handle>::traits;

    /** The document's storage. */
    using storage_type = detail::handle_parts<Handle>::storage;

    /** A key as the members take it: UTF-8, whatever the traits' encoding. */
    using key_type = std::string_view;

    /** A member's value: its handle. */
    using mapped_type = Handle;

    /** A member: its key, a view of the image valid until the next change, and its handle. */
    using value_type = std::pair<detail::text_view_t<traits_type>, Handle>;

    /** What dereferencing an iterator gives: a value, made at each step. */
    using reference = value_type;

    /** A count of members. */
    using size_type = std::uint64_t;

    /** The difference of two positions. */
    using difference_type = std::ptrdiff_t;

    /** A bidirectional iterator in the traits' key order. */
    using iterator = detail::child_iterator<Handle, detail::walk::policy, true>;

    /** A reverse iterator in the traits' key order. */
    using reverse_iterator = std::reverse_iterator<iterator>;

    /** A view of nothing, which is empty and fails every insertion with @ref errc::type. */
    basic_object_ref() noexcept = default;

    /**
     How many members the object has, `Object.keys(o).length`.

     @return The count; 0 once it is no object.
    */
    [[nodiscard]] size_type size() const noexcept {
        return self() == nullptr ? 0 : tree_ptr()->count(id_);
    }

    /**
     Whether the object has no member.

     @return Whether @ref size is 0.
    */
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /**
     The member `key`, in O(log n).

     @param key The key, in UTF-8.
     @return Its iterator; the end when it is not there.
    */
    [[nodiscard]] iterator find(std::string_view key) const noexcept {
        if (self() == nullptr) {
            return end();
        }
        return at(tree_ptr()->member(id_, key));
    }

    /**
     Whether the object has the member `key`, `key in o`.

     @param key The key, in UTF-8.
     @return Whether it is there.
    */
    [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != end(); }

    /**
     How many members have the key `key`: 1 or 0, as `std::map::count`.

     @param key The key, in UTF-8.
     @return The count.
    */
    [[nodiscard]] size_type count(std::string_view key) const noexcept {
        return contains(key) ? 1 : 0;
    }

    /**
     Inserts the member `key` holding `v` unless it is there, as `std::map::insert`: a new key
     takes the place the traits' policy gives it, and an existing one is left as it is.

     @tparam T The value's type, any that `set` takes but @ref undefined.
     @param key The key, in UTF-8.
     @param v The value; a handle or a document is copied deeply.
     @return The member's iterator and whether it was inserted; @ref errc::type once this is no
     object, for a key that is not valid UTF-8 and for a source that reads `undefined`,
     @ref errc::capacity when there is no room, and the errors of `set` for the value.
    */
    template <class T>
    [[nodiscard]] result<std::pair<iterator, bool>> insert(std::string_view key,
                                                           const T& v) const noexcept
        requires detail::writable_handle<Handle> && detail::settable<traits_type, T>
    {
        if (self() == nullptr) {
            return errc::type;
        }
        if (const detail::node<traits_type>* m = tree_ptr()->member(id_, key)) {
            return std::pair(at(m), false);
        }
        return put(key, v, true);
    }

    /**
     Inserts the member `key` holding `v`, or assigns `v` to it when it is there, where it keeps
     its place: JavaScript's `o[key] = v`.

     @tparam T The value's type, any that `set` takes but @ref undefined.
     @param key The key, in UTF-8.
     @param v The value; a handle or a document is copied deeply.
     @return The member's iterator and whether it was inserted; the errors of @ref insert.
    */
    template <class T>
    [[nodiscard]] result<std::pair<iterator, bool>> insert_or_assign(std::string_view key,
                                                                     const T& v) const noexcept
        requires detail::writable_handle<Handle> && detail::settable<traits_type, T>
    {
        if (self() == nullptr) {
            return errc::type;
        }
        return put(key, v, tree_ptr()->member(id_, key) == nullptr);
    }

    /**
     Inserts the member `key` holding `v` unless it is there, as `std::map::try_emplace`.

     It is @ref insert, since neither builds a value before the key is known to be absent, so `v`
     is read only when it is stored.

     @tparam T The value's type, any that `set` takes but @ref undefined.
     @param key The key, in UTF-8.
     @param v The value; a handle or a document is copied deeply.
     @return The member's iterator and whether it was inserted; the errors of @ref insert.
    */
    template <class T>
    [[nodiscard]] result<std::pair<iterator, bool>> try_emplace(std::string_view key,
                                                                const T& v) const noexcept
        requires detail::writable_handle<Handle> && detail::settable<traits_type, T>
    {
        return insert(key, v);
    }

    /**
     Erases the member at `pos` and its subtree, `delete o[key]`.

     @param pos An iterator of this object at a member.
     @return The iterator of the member after it; the end when `pos` is the end, of another
     container, or at a member already erased.
    */
    // As a standard container's erase, it is called for its effect as often as for its result.
    // NOLINTNEXTLINE(modernize-use-nodiscard)
    iterator erase(iterator pos) const noexcept
        requires detail::writable_handle<Handle>
    {
        const detail::node<traits_type>* n =
            self() == nullptr || pos.parent() != id_ ? nullptr : tree_ptr()->find(pos.child());
        if (n == nullptr || n->parent != id_) {
            return end();
        }
        iterator next = pos;
        ++next;
        erase_member(*n);
        return next;
    }

    /**
     Erases the members from `first` to `last`, `last` excluded, and their subtrees.

     @param first An iterator of this object.
     @param last An iterator of this object at or after `first`.
     @return `last`.
    */
    // As a standard container's erase, it is called for its effect as often as for its result.
    // NOLINTNEXTLINE(modernize-use-nodiscard)
    iterator erase(iterator first, iterator last) const noexcept
        requires detail::writable_handle<Handle>
    {
        while (first != last && first.child() != 0) {
            first = erase(first);
        }
        return last;
    }

    /**
     Erases the member `key` and its subtree, `delete o[key]`.

     @param key The key, in UTF-8.
     @return How many members were erased, 1 or 0; @ref errc::type once this is no object.
    */
    [[nodiscard]] result<size_type> erase(std::string_view key) const noexcept
        requires detail::writable_handle<Handle>
    {
        if (self() == nullptr) {
            return errc::type;
        }
        const auto erased = writable_tree()->erase_member(id_, key);
        if (!erased.has_value()) {
            return erased.error();
        }
        return erased.unsafe_value() ? 1 : 0;
    }

    /** Erases every member, which needs no room. */
    void clear() const noexcept
        requires detail::writable_handle<Handle>
    {
        erase(begin(), end());
    }

    /**
     The first member in the traits' key order.

     @return The iterator; the end when there is none.
    */
    [[nodiscard]] iterator begin() const noexcept {
        if (self() == nullptr) {
            return end();
        }
        return at(detail::walker<traits_type>::first(*tree_ptr(), id_, detail::walk::policy));
    }

    /**
     The end of the members, which every change leaves valid.

     @return The iterator.
    */
    [[nodiscard]] iterator end() const noexcept { return iterator(doc_, id_, 0); }

    /**
     The last member in the traits' key order, first in reverse.

     @return The reverse iterator.
    */
    [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }

    /**
     The end of the members in reverse.

     @return The reverse iterator.
    */
    [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

    /**
     The members in lexicographic key order, by UTF-16 code units, with bounds for prefix and range
     queries.

     @return The view.
    */
    [[nodiscard]] basic_object_by_key<Handle> by_key() const noexcept {
        return basic_object_by_key<Handle>(doc_, id_);
    }

private:
    template <class, class>
    friend class basic_const_value;
    template <class, class>
    friend class basic_value;

    /** The object `id` of `doc`. */
    basic_object_ref(basic_document<traits_type, storage_type>* doc, std::uint64_t id) noexcept
        : doc_(doc), id_(id) {}

    /** The object's node, or null once it is no object. */
    [[nodiscard]] const detail::node<traits_type>* self() const noexcept {
        return detail::container_node(doc_, id_, webcpp::document::kind::object);
    }

    /** The tree of the document; the object's node must be there. */
    [[nodiscard]] const detail::tree<traits_type>* tree_ptr() const noexcept {
        return detail::access::tree_of(std::as_const(*doc_));
    }

    /** The tree of the document, to write; the object's node must be there. */
    [[nodiscard]] detail::tree<traits_type>* writable_tree() const noexcept {
        return detail::access::tree_of(*doc_);
    }

    /** The iterator at `n`, or the end when it is null. */
    [[nodiscard]] iterator at(const detail::node<traits_type>* n) const noexcept {
        return iterator(doc_, id_, n == nullptr ? 0 : n->id);
    }

    /** Sets the member `key` to `v`; its iterator and `created`. */
    template <class T>
    [[nodiscard]] result<std::pair<iterator, bool>> put(std::string_view key, const T& v,
                                                        bool created) const noexcept {
        detail::prepared<traits_type> p;
        if (const auto made = detail::prepare_for_view(v, p, false); !made.has_value()) {
            return made.error();
        }
        const auto id = writable_tree()->set_member(id_, key, p.value());
        if (!id.has_value()) {
            return id.error();
        }
        return std::pair(iterator(doc_, id_, id.unsafe_value()), created);
    }

    // A UTF-16 key is turned into UTF-8 on the process heap, whose exhaustion ends the program, as
    // the library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /** Erases the member `m` of the object, which needs no room. */
    void erase_member(const detail::node<traits_type>& m) const noexcept {
        // The tree finds the member by its key before it frees anything, so a key read from the
        // image may be given as it is.
        if constexpr (std::same_as<typename traits_type::string, utf16>) {
            const std::u16string_view units = m.key.units();
            std::string key(static_cast<std::size_t>(detail::utf8_size(units)), '\0');
            detail::to_utf8(units, key.data());
            static_cast<void>(writable_tree()->erase_member(id_, key));
        } else {
            static_cast<void>(writable_tree()->erase_member(id_, m.key.chars()));
        }
    }

    // NOLINTEND(bugprone-exception-escape)

    basic_document<traits_type, storage_type>* doc_ = nullptr;
    std::uint64_t id_ = 0;
};

/**
 An array as a vector-like container: `front`, `back`, `operator[]`, `at`, `push_back`,
 `emplace_back`, `pop_back`, `insert`, `erase`, `resize`, `clear` and iteration both ways.

 Made by `as_array()` on a handle, it holds the document's address and the array's id, and is
 cheap to copy: a reference, like `std::span`, whose constness is its own, so a `const` view still
 writes; @ref const_array_ref is the view that only reads. It is a
 `std::ranges::bidirectional_range`, a view and a borrowed range. The tree finds an element by
 its index in O(log n), so `operator[]` costs O(log n), not O(1), and the iterators are
 bidirectional, not random access, whose O(1) jumps they could not keep.

 Iterators and handles hold ids, which no change reuses:

 | Change | Iterators and handles | A `std::string_view` read from a value |
 | --- | --- | --- |
 | An element inserted, and any growth of the image | all follow their elements | invalid |
 | An element erased | those to it and its subtree; the others follow their elements | invalid |
 | An element's value replaced | those to its old subtree | invalid |
 | The document moved from or destroyed | all | invalid |

 What can fail returns `result`: an insertion, which needs room in the image, and `at`. An erasure
 needs no room and cannot fail. There are no throwing forms: they would differ from these only in
 their return type, which C++ does not overload on.

 @tparam Handle The handle its elements are read through: @ref basic_value, to write, or
 @ref basic_const_value.
 @see "Handles", in the guide.
 @see "Arrays", in the guide.
*/
template <class Handle>
class WEBCPP_DOCUMENT_NO_DANGLING basic_array_ref {
public:
    /** The document's traits. */
    using traits_type = detail::handle_parts<Handle>::traits;

    /** The document's storage. */
    using storage_type = detail::handle_parts<Handle>::storage;

    /** An element: its handle. */
    using value_type = Handle;

    /** What dereferencing an iterator gives: a handle, made at each step. */
    using reference = Handle;

    /** A count of elements, or an index. */
    using size_type = std::uint64_t;

    /** The difference of two positions. */
    using difference_type = std::ptrdiff_t;

    /** A bidirectional iterator in index order. */
    using iterator = detail::child_iterator<Handle, detail::walk::policy, false>;

    /** A reverse iterator in index order. */
    using reverse_iterator = std::reverse_iterator<iterator>;

    /** A view of nothing, which is empty and fails every insertion with @ref errc::type. */
    basic_array_ref() noexcept = default;

    /**
     The array's `length`.

     @return The count; 0 once it is no array.
    */
    [[nodiscard]] size_type size() const noexcept {
        return self() == nullptr ? 0 : tree_ptr()->count(id_);
    }

    /**
     Whether the array has no element.

     @return Whether @ref size is 0.
    */
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /**
     The first element.

     @return Its handle; one that names nothing when the array is empty.
    */
    [[nodiscard]] Handle front() const noexcept { return element_or_nothing(0); }

    /**
     The last element.

     @return Its handle; one that names nothing when the array is empty.
    */
    [[nodiscard]] Handle back() const noexcept {
        const size_type n = size();
        return n == 0 ? Handle() : element_or_nothing(n - 1);
    }

    /**
     The element `index`, `a[index]`, in O(log n): the tree finds it by its index.

     @param index The index.
     @return Its handle, as a handle's `operator[]` gives it: past the end, a missing element,
     which `set` creates.
    */
    Handle operator[](size_type index) const noexcept {
        auto self_handle = detail::access::handle_at<Handle>(doc_, id_);
        return self_handle[index];
    }

    /**
     The element `index`, checked, in O(log n).

     @param index The index.
     @return Its handle; @ref errc::range past the end, @ref errc::type once this is no array.
    */
    [[nodiscard]] result<Handle> at(size_type index) const noexcept {
        if (self() == nullptr) {
            return errc::type;
        }
        if (index >= size()) {
            return errc::range;
        }
        return element_or_nothing(index);
    }

    /**
     Appends `v`, `a.push(v)`.

     @tparam T The value's type, any that `set` takes but @ref undefined.
     @param v The value; a handle or a document is copied deeply, `null` for one that reads
     `undefined`.
     @return Success; @ref errc::type once this is no array, @ref errc::capacity when there is no
     room, and the errors of `set` for the value.
    */
    template <class T>
    [[nodiscard]] result<void> push_back(const T& v) const noexcept
        requires detail::writable_handle<Handle> && detail::settable<traits_type, T>
    {
        const auto inserted = insert(end(), v);
        if (!inserted.has_value()) {
            return inserted.error();
        }
        return {};
    }

    /**
     Appends `v` and gives its handle.

     @tparam T The value's type, any that `set` takes but @ref undefined.
     @param v The value; a handle or a document is copied deeply, `null` for one that reads
     `undefined`.
     @return The new element's handle; the errors of @ref push_back.
    */
    template <class T>
    [[nodiscard]] result<Handle> emplace_back(const T& v) const noexcept
        requires detail::writable_handle<Handle> && detail::settable<traits_type, T>
    {
        const auto inserted = insert(end(), v);
        if (!inserted.has_value()) {
            return inserted.error();
        }
        return *inserted.unsafe_value();
    }

    /** Erases the last element, if there is one, which needs no room. */
    void pop_back() const noexcept
        requires detail::writable_handle<Handle>
    {
        const size_type n = size();
        if (n != 0) {
            remove(n - 1, 1);
        }
    }

    /**
     Inserts `v` before `pos`, the later elements moved one index up, as `a.splice(i, 0, v)`.

     @tparam T The value's type, any that `set` takes but @ref undefined.
     @param pos An iterator of this array, the end to append.
     @param v The value; a handle or a document is copied deeply, `null` for one that reads
     `undefined`.
     @return The new element's iterator; @ref errc::type once this is no array or for an iterator
     of another container or at an element already erased, @ref errc::capacity when there is no
     room, and the errors of `set` for the value.
    */
    template <class T>
    [[nodiscard]] result<iterator> insert(iterator pos, const T& v) const noexcept
        requires detail::writable_handle<Handle> && detail::settable<traits_type, T>
    {
        const std::optional<size_type> index = index_of(pos);
        if (!index.has_value()) {
            return errc::type;
        }
        detail::prepared<traits_type> p;
        if (const auto made = detail::prepare_for_view(v, p, true); !made.has_value()) {
            return made.error();
        }
        const detail::item<traits_type> item = p.value();
        detail::tree<traits_type>* t = writable_tree();
        if (const auto done = t->insert_elements(id_, *index, std::span(&item, 1));
            !done.has_value()) {
            return done.error();
        }
        return at_node(t->element(id_, *index));
    }

    /**
     Erases the element at `pos` and its subtree, the later elements moved one index down.

     @param pos An iterator of this array at an element.
     @return The iterator of the element after it; the end when `pos` is the end, of another
     container, or at an element already erased.
    */
    // As a standard container's erase, it is called for its effect as often as for its result.
    // NOLINTNEXTLINE(modernize-use-nodiscard)
    iterator erase(iterator pos) const noexcept
        requires detail::writable_handle<Handle>
    {
        const std::optional<size_type> index = index_of(pos);
        if (!index.has_value() || pos.child() == 0) {
            return end();
        }
        iterator next = pos;
        ++next;
        remove(*index, 1);
        return next;
    }

    /**
     Erases the elements from `first` to `last`, `last` excluded, and their subtrees.

     @param first An iterator of this array.
     @param last An iterator of this array at or after `first`.
     @return `last`, which follows its element; the end when either is of another container or at
     an element already erased, or `last` is before `first`, which erases nothing.
    */
    // As a standard container's erase, it is called for its effect as often as for its result.
    // NOLINTNEXTLINE(modernize-use-nodiscard)
    iterator erase(iterator first, iterator last) const noexcept
        requires detail::writable_handle<Handle>
    {
        const std::optional<size_type> from = index_of(first);
        const std::optional<size_type> to = index_of(last);
        if (!from.has_value() || !to.has_value() || *to < *from) {
            return end();
        }
        remove(*from, *to - *from);
        return last;
    }

    /**
     Truncates the array to `n` elements, or appends `null` up to `n`: `a.length = n`.

     @param n The new length.
     @return Success; @ref errc::type once this is no array, @ref errc::capacity when there is no
     room.
    */
    [[nodiscard]] result<void> resize(size_type n) const noexcept
        requires detail::writable_handle<Handle>
    {
        if (self() == nullptr) {
            return errc::type;
        }
        return writable_tree()->set_length(id_, n);
    }

    /** Erases every element, which needs no room. */
    void clear() const noexcept
        requires detail::writable_handle<Handle>
    {
        remove(0, size());
    }

    /**
     The first element.

     @return The iterator; the end when there is none.
    */
    [[nodiscard]] iterator begin() const noexcept {
        if (self() == nullptr) {
            return end();
        }
        return at_node(tree_ptr()->element(id_, 0));
    }

    /**
     The end of the elements, which every change leaves valid.

     @return The iterator.
    */
    [[nodiscard]] iterator end() const noexcept { return iterator(doc_, id_, 0); }

    /**
     The last element, first in reverse.

     @return The reverse iterator.
    */
    [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }

    /**
     The end of the elements in reverse.

     @return The reverse iterator.
    */
    [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }

private:
    template <class, class>
    friend class basic_const_value;
    template <class, class>
    friend class basic_value;

    /** The array `id` of `doc`. */
    basic_array_ref(basic_document<traits_type, storage_type>* doc, std::uint64_t id) noexcept
        : doc_(doc), id_(id) {}

    /** The array's node, or null once it is no array. */
    [[nodiscard]] const detail::node<traits_type>* self() const noexcept {
        return detail::container_node(doc_, id_, webcpp::document::kind::array);
    }

    /** The tree of the document; the array's node must be there. */
    [[nodiscard]] const detail::tree<traits_type>* tree_ptr() const noexcept {
        return detail::access::tree_of(std::as_const(*doc_));
    }

    /** The tree of the document, to write; the array's node must be there. */
    [[nodiscard]] detail::tree<traits_type>* writable_tree() const noexcept {
        return detail::access::tree_of(*doc_);
    }

    /** The iterator at `n`, or the end when it is null. */
    [[nodiscard]] iterator at_node(const detail::node<traits_type>* n) const noexcept {
        return iterator(doc_, id_, n == nullptr ? 0 : n->id);
    }

    /** The handle of the element `index`, or one that names nothing. */
    [[nodiscard]] Handle element_or_nothing(size_type index) const noexcept {
        const detail::node<traits_type>* n =
            self() == nullptr ? nullptr : tree_ptr()->element(id_, index);
        return n == nullptr ? Handle() : detail::access::handle_at<Handle>(doc_, n->id);
    }

    /**
     The index of `pos`: its element's, or the length for the end; nothing once this is no array,
     or for an iterator of another container or at an element already erased.
    */
    [[nodiscard]] std::optional<size_type> index_of(iterator pos) const noexcept {
        if (self() == nullptr || pos.parent() != id_) {
            return std::nullopt;
        }
        if (pos.child() == 0) {
            return size();
        }
        const detail::node<traits_type>* n = tree_ptr()->find(pos.child());
        if (n == nullptr || n->parent != id_) {
            return std::nullopt;
        }
        return n->ordinal;
    }

    /** Erases `n` elements from `index`, which needs no room. */
    void remove(size_type index, size_type n) const noexcept {
        if (n == 0) {
            return;
        }
        // A removal allocates nothing in the image, so its probe always finds room.
        [[maybe_unused]] const auto removed = writable_tree()->erase_elements(id_, index, n);
        BOOST_ASSERT(removed.has_value());
    }

    basic_document<traits_type, storage_type>* doc_ = nullptr;
    std::uint64_t id_ = 0;
};

/** A view of an object of a @ref document, reading and writing. */
using object_ref = basic_object_ref<value>;

/** A view of an object of a @ref document, read only. */
using const_object_ref = basic_object_ref<const_value>;

/** A view of an array of a @ref document, reading and writing. */
using array_ref = basic_array_ref<value>;

/** A view of an array of a @ref document, read only. */
using const_array_ref = basic_array_ref<const_value>;

// The members below make views, which are complete only here.

template <class Traits, class Storage>
result<basic_object_ref<basic_const_value<Traits, Storage>>>
basic_const_value<Traits, Storage>::as_object() const noexcept {
    const std::uint64_t id = id_of(webcpp::document::kind::object);
    if (id == 0) {
        return errc::type;
    }
    return basic_object_ref<basic_const_value>(doc_, id);
}

template <class Traits, class Storage>
result<basic_array_ref<basic_const_value<Traits, Storage>>>
basic_const_value<Traits, Storage>::as_array() const noexcept {
    const std::uint64_t id = id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    return basic_array_ref<basic_const_value>(doc_, id);
}

template <class Traits, class Storage>
result<basic_object_ref<basic_value<Traits, Storage>>>
basic_value<Traits, Storage>::as_object() noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::object);
    if (id == 0) {
        return errc::type;
    }
    return basic_object_ref<basic_value>(this->doc_, id);
}

template <class Traits, class Storage>
result<basic_array_ref<basic_value<Traits, Storage>>>
basic_value<Traits, Storage>::as_array() noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    return basic_array_ref<basic_value>(this->doc_, id);
}

}  // namespace webcpp::document

// The views are views and borrowed ranges: each holds two words, and its iterators hold the
// document's address and ids, not the view. The standard lets a program specialize both for its own
// types, which the check of the std namespace cannot tell from a modification.
// NOLINTNEXTLINE(bugprone-std-namespace-modification,cert-dcl58-cpp)
namespace std::ranges {

template <class Handle>
inline constexpr bool enable_view<webcpp::document::basic_object_ref<Handle>> = true;

template <class Handle>
inline constexpr bool enable_view<webcpp::document::basic_object_by_key<Handle>> = true;

template <class Handle>
inline constexpr bool enable_view<webcpp::document::basic_array_ref<Handle>> = true;

template <class Handle>
inline constexpr bool enable_borrowed_range<webcpp::document::basic_object_ref<Handle>> = true;

template <class Handle>
inline constexpr bool enable_borrowed_range<webcpp::document::basic_object_by_key<Handle>> = true;

template <class Handle>
inline constexpr bool enable_borrowed_range<webcpp::document::basic_array_ref<Handle>> = true;

}  // namespace std::ranges

#endif  // WEBCPP_DOCUMENT_CONTAINERS_HPP
