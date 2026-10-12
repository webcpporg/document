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
 methods are defined in `detail/value_array.hpp`, the sugar that throws in `detail/value_sugar.hpp`
 and `stringify` in `stringify.hpp`, each included at its end.
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
    children_iterator(basic_document<Traits, Storage>* doc, std::uint64_t child) noexcept;

    /** The current child. */
    value_type operator*() const noexcept;

    /** Moves to the next child. */
    children_iterator& operator++() noexcept;

    /** Moves to the next child, and returns where it was. */
    children_iterator operator++(int) noexcept;

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
    /** The children of the node `parent` of `doc`; none when `doc` is null. */
    children(basic_document<Traits, Storage>* doc, std::uint64_t parent) noexcept;

    /** The first child. */
    [[nodiscard]] children_iterator<Traits, Storage, Kind> begin() const noexcept;

    /** The end. */
    [[nodiscard]] children_iterator<Traits, Storage, Kind> end() const noexcept;

private:
    basic_document<Traits, Storage>* doc_ = nullptr;
    std::uint64_t parent_ = 0;
};

/** What the library's other parts reach inside documents and handles: their tree and their node. */
struct access {
    /** The tree of a document, or null when it was moved from. */
    template <class Traits, class Storage>
    static tree<Traits>* tree_of(basic_document<Traits, Storage>& d) noexcept;

    /** The tree of a document, or null when it was moved from. */
    template <class Traits, class Storage>
    static const tree<Traits>* tree_of(const basic_document<Traits, Storage>& d) noexcept;

    /** The tree of a handle's document, or null when it names nothing. */
    template <class Traits, class Storage>
    static const tree<Traits>* tree_of(const basic_const_value<Traits, Storage>& v) noexcept;

    /** The node a handle names, or null when it reads `undefined`. */
    template <class Traits, class Storage>
    static const node<Traits>* node_of(const basic_const_value<Traits, Storage>& v) noexcept;

    /** The read-only handle of the node `id` of a document. */
    template <class Traits, class Storage>
    static basic_const_value<Traits, Storage> handle_of(basic_document<Traits, Storage>& d,
                                                        std::uint64_t id) noexcept;

    /** The document of a region whose tree was formatted or attached, the tree rebound to it. */
    template <class Traits, class Storage>
    static basic_document<Traits, Storage> make_document(region r) noexcept;
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
    [[nodiscard]] webcpp::document::kind kind() const noexcept;

    /**
     Whether nothing is there, `v === undefined`.

     @return Whether the kind is @ref kind::undefined.
    */
    [[nodiscard]] bool is_undefined() const noexcept;

    /**
     Whether the value is `null`.

     @return Whether the kind is @ref kind::null.
    */
    [[nodiscard]] bool is_null() const noexcept;

    /**
     Whether the value is `true` or `false`.

     @return Whether the kind is @ref kind::boolean.
    */
    [[nodiscard]] bool is_boolean() const noexcept;

    /**
     Whether the value is a number.

     @return Whether the kind is @ref kind::number.
    */
    [[nodiscard]] bool is_number() const noexcept;

    /**
     Whether the value is a number that is an integer, JavaScript's `Number.isInteger`.

     @return Whether it is a number with no fractional part.
    */
    [[nodiscard]] bool is_integer() const noexcept;

    /**
     Whether the value is a string.

     @return Whether the kind is @ref kind::string.
    */
    [[nodiscard]] bool is_string() const noexcept;

    /**
     Whether the value is a date.

     @return Whether the kind is @ref kind::date.
    */
    [[nodiscard]] bool is_date() const noexcept;

    /**
     Whether the value is bytes.

     @return Whether the kind is @ref kind::bytes.
    */
    [[nodiscard]] bool is_bytes() const noexcept;

    /**
     Whether the value is an array, `Array.isArray`.

     @return Whether the kind is @ref kind::array.
    */
    [[nodiscard]] bool is_array() const noexcept;

    /**
     Whether the value is a plain object.

     @return Whether the kind is @ref kind::object.
    */
    [[nodiscard]] bool is_object() const noexcept;

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
        requires detail::readable<Traits, T>;

    /**
     The value as `T`, or `fallback`: JavaScript's `v ?? fallback`, and the fallback too when the
     value cannot be read as `T`.

     @tparam T The type, as @ref get takes it.
     @param fallback The value when the value is `undefined` or `null`, or not readable as `T`.
     @return The value or the fallback.
    */
    template <class T>
    [[nodiscard]] T value_or(T fallback) const noexcept
        requires detail::readable<Traits, T>;

    /**
     Whether an object has the member `key`, `key in v`.

     @param key The key, in UTF-8.
     @return Whether it is there; false for any other kind.
    */
    [[nodiscard]] bool contains(std::string_view key) const noexcept;

    /**
     An object's key count or an array's length.

     @return The count; 0 for any other kind.
    */
    [[nodiscard]] std::uint64_t size() const noexcept;

    /**
     An array's `length`, or a string's in UTF-16 code units, as JavaScript counts it.

     @return The length; @ref errc::type for any other kind.
    */
    [[nodiscard]] result<std::uint64_t> length() const noexcept;

    /**
     The member `key`, `v.key`, which never fails.

     @param key The key, in UTF-8.
     @return Its handle: the member, a missing member of an object, or nothing.
    */
    basic_const_value operator[](std::string_view key) const noexcept;

    /**
     The element `index`, `v[index]`, which never fails.

     @param index The index.
     @return Its handle: the element, a missing element of an array, or nothing.
    */
    basic_const_value operator[](std::uint64_t index) const noexcept;

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
        const noexcept;

    /**
     `Object.keys`: each key, in the policy's order; a `std::string_view` with `utf8`, a
     `std::u16string_view` with `utf16`.

     @return The range; empty for any other kind than an object.
    */
    [[nodiscard]] detail::children<Traits, Storage, detail::children_of::keys> keys()
        const noexcept;

    /**
     `Object.values`: each value, in the policy's order.

     @return The range; empty for any other kind than an object.
    */
    [[nodiscard]] detail::children<Traits, Storage, detail::children_of::values> values()
        const noexcept;

    /**
     The first element of an array, for `for (auto v : arr)`.

     @return The iterator; the end for any other kind.
    */
    [[nodiscard]] detail::children_iterator<Traits, Storage, detail::children_of::values> begin()
        const noexcept;

    /**
     The end of an array's elements.

     @return The iterator.
    */
    [[nodiscard]] detail::children_iterator<Traits, Storage, detail::children_of::values> end()
        const noexcept;

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
    /**
     A handle of `doc` at the place `where`: the node or container `id`, and a missing element's
     `index` or member's `key`.
    */
    basic_const_value(basic_document<Traits, Storage>* doc, detail::place where, std::uint64_t id,
                      std::uint64_t index, std::string_view key) noexcept;

    /** The node the handle names now, or null when it reads `undefined`. */
    [[nodiscard]] const detail::node<Traits>* resolve() const noexcept;

    /** Whether the values of two handles are equal. */
    static bool equals(const basic_const_value& a, const basic_const_value& b) noexcept;

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

 @tparam Traits The document's traits.
 @tparam Storage The document's storage.
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

    /**
     The member `key`, `v.key`, which never fails.

     @param key The key, in UTF-8.
     @return Its handle: the member, a missing member of an object, or nothing.
    */
    basic_value operator[](std::string_view key) noexcept;

    /**
     The element `index`, `v[index]`, which never fails.

     @param index The index.
     @return Its handle: the element, a missing element of an array, or nothing.
    */
    basic_value operator[](std::uint64_t index) noexcept;

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
        requires detail::settable<Traits, T> || std::same_as<T, undefined_t>;

    /**
     `delete v[key]`: erases an object's member and its subtree.

     @param key The key, in UTF-8.
     @return Whether it was there; @ref errc::type for any other kind than an object.
    */
    [[nodiscard]] result<bool> erase(std::string_view key) noexcept;

    /**
     `Object.assign(v, source)`: sets each member of `source` here, in its order, as one operation.

     An array's elements are its members `"0"` to `"n-1"`, and `null` or `undefined` adds nothing.

     @tparam S The source's storage.
     @param source The source.
     @return Success; @ref errc::type when this is not an object or `source` is a scalar,
     @ref errc::capacity when there is no room.
    */
    template <class S>
    [[nodiscard]] result<void> assign_from(basic_const_value<Traits, S> source) noexcept;

    /**
     `Array.prototype.push`: appends each value.

     @tparam T The values' types, each settable.
     @param v The values.
     @return The new length; @ref errc::type for any other kind than an array.
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
     @return The new length; @ref errc::type for any other kind than an array.
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
                std::uint64_t index, std::string_view key) noexcept;
};

/** A handle of a @ref document, reading and writing. */
using value = basic_value<>;

/** A handle of a @ref document, read only. */
using const_value = basic_const_value<>;

}  // namespace webcpp::document

#include <webcpp/document/detail/value_array.hpp>
#include <webcpp/document/detail/value_sugar.hpp>
#include <webcpp/document/stringify.hpp>

#endif  // WEBCPP_DOCUMENT_VALUE_HPP
