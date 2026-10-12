// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The handles of a document: a place in it, read through @ref basic_const_value and read and
 written through @ref basic_value, with JavaScript's property access, assignment, `delete`, the
 array methods and iteration.

 This header declares every member of the handles. It defines reading and writing; the array
 methods are defined in `detail/value_array.hpp`, the views `as_object` and `as_array` make in
 `containers.hpp`, the sugar that throws in `detail/value_sugar.hpp` and `stringify` in
 `stringify.hpp`, each included at its end.
*/

#ifndef WEBCPP_DOCUMENT_VALUE_HPP
#define WEBCPP_DOCUMENT_VALUE_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/detail/convert.hpp>
#include <webcpp/document/detail/fwd.hpp>
#include <webcpp/document/detail/tree.hpp>
#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/kind.hpp>
#include <webcpp/document/literal.hpp>
#include <webcpp/document/traits.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::document {

namespace detail {

/**
 What a handle names: a node, a missing member of an object, a missing element of an array, or
 nothing.
*/
enum class place : std::uint8_t {
    /** A node that exists, by its id; it reads `undefined` once erased. */
    node,

    /** A member of the object of id `id` that is not there, by its key. */
    member,

    /** An element of the array of id `id` past its end, by its index. */
    element,

    /** Nothing: navigation through `undefined`, a scalar, or a moved-from document. */
    nothing,
};

/** Whether `T` is a document or a handle, and of which traits: a source of a deep copy. */
template <class T>
struct source_of {
    /** It is not. */
    static constexpr bool value = false;
};

/** A read-only handle is a source of a deep copy. */
template <class Traits, class Storage>
struct source_of<basic_const_value<Traits, Storage>> {
    /** It is. */
    static constexpr bool value = true;

    /** Its traits. */
    using traits = Traits;
};

/** A handle is a source of a deep copy. */
template <class Traits, class Storage>
struct source_of<basic_value<Traits, Storage>> {
    /** It is. */
    static constexpr bool value = true;

    /** Its traits. */
    using traits = Traits;
};

/** A document is a source of a deep copy, its value. */
template <class Traits, class Storage>
struct source_of<basic_document<Traits, Storage>> {
    /** It is. */
    static constexpr bool value = true;

    /** Its traits. */
    using traits = Traits;
};

/** A document or a handle of the same traits, of any storage, whose value `set` copies deeply. */
template <class Traits, class T>
concept copy_source = source_of<T>::value && std::same_as<typename source_of<T>::traits, Traits>;

/**
 What `set` and the array methods take: what @ref prepare takes, and a copy source; `set` also
 takes @ref undefined.
*/
template <class Traits, class T>
concept settable = preparable<Traits, T> || copy_source<Traits, T>;

/** What a range of a container's children yields. */
enum class children_of : std::uint8_t {
    /** Pairs of a key and a read-only handle, as `Object.entries`. */
    entries,

    /** Keys, as `Object.keys`. */
    keys,

    /** Read-only handles, as `Object.values` and an array's elements. */
    values,
};

/**
 An iterator over a container's children, in the policy's order, which finds the next child by its
 id at each step.
*/
template <class Traits, class Storage, children_of Kind>
class WEBCPP_DOCUMENT_NO_DANGLING children_iterator {
public:
    /** What it yields: a pair of the key and the handle, a key, or a handle. */
    using value_type =
        std::conditional_t<Kind == children_of::entries,
                           std::pair<text_view_t<Traits>, basic_const_value<Traits, Storage>>,
                           std::conditional_t<Kind == children_of::keys, text_view_t<Traits>,
                                              basic_const_value<Traits, Storage>>>;

    /** The difference of two positions. */
    using difference_type = std::ptrdiff_t;

    /** A forward iterator whose values are made at each step. */
    using iterator_concept = std::forward_iterator_tag;

    /** The end of every range. */
    children_iterator() noexcept = default;

    /** The child `child` of a container of `doc`, or the end when `child` is 0. */
    children_iterator(basic_document<Traits, Storage>* doc, std::uint64_t child) noexcept
        : doc_(doc), child_(child) {}

    /** The current child. */
    value_type operator*() const noexcept;

    /** Moves to the next child. */
    children_iterator& operator++() noexcept;

    /** Moves to the next child, and returns where it was. */
    children_iterator operator++(int) noexcept {
        children_iterator was = *this;
        ++*this;
        return was;
    }

    /** Whether two iterators are at the same child, or both at the end. */
    friend bool operator==(const children_iterator& a, const children_iterator& b) noexcept {
        return a.child_ == b.child_;
    }

private:
    basic_document<Traits, Storage>* doc_ = nullptr;
    std::uint64_t child_ = 0;
};

/** The range of a container's children: empty for any other kind. */
template <class Traits, class Storage, children_of Kind>
class WEBCPP_DOCUMENT_NO_DANGLING children {
public:
    /** The children of the node `parent` of `doc`; none when `doc` is null or `parent` is 0. */
    children(basic_document<Traits, Storage>* doc, std::uint64_t parent) noexcept
        : doc_(doc), parent_(parent) {}

    /** The first child. */
    [[nodiscard]] children_iterator<Traits, Storage, Kind> begin() const noexcept;

    /** The end. */
    [[nodiscard]] children_iterator<Traits, Storage, Kind> end() const noexcept { return {}; }

private:
    basic_document<Traits, Storage>* doc_ = nullptr;
    std::uint64_t parent_ = 0;
};

/** What the library's other parts reach inside documents and handles: their tree and their node. */
struct access {
    /** The tree of a document, or null when it was moved from. */
    template <class Traits, class Storage>
    static tree<Traits>* tree_of(basic_document<Traits, Storage>& d) noexcept {
        return d.region_.data() == nullptr ? nullptr : &d.tree_;
    }

    /** The tree of a document, or null when it was moved from. */
    template <class Traits, class Storage>
    static const tree<Traits>* tree_of(const basic_document<Traits, Storage>& d) noexcept {
        return d.region_.data() == nullptr ? nullptr : &d.tree_;
    }

    /** The tree of a handle's document, or null when it names nothing. */
    template <class Traits, class Storage>
    static const tree<Traits>* tree_of(const basic_const_value<Traits, Storage>& v) noexcept {
        return v.tree_ptr();
    }

    /** The node a handle names, or null when it reads `undefined`. */
    template <class Traits, class Storage>
    static const node<Traits>* node_of(const basic_const_value<Traits, Storage>& v) noexcept {
        return v.resolve();
    }

    /** The read-only handle of the node `id` of a document. */
    template <class Traits, class Storage>
    static basic_const_value<Traits, Storage> handle_of(basic_document<Traits, Storage>& d,
                                                        std::uint64_t id) noexcept {
        return basic_const_value<Traits, Storage>(&d, place::node, id, 0, {});
    }

    /** The handle, of type `Handle`, of the node `id` of `d`; one that names nothing when null. */
    template <class Handle, class Traits, class Storage>
    static Handle handle_at(basic_document<Traits, Storage>* d, std::uint64_t id) noexcept {
        return Handle(d, place::node, id, 0, {});
    }

    /** The document of a region whose tree was formatted or attached, the tree rebound to it. */
    template <class Traits, class Storage>
    static basic_document<Traits, Storage> make_document(region r) noexcept {
        return basic_document<Traits, Storage>(std::move(r));
    }
};

}  // namespace detail

/**
 A read-only handle: one place in one document, cheap to copy, which reads what is there now.

 A handle names a node, a missing member or element of a container that exists (it reads
 `undefined`, and @ref basic_value::set creates it), or nothing (it reads `undefined`, and `set`
 fails with @ref errc::type). It holds the document's address and node ids, so it stays valid when
 the document grows and its image moves; it reads `undefined` once its node is erased, and is
 invalid once its document is moved from or destroyed.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @see "Handles", in the guide.
*/
template <class Traits, class Storage>
class WEBCPP_DOCUMENT_NO_DANGLING basic_const_value {
public:
    /** A handle that names nothing. */
    basic_const_value() noexcept = default;

    /**
     The kind of the value, JavaScript's `typeof` with `null`, arrays, dates and bytes told apart.

     @return The kind; @ref kind::undefined when nothing is there.
    */
    [[nodiscard]] webcpp::document::kind kind() const noexcept {
        const detail::node<Traits>* n = resolve();
        return n == nullptr ? webcpp::document::kind::undefined : n->kind;
    }

    /**
     Whether nothing is there, `v === undefined`.

     @return Whether the kind is @ref kind::undefined.
    */
    [[nodiscard]] bool is_undefined() const noexcept {
        return kind() == webcpp::document::kind::undefined;
    }

    /**
     Whether the value is `null`.

     @return Whether the kind is @ref kind::null.
    */
    [[nodiscard]] bool is_null() const noexcept { return kind() == webcpp::document::kind::null; }

    /**
     Whether the value is `true` or `false`.

     @return Whether the kind is @ref kind::boolean.
    */
    [[nodiscard]] bool is_boolean() const noexcept {
        return kind() == webcpp::document::kind::boolean;
    }

    /**
     Whether the value is a number.

     @return Whether the kind is @ref kind::number.
    */
    [[nodiscard]] bool is_number() const noexcept {
        return kind() == webcpp::document::kind::number;
    }

    /**
     Whether the value is a number that is an integer, JavaScript's `Number.isInteger`.

     @return Whether it is a number with no fractional part.
    */
    [[nodiscard]] bool is_integer() const noexcept {
        const detail::node<Traits>* n = resolve();
        if (n == nullptr || n->kind != webcpp::document::kind::number) {
            return false;
        }
        if (n->number == detail::repr::integer) {
            return true;
        }
        // One held as `number` is still an integer when it is too large for `integer`, as 1e30 is.
        using codec = number_codec<typename Traits::number>;
        const auto v = n->template load<typename Traits::number>();
        if (!codec::finite(v)) {
            return false;
        }
        const decimal_digits d = codec::digits(v);
        return d.count == 0 || std::cmp_greater_equal(d.point, d.count);
    }

    /**
     Whether the value is a string.

     @return Whether the kind is @ref kind::string.
    */
    [[nodiscard]] bool is_string() const noexcept {
        return kind() == webcpp::document::kind::string;
    }

    /**
     Whether the value is a date.

     @return Whether the kind is @ref kind::date.
    */
    [[nodiscard]] bool is_date() const noexcept { return kind() == webcpp::document::kind::date; }

    /**
     Whether the value is bytes.

     @return Whether the kind is @ref kind::bytes.
    */
    [[nodiscard]] bool is_bytes() const noexcept { return kind() == webcpp::document::kind::bytes; }

    /**
     Whether the value is an array, `Array.isArray`.

     @return Whether the kind is @ref kind::array.
    */
    [[nodiscard]] bool is_array() const noexcept { return kind() == webcpp::document::kind::array; }

    /**
     Whether the value is a plain object.

     @return Whether the kind is @ref kind::object.
    */
    [[nodiscard]] bool is_object() const noexcept {
        return kind() == webcpp::document::kind::object;
    }

    /**
     Reads the value as `T`.

     `T` is `bool`, any integer type, a decimal type, the traits' `number`, `std::string`,
     `std::string_view` (with `utf8`), `std::u16string`, `std::u16string_view` (with `utf16`),
     @ref date_ms, `std::vector<std::byte>` or `std::span<const std::byte>`. A view is valid until
     the document's next change. A string reads as a date when it is exactly Date.prototype.toJSON's
     text, and as bytes when it is text of the traits' bytes policy.

     @tparam T The type.
     @return The value; @ref errc::type when nothing is there or for another kind,
     @ref errc::range for a number that is not an integer, does not fit `T` or that `T` cannot hold
     exactly.
    */
    template <class T>
    [[nodiscard]] result<T> get() const noexcept
        requires detail::readable<Traits, T>
    {
        const detail::tree<Traits>* t = tree_ptr();
        if (t == nullptr) {
            return errc::type;
        }
        return detail::read<T>(*t, resolve());
    }

    /**
     The value as `T`, or `fallback`: JavaScript's `v ?? fallback`, and the fallback too when the
     value cannot be read as `T`.

     @tparam T The type, as @ref get takes it.
     @param fallback The value when the value is `undefined` or `null`, or not readable as `T`.
     @return The value or the fallback.
    */
    template <class T>
    [[nodiscard]] T value_or(T fallback) const noexcept
        requires detail::readable<Traits, T>
    {
        result<T> read = get<T>();
        if (read.has_value()) {
            return std::move(read.unsafe_value());
        }
        return fallback;
    }

    /**
     Whether an object has the member `key`, `key in v`.

     @param key The key, in UTF-8.
     @return Whether it is there; false for any other kind.
    */
    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        const detail::node<Traits>* n = resolve();
        return n != nullptr && n->kind == webcpp::document::kind::object &&
               tree_ptr()->member(n->id, key) != nullptr;
    }

    /**
     An object's key count or an array's length.

     @return The count; 0 for any other kind.
    */
    [[nodiscard]] std::uint64_t size() const noexcept {
        const detail::node<Traits>* n = resolve();
        return n == nullptr ? 0 : tree_ptr()->count(n->id);
    }

    /**
     An array's `length`, or a string's in UTF-16 code units, as JavaScript counts it.

     @return The length; @ref errc::type for any other kind.
    */
    [[nodiscard]] result<std::uint64_t> length() const noexcept {
        const detail::node<Traits>* n = resolve();
        if (n == nullptr) {
            return errc::type;
        }
        if (n->kind == webcpp::document::kind::array) {
            return tree_ptr()->count(n->id);
        }
        if (n->kind != webcpp::document::kind::string) {
            return errc::type;
        }
        if constexpr (std::same_as<typename Traits::string, utf16>) {
            return n->text.units().size();
        } else {
            return detail::utf16_length(n->text.chars());
        }
    }

    /**
     The member `key`, `v.key`, which never fails.

     @param key The key, in UTF-8.
     @return Its handle: the member, a missing member of an object, or nothing.
    */
    basic_const_value operator[](std::string_view key) const noexcept {
        return member_handle<basic_const_value>(key);
    }

    /**
     The element `index`, `v[index]`, which never fails.

     @param index The index.
     @return Its handle: the element, a missing element of an array, or nothing.
    */
    basic_const_value operator[](std::uint64_t index) const noexcept {
        return element_handle<basic_const_value>(index);
    }

    /**
     `Array.prototype.at`: the element `index`, a negative index counting from the end.

     @param index The index.
     @return Its handle; nothing out of range or for any other kind.
    */
    [[nodiscard]] basic_const_value at(std::int64_t index) const noexcept;

    /**
     `Object.entries`: pairs of each key and its value, in the policy's order.

     @return The range; empty for any other kind than an object.
    */
    [[nodiscard]] detail::children<Traits, Storage, detail::children_of::entries> entries()
        const noexcept {
        return {doc_, id_of(webcpp::document::kind::object)};
    }

    /**
     `Object.keys`: each key, in the policy's order; a `std::string_view` with `utf8`, a
     `std::u16string_view` with `utf16`.

     @return The range; empty for any other kind than an object.
     @see "Key order", in the guide.
    */
    [[nodiscard]] detail::children<Traits, Storage, detail::children_of::keys> keys()
        const noexcept {
        return {doc_, id_of(webcpp::document::kind::object)};
    }

    /**
     `Object.values`: each value, in the policy's order.

     @return The range; empty for any other kind than an object.
    */
    [[nodiscard]] detail::children<Traits, Storage, detail::children_of::values> values()
        const noexcept {
        return {doc_, id_of(webcpp::document::kind::object)};
    }

    /**
     The object as a sorted-map-like container that reads: `find`, bounds by key, and iterators
     that `<algorithm>` and `<ranges>` take.

     @return The view, defined in `containers.hpp`; @ref errc::type for any other kind.
     @see "Key order", in the guide.
    */
    [[nodiscard]] result<basic_object_ref<basic_const_value>> as_object() const noexcept;

    /**
     The array as a vector-like container that reads: `front`, `back`, `at`, and iterators that
     `<algorithm>` and `<ranges>` take.

     @return The view, defined in `containers.hpp`; @ref errc::type for any other kind.
     @see "Arrays", in the guide.
    */
    [[nodiscard]] result<basic_array_ref<basic_const_value>> as_array() const noexcept;

    /**
     The first element of an array, for `for (auto v : arr)`.

     @return The iterator; the end for any other kind.
    */
    [[nodiscard]] detail::children_iterator<Traits, Storage, detail::children_of::values> begin()
        const noexcept {
        return detail::children<Traits, Storage, detail::children_of::values>(
                   doc_, id_of(webcpp::document::kind::array))
            .begin();
    }

    /**
     The end of an array's elements.

     @return The iterator.
    */
    [[nodiscard]] detail::children_iterator<Traits, Storage, detail::children_of::values> end()
        const noexcept {
        return {};
    }

    /**
     `Array.prototype.slice`: a copy of the elements from `start` to `stop`, each a negative index
     counting from the end, clamped to the length.

     @param start The first index.
     @param stop The index after the last.
     @return A new owned document holding the array of the copies; @ref errc::type for any other
     kind, @ref errc::capacity when it does not fit.
    */
    [[nodiscard]] result<basic_document<Traits, owned>> slice(
        std::int64_t start = 0,
        std::int64_t stop = std::numeric_limits<std::int64_t>::max()) const noexcept;

    /**
     `Array.prototype.concat`: this array, then each argument, an array's elements or the value.

     @tparam T The arguments' types, each settable.
     @param v The arguments.
     @return A new owned document holding the array; @ref errc::type for any other kind,
     @ref errc::capacity when it does not fit.
    */
    template <class... T>
    [[nodiscard]] result<basic_document<Traits, owned>> concat(const T&... v) const noexcept
        requires(detail::settable<Traits, T> && ...);

    /**
     The value as `JSON.stringify(value, null, indent)` writes it.

     @param indent The spaces of each level, clamped to 10; 0 writes compactly.
     @return The text; empty when the value is `undefined`.
    */
    [[nodiscard]] std::string stringify(std::uint32_t indent = 0) const;

    /**
     Reads the value as `T`, as @ref get does, throwing on failure; only with exceptions.

     @tparam T The type.
     @return The value.
     @throws type_error For another kind.
     @throws range_error For a number that does not fit.
     @see "The sugar that throws", in the guide.
    */
    template <class T>
    [[nodiscard]] T as() const
        requires detail::exceptions_enabled<T> && detail::readable<Traits, T>;

    /**
     Reads a string as `std::string_view`, throwing on failure; only with exceptions.

     @tparam C `char`, a parameter only so that the call is checked where it is made.
     @return The string, valid until the document's next change.
     @throws type_error For another kind.
    */
    template <class C = char>
    [[nodiscard]] std::string_view as_string() const
        requires detail::exceptions_enabled<C>;

    /**
     Reads a boolean, throwing on failure; only with exceptions.

     @tparam B `bool`, a parameter only so that the call is checked where it is made.
     @return The boolean.
     @throws type_error For another kind.
    */
    template <class B = bool>
    [[nodiscard]] bool as_bool() const
        requires detail::exceptions_enabled<B>;

    /**
     Whether two values are equal, structurally: the same kind; numbers by value (`1`, `1.0` and
     `1e0` are equal); strings, dates and bytes by value; arrays element by element; objects with
     the same keys, each with an equal value, in any order.

     Two `undefined` are equal; JavaScript has no deep equality, and this one is the document's.

     @param a A handle.
     @param b A handle.
     @return Whether their values are equal.
    */
    friend bool operator==(const basic_const_value& a, const basic_const_value& b) noexcept {
        return equals(a, b);
    }

private:
    // A missing member's key is held on the process heap, whose exhaustion ends the program, as
    // the library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     A handle of `doc` at the place `where`: the node or container `id`, and a missing element's
     `index` or member's `key`.
    */
    basic_const_value(basic_document<Traits, Storage>* doc, detail::place where, std::uint64_t id,
                      std::uint64_t index, std::string_view key) noexcept
        // A handle that names nothing holds no document, so that nothing reaches one through it.
        : doc_(where == detail::place::nothing ? nullptr : doc),
          id_(where == detail::place::nothing ? 0 : id),
          index_(where == detail::place::element ? index : 0),
          key_(where == detail::place::member ? key : std::string_view()),
          place_(doc_ == nullptr ? detail::place::nothing : where) {}

    // NOLINTEND(bugprone-exception-escape)

    /** The tree of the handle's document, or null when it names nothing or the document none. */
    [[nodiscard]] const detail::tree<Traits>* tree_ptr() const noexcept {
        return doc_ == nullptr ? nullptr : detail::access::tree_of(std::as_const(*doc_));
    }

    /** The node the handle names now, or null when it reads `undefined`. */
    [[nodiscard]] const detail::node<Traits>* resolve() const noexcept {
        const detail::tree<Traits>* t = tree_ptr();
        if (t == nullptr) {
            return nullptr;
        }
        // A missing member or element is looked for again: it reads what is there now.
        const detail::node<Traits>* n = nullptr;
        switch (place_) {
            case detail::place::node: n = t->find(id_); break;
            case detail::place::member: n = t->member(id_, key_); break;
            case detail::place::element: n = t->element(id_, index_); break;
            case detail::place::nothing: break;
        }
        // Only a root holds undefined, which reads as nothing there.
        return n == nullptr || n->kind == webcpp::document::kind::undefined ? nullptr : n;
    }

    /** The id of the node the handle names when it is of kind `k`, else 0. */
    [[nodiscard]] std::uint64_t id_of(webcpp::document::kind k) const noexcept {
        const detail::node<Traits>* n = resolve();
        return n == nullptr || n->kind != k ? 0 : n->id;
    }

    /** The handle, of type `Handle`, of the member `key` of what this handle names. */
    template <class Handle>
    [[nodiscard]] Handle member_handle(std::string_view key) const noexcept {
        const detail::node<Traits>* n = resolve();
        if (n == nullptr || n->kind != webcpp::document::kind::object) {
            return Handle(nullptr, detail::place::nothing, 0, 0, {});
        }
        if (const detail::node<Traits>* m = tree_ptr()->member(n->id, key)) {
            return Handle(doc_, detail::place::node, m->id, 0, {});
        }
        return Handle(doc_, detail::place::member, n->id, 0, key);
    }

    /** The handle, of type `Handle`, of the element `index` of what this handle names. */
    template <class Handle>
    [[nodiscard]] Handle element_handle(std::uint64_t index) const noexcept {
        const detail::node<Traits>* n = resolve();
        if (n == nullptr || n->kind != webcpp::document::kind::array) {
            return Handle(nullptr, detail::place::nothing, 0, 0, {});
        }
        if (const detail::node<Traits>* e = tree_ptr()->element(n->id, index)) {
            return Handle(doc_, detail::place::node, e->id, 0, {});
        }
        return Handle(doc_, detail::place::element, n->id, index, {});
    }

    /** Whether the values of two handles are equal. */
    static bool equals(const basic_const_value& a, const basic_const_value& b) noexcept {
        const detail::node<Traits>* x = a.resolve();
        const detail::node<Traits>* y = b.resolve();
        if (x == nullptr || y == nullptr) {
            return x == y;
        }
        return detail::equal(*a.tree_ptr(), x, *b.tree_ptr(), y);
    }

    /** The document, or null for a handle that names nothing. */
    basic_document<Traits, Storage>* doc_ = nullptr;

    /** The node, or the container of a missing member or element. */
    std::uint64_t id_ = 0;

    /** A missing element's index. */
    std::uint64_t index_ = 0;

    /** A missing member's key, in UTF-8. */
    std::string key_;

    /** What the handle names. */
    detail::place place_ = detail::place::nothing;

    template <class, class>
    friend class basic_document;
    template <class, class>
    friend class basic_const_value;
    template <class, class>
    friend class basic_value;
    template <class, class>
    friend class basic_parser;
    template <class, class, detail::children_of>
    friend class detail::children_iterator;
    friend struct detail::access;
};

/**
 A handle that reads and writes: one place in one document, as @ref basic_const_value, with
 JavaScript's assignment, `delete`, `Object.assign` and the array methods.

 Every writing function returns `result` and either completes or changes nothing, a full heap
 included. Assignment to a handle writes to its place, with exceptions only: it never rebinds the
 handle.

 So a handle is not a regular value, and must not be swapped or sorted: `std::swap(x, y)` assigns
 through both, leaving both places holding `y`'s value, and `std::sort` over handles writes the
 document's values the same way.

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
 @see "Handles", in the guide.
 @see "Arrays", in the guide.
*/
template <class Traits, class Storage>
class WEBCPP_DOCUMENT_NO_DANGLING basic_value : public basic_const_value<Traits, Storage> {
public:
    /** A handle that names nothing. */
    basic_value() noexcept = default;

    /**
     A handle of the same place.

     @param other The handle.
    */
    basic_value(const basic_value& other) = default;

    /**
     A handle of the same place.

     @param other The handle.
    */
    basic_value(basic_value&& other) noexcept = default;

    /** Nothing to free: the document owns the value. */
    ~basic_value() = default;

    /** The read-only navigation of a read-only handle, for a `const` one. */
    using basic_const_value<Traits, Storage>::operator[];

    /** The read-only `at` of a read-only handle, for a `const` one. */
    using basic_const_value<Traits, Storage>::at;

    /** The read-only view of an object, for a `const` handle. */
    using basic_const_value<Traits, Storage>::as_object;

    /** The read-only view of an array, for a `const` handle. */
    using basic_const_value<Traits, Storage>::as_array;

    /**
     The object as a sorted-map-like container that reads and writes: `find`, `insert`, `erase`,
     bounds by key, and iterators that `<algorithm>` and `<ranges>` take.

     @return The view, defined in `containers.hpp`; @ref errc::type for any other kind.
     @see "Key order", in the guide.
    */
    [[nodiscard]] result<basic_object_ref<basic_value>> as_object() noexcept;

    /**
     The array as a vector-like container that reads and writes: `push_back`, `insert`, `erase`,
     `resize`, and iterators that `<algorithm>` and `<ranges>` take.

     @return The view, defined in `containers.hpp`; @ref errc::type for any other kind.
     @see "Arrays", in the guide.
    */
    [[nodiscard]] result<basic_array_ref<basic_value>> as_array() noexcept;

    /**
     The member `key`, `v.key`, which never fails.

     @param key The key, in UTF-8.
     @return Its handle: the member, a missing member of an object, or nothing.
    */
    basic_value operator[](std::string_view key) noexcept {
        return this->template member_handle<basic_value>(key);
    }

    /**
     The element `index`, `v[index]`, which never fails.

     @param index The index.
     @return Its handle: the element, a missing element of an array, or nothing.
    */
    basic_value operator[](std::uint64_t index) noexcept {
        return this->template element_handle<basic_value>(index);
    }

    /**
     `Array.prototype.at`: the element `index`, a negative index counting from the end.

     @param index The index.
     @return Its handle; nothing out of range or for any other kind.
    */
    basic_value at(std::int64_t index) noexcept;

    /**
     Stores a value at this place, `v = x`: a missing member is created, an element past the end
     fills the elements between with `null`, and an existing value is replaced.

     Settable: `bool`; any integer type; the decimal types (finite, else @ref errc::range); the
     traits' `number`; `std::string_view`, `const char*`, `std::string` (valid UTF-8, else
     @ref errc::type), `std::u16string_view`; `std::span<const std::byte>` (bytes); a
     @ref date_ms (within JavaScript's range, else @ref errc::range); `nullptr` or @ref null; the
     literals @ref object and @ref array; a handle or a document of the same traits, copied deeply;
     and @ref undefined, which erases a member, stores `null` in an element and makes a root hold
     `undefined`. Numbers are stored by value.

     @tparam T The value's type.
     @param v The value.
     @return Success; @ref errc::type for a place that is nothing, @ref errc::capacity when there is
     no room, and the errors above.
    */
    template <class T>
    [[nodiscard]] result<void> set(const T& v) noexcept
        requires detail::settable<Traits, T> || std::same_as<T, undefined_t>
    {
        if constexpr (std::same_as<T, undefined_t>) {
            return store_undefined();
        } else if constexpr (detail::copy_source<Traits, T>) {
            const auto source = copy_from(v);
            if (!source.has_value()) {
                return source.error();
            }
            const auto [from, n] = source.unsafe_value();
            if (n == nullptr) {
                // The source reads undefined, which stores as undefined does.
                return store_undefined();
            }
            // The tree reads the source before it changes anything, so it may be this tree.
            detail::prepared<Traits> p;
            p.copy_of(*from, n->id);
            return store(p.value());
        } else {
            detail::prepared<Traits> p;
            if (const auto made = detail::prepare(v, p); !made.has_value()) {
                return made;
            }
            return store(p.value());
        }
    }

    /**
     `delete v[key]`: erases an object's member and its subtree.

     @param key The key, in UTF-8.
     @return Whether it was there; @ref errc::type for any other kind than an object.
    */
    [[nodiscard]] result<bool> erase(std::string_view key) noexcept {
        const detail::node<Traits>* n = this->resolve();
        if (n == nullptr || n->kind != webcpp::document::kind::object) {
            return errc::type;
        }
        return writable_tree()->erase_member(n->id, key);
    }

    // The source's keys and ids are staged on the process heap, whose exhaustion ends the program,
    // as the library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     `Object.assign(v, source)`: sets each member of `source` here, in its order, as one operation.

     An array's elements are its members `"0"` to `"n-1"`, and `null` or `undefined` adds nothing.

     @tparam S The source's storage.
     @param source The source.
     @return Success; @ref errc::type when this is not an object or `source` is a scalar,
     @ref errc::capacity when there is no room.
    */
    template <class S>
    [[nodiscard]] result<void> assign_from(const basic_const_value<Traits, S>& source) noexcept {
        const detail::node<Traits>* target = this->resolve();
        if (target == nullptr || target->kind != webcpp::document::kind::object) {
            return errc::type;
        }
        const detail::node<Traits>* s = source.resolve();
        if (s == nullptr || s->kind == webcpp::document::kind::null) {
            return {};
        }
        const bool elements = s->kind == webcpp::document::kind::array;
        if (!elements && s->kind != webcpp::document::kind::object) {
            return errc::type;
        }
        const detail::tree<Traits>& from = *source.tree_ptr();
        // The keys are owned here, in UTF-8, before an entry points at one: the tree may move the
        // source's image while it finds its room, when the source is this document.
        std::vector<std::string> keys;
        std::vector<std::uint64_t> ids;
        for (const detail::node<Traits>* c = from.first_child(s->id); c != nullptr;
             c = from.next_sibling(*c)) {
            keys.push_back(elements ? std::to_string(c->ordinal) : utf8_key(*c));
            ids.push_back(c->id);
        }
        std::vector<detail::literal_entry<Traits>> entries(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            entries[i].key = keys[i];
            entries[i].value.from = &from;
            entries[i].value.from_id = ids[i];
        }
        return writable_tree()->set_members(target->id, entries);
    }

    // NOLINTEND(bugprone-exception-escape)

    /**
     `Array.prototype.push`: appends each value.

     @tparam T The values' types, each settable.
     @param v The values.
     @return The new length; @ref errc::type for any other kind than an array, @ref errc::capacity
     when there is no room, and the errors of @ref set for a value.
    */
    template <class... T>
    [[nodiscard]] result<std::uint64_t> push(const T&... v) noexcept
        requires(detail::settable<Traits, T> && ...);

    /**
     `Array.prototype.pop`: removes the last element.

     @return A new owned document holding it, or holding `undefined` when the array is empty;
     @ref errc::type for any other kind.
    */
    [[nodiscard]] result<basic_document<Traits, owned>> pop() noexcept;

    /**
     `Array.prototype.shift`: removes the first element.

     @return A new owned document holding it, or holding `undefined` when the array is empty;
     @ref errc::type for any other kind.
    */
    [[nodiscard]] result<basic_document<Traits, owned>> shift() noexcept;

    /**
     `Array.prototype.unshift`: inserts each value at the front, in order.

     @tparam T The values' types, each settable.
     @param v The values.
     @return The new length; @ref errc::type for any other kind than an array, @ref errc::capacity
     when there is no room, and the errors of @ref set for a value.
    */
    template <class... T>
    [[nodiscard]] result<std::uint64_t> unshift(const T&... v) noexcept
        requires(detail::settable<Traits, T> && ...);

    /**
     `Array.prototype.splice`: removes `count` elements from `start` and inserts each value there.

     A negative `start` counts from the end, clamped at 0, and one past the length is the length;
     a negative `count` is 0, and one past the end removes to the end.

     @tparam T The values' types, each settable.
     @param start The first index.
     @param count How many to remove.
     @param v The values to insert.
     @return A new owned document holding the array of the removed elements; @ref errc::type for
     any other kind.
     @see "Arrays", in the guide.
    */
    template <class... T>
    [[nodiscard]] result<basic_document<Traits, owned>> splice(std::int64_t start,
                                                               std::int64_t count,
                                                               const T&... v) noexcept
        requires(detail::settable<Traits, T> && ...);

    /**
     `Array.prototype.splice(start)`: removes every element from `start`.

     @param start The first index, a negative one counting from the end.
     @return A new owned document holding the array of the removed elements; @ref errc::type for
     any other kind.
    */
    [[nodiscard]] result<basic_document<Traits, owned>> splice(std::int64_t start) noexcept;

    /**
     `Array.prototype.reverse`: reverses the elements in place.

     @return Success; @ref errc::type for any other kind.
    */
    [[nodiscard]] result<void> reverse() noexcept;

    /**
     `a.length = n`: truncates the array, or appends `null` up to `n`.

     @param n The new length.
     @return Success; @ref errc::type for any other kind.
    */
    [[nodiscard]] result<void> set_length(std::uint64_t n) noexcept;

    /**
     Stores a value at this place, `v = x`, as @ref set does, throwing on failure; only with
     exceptions.

     @tparam T The value's type.
     @param v The value.
     @return This handle.
     @throws type_error For a place that is nothing.
     @throws capacity_error When there is no room.
     @see "The sugar that throws", in the guide.
    */
    template <class T>
    basic_value& operator=(const T& v)
        requires detail::exceptions_enabled<T> &&
                 (detail::settable<Traits, T> || std::same_as<T, undefined_t>);

    /**
     Stores a deep copy of another handle's value at this place, as @ref set does, throwing on
     failure; only with exceptions.

     It never rebinds the handle: assignment to a handle writes to its place.

     @param v The handle.
     @return This handle.
     @throws type_error For a place that is nothing.
     @throws capacity_error When there is no room.
    */
    basic_value& operator=(const basic_value& v)
        requires detail::exceptions_enabled<Traits>;

private:
    template <class, class>
    friend class basic_document;
    template <class, class>
    friend class basic_const_value;
    friend struct detail::access;

    /** A handle of `doc` at the place `where`, as the read-only handle's. */
    basic_value(basic_document<Traits, Storage>* doc, detail::place where, std::uint64_t id,
                std::uint64_t index, std::string_view key) noexcept
        : basic_const_value<Traits, Storage>(doc, where, id, index, key) {}

    /** The tree of the handle's document, to write; null when it names nothing. */
    [[nodiscard]] detail::tree<Traits>* writable_tree() const noexcept {
        return this->doc_ == nullptr ? nullptr : detail::access::tree_of(*this->doc_);
    }

    /** Where a deep copy of a document reads: its tree and its root; none when moved from. */
    template <class S>
    static result<std::pair<const detail::tree<Traits>*, const detail::node<Traits>*>> copy_from(
        const basic_document<Traits, S>& d) noexcept {
        const detail::tree<Traits>* t = detail::access::tree_of(d);
        if (t == nullptr) {
            return errc::type;
        }
        return std::pair(t, detail::access::node_of(d.root()));
    }

    /** Where a deep copy of a handle's value reads: its tree and its node, null for undefined. */
    template <class S>
    static result<std::pair<const detail::tree<Traits>*, const detail::node<Traits>*>> copy_from(
        const basic_const_value<Traits, S>& v) noexcept {
        return std::pair(detail::access::tree_of(v), detail::access::node_of(v));
    }

    /** A key of `n`, in UTF-8: what the tree's operations take. */
    static std::string utf8_key(const detail::node<Traits>& n) {
        if constexpr (std::same_as<typename Traits::string, utf16>) {
            const std::u16string_view units = n.key.units();
            std::string out(static_cast<std::size_t>(detail::utf8_size(units)), '\0');
            detail::to_utf8(units, out.data());
            return out;
        } else {
            return std::string(n.key.chars());
        }
    }

    /** Stores an item at this place: the node replaced, or the member or element created. */
    result<void> store(const detail::item<Traits>& v) noexcept {
        detail::tree<Traits>* t = writable_tree();
        if (t == nullptr) {
            return errc::type;
        }
        switch (this->place_) {
            case detail::place::node: return t->assign(this->id_, v);
            case detail::place::member: {
                const auto set = t->set_member(this->id_, this->key_, v);
                if (!set.has_value()) {
                    return set.error();
                }
                return {};
            }
            case detail::place::element: return t->set_element(this->id_, this->index_, v);
            case detail::place::nothing: break;
        }
        return errc::type;
    }

    // A member's key is turned into UTF-8 on the process heap, whose exhaustion ends the program,
    // as the library tolerates.
    // NOLINTBEGIN(bugprone-exception-escape)
    /**
     Stores `undefined` at this place: a member erased, an element `null`, a root holding
     `undefined`.
    */
    result<void> store_undefined() noexcept {
        detail::tree<Traits>* t = writable_tree();
        if (t == nullptr) {
            return errc::type;
        }
        switch (this->place_) {
            case detail::place::node: break;
            case detail::place::member: {
                const auto erased = t->erase_member(this->id_, this->key_);
                if (!erased.has_value()) {
                    return erased.error();
                }
                return {};
            }
            case detail::place::element:
                return t->set_element(this->id_, this->index_,
                                      detail::empty_item<Traits>(webcpp::document::kind::null));
            case detail::place::nothing: return errc::type;
        }
        const detail::node<Traits>* n = t->find(this->id_);
        if (n == nullptr) {
            return errc::type;
        }
        if (n->parent == 0) {
            return t->assign(this->id_,
                             detail::empty_item<Traits>(webcpp::document::kind::undefined));
        }
        if (const detail::node<Traits>* parent = t->find(n->parent);
            parent->kind == webcpp::document::kind::array) {
            return t->assign(this->id_, detail::empty_item<Traits>(webcpp::document::kind::null));
        }
        const auto erased = t->erase_member(n->parent, utf8_key(*n));
        if (!erased.has_value()) {
            return erased.error();
        }
        return {};
    }

    // NOLINTEND(bugprone-exception-escape)
};

/** A handle of a @ref document, reading and writing. */
using value = basic_value<>;

/** A handle of a @ref document, read only. */
using const_value = basic_const_value<>;

// The members below make handles, which are complete only here.

template <class Traits, class Storage>
basic_value<Traits, Storage> basic_document<Traits, Storage>::root() noexcept {
    return basic_value<Traits, Storage>(this, detail::place::node, detail::root_id, 0, {});
}

template <class Traits, class Storage>
basic_const_value<Traits, Storage> basic_document<Traits, Storage>::root() const noexcept {
    // A read-only handle never writes through the document it names.
    return basic_const_value<Traits, Storage>(const_cast<basic_document*>(this),
                                              detail::place::node, detail::root_id, 0, {});
}

template <class Traits, class Storage>
basic_value<Traits, Storage> basic_document<Traits, Storage>::operator[](
    std::string_view key) noexcept {
    return root()[key];
}

template <class Traits, class Storage>
basic_value<Traits, Storage> basic_document<Traits, Storage>::operator[](
    std::uint64_t index) noexcept {
    return root()[index];
}

template <class Traits, class Storage>
basic_const_value<Traits, Storage> basic_document<Traits, Storage>::operator[](
    std::string_view key) const noexcept {
    return root()[key];
}

template <class Traits, class Storage>
basic_const_value<Traits, Storage> basic_document<Traits, Storage>::operator[](
    std::uint64_t index) const noexcept {
    return root()[index];
}

namespace detail {

// A handle of a node holds no key, so copying one allocates nothing; were it to, the process heap's
// exhaustion would end the program, as the library tolerates.
// NOLINTBEGIN(bugprone-exception-escape)
template <class Traits, class Storage, children_of Kind>
auto children_iterator<Traits, Storage, Kind>::operator*() const noexcept -> value_type {
    if constexpr (Kind == children_of::values) {
        return basic_const_value<Traits, Storage>(doc_, place::node, child_, 0, {});
    } else {
        // A key is a view of the image, valid until the document's next change.
        const tree<Traits>* t = doc_ == nullptr ? nullptr : access::tree_of(std::as_const(*doc_));
        const node<Traits>* n = t == nullptr ? nullptr : t->find(child_);
        text_view_t<Traits> key;
        if (n != nullptr) {
            if constexpr (std::same_as<typename Traits::string, utf16>) {
                key = n->key.units();
            } else {
                key = n->key.chars();
            }
        }
        if constexpr (Kind == children_of::keys) {
            return key;
        } else {
            return value_type(key,
                              basic_const_value<Traits, Storage>(doc_, place::node, child_, 0, {}));
        }
    }
}

// NOLINTEND(bugprone-exception-escape)

template <class Traits, class Storage, children_of Kind>
children_iterator<Traits, Storage, Kind>&
children_iterator<Traits, Storage, Kind>::operator++() noexcept {
    const tree<Traits>* t = doc_ == nullptr ? nullptr : access::tree_of(std::as_const(*doc_));
    const node<Traits>* n = t == nullptr ? nullptr : t->find(child_);
    const node<Traits>* next = n == nullptr ? nullptr : t->next_sibling(*n);
    child_ = next == nullptr ? 0 : next->id;
    return *this;
}

template <class Traits, class Storage, children_of Kind>
children_iterator<Traits, Storage, Kind> children<Traits, Storage, Kind>::begin() const noexcept {
    // The root's parent is 0: the children of 0 are those of nothing, not the root.
    const tree<Traits>* t =
        doc_ == nullptr || parent_ == 0 ? nullptr : access::tree_of(std::as_const(*doc_));
    const node<Traits>* first = t == nullptr ? nullptr : t->first_child(parent_);
    return {doc_, first == nullptr ? 0 : first->id};
}

}  // namespace detail

}  // namespace webcpp::document

#include <webcpp/document/containers.hpp>
#include <webcpp/document/detail/value_array.hpp>
#include <webcpp/document/detail/value_sugar.hpp>
#include <webcpp/document/stringify.hpp>

#endif  // WEBCPP_DOCUMENT_VALUE_HPP
