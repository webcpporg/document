// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The definitions of the handles' array methods, which `value.hpp` declares and includes this header
 to define: `at`, `slice`, `concat`, `push`, `pop`, `shift`, `unshift`, `splice`, `reverse` and
 `set_length`, with JavaScript's index rules, written once in @ref relative_index.
*/

#ifndef WEBCPP_DOCUMENT_DETAIL_VALUE_ARRAY_HPP
#define WEBCPP_DOCUMENT_DETAIL_VALUE_ARRAY_HPP

#include <webcpp/document/config.hpp>

#include <webcpp/document/value.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::document::detail {

/**
 ECMA-262's relative index, after ToIntegerOrInfinity: a negative index counts from the end, and
 the result is clamped to `[0, length]`, with no overflow at `INT64_MIN`.
*/
constexpr std::uint64_t relative_index(std::int64_t i, std::uint64_t length) noexcept {
    if (i < 0) {
        // -(i + 1) + 1 is -i, written so that INT64_MIN does not overflow.
        const std::uint64_t back = static_cast<std::uint64_t>(-(i + 1)) + 1;
        return back >= length ? 0 : length - back;
    }
    return std::cmp_greater(i, length) ? length : static_cast<std::uint64_t>(i);
}

/**
 The element `Array.prototype.at` reads: a negative index counts from the end; `length` when the
 index is out of range, where JavaScript reads `undefined`.
*/
constexpr std::uint64_t at_index(std::int64_t i, std::uint64_t length) noexcept {
    // Past the front, the relative index would clamp to 0, where `at` reads nothing.
    if (i < 0 && std::cmp_greater_equal(-(i + 1), length)) {
        return length;
    }
    return relative_index(i, length);
}

/** `splice`'s count: a negative one is 0, and one past the end removes to the end. */
constexpr std::uint64_t splice_count(std::int64_t count, std::uint64_t start,
                                     std::uint64_t length) noexcept {
    if (count < 0) {
        return 0;
    }
    const std::uint64_t left = length - start;
    return std::cmp_greater(count, left) ? left : static_cast<std::uint64_t>(count);
}

/** The tree and node a copy source reads. */
template <class Traits>
struct source_read {
    /** The tree. */
    const tree<Traits>* from = nullptr;

    /** The node, or null when the source reads `undefined`. */
    const node<Traits>* n = nullptr;
};

/** Where a document's value is read: its tree and its root; @ref errc::type when moved from. */
template <class Traits, class Storage>
result<source_read<Traits>> read_source(const basic_document<Traits, Storage>& d) noexcept {
    const tree<Traits>* t = access::tree_of(d);
    if (t == nullptr) {
        return errc::type;
    }
    return source_read<Traits>{t, access::node_of(d.root())};
}

/** Where a handle's value is read: its document's tree and its node, null for `undefined`. */
template <class Traits, class Storage>
result<source_read<Traits>> read_source(const basic_const_value<Traits, Storage>& v) noexcept {
    return source_read<Traits>{access::tree_of(v), access::node_of(v)};
}

/**
 Makes `out` the item of a value an array method stores: a C++ value prepared, or a deep copy of a
 source's value, `null` for one that reads `undefined`, as `[undefined]` is written.
*/
template <class Traits, class T>
    requires settable<Traits, T>
result<void> prepare_element(const T& v, prepared<Traits>& out) noexcept {
    if constexpr (copy_source<Traits, T>) {
        const auto s = read_source(v);
        if (!s.has_value()) {
            return s.error();
        }
        // A prepared value holds null until it is made a copy.
        if (const node<Traits>* n = s.unsafe_value().n; n != nullptr) {
            out.copy_of(*s.unsafe_value().from, n->id);
        }
        return {};
    } else {
        return prepare(v, out);
    }
}

/**
 Prepares each value into `held` and its item into `items`, in order: the first failure stops it.
 The items point into `held`, which must outlive them.
*/
template <class Traits, class... T, std::size_t... I>
result<void> prepare_elements(std::array<prepared<Traits>, sizeof...(T)>& held,
                              std::array<item<Traits>, sizeof...(T)>& items,
                              std::index_sequence<I...> /*indices*/, const T&... v) noexcept {
    if constexpr (sizeof...(T) != 0) {
        result<void> made;
        // The fold stops at the first value that fails.
        static_cast<void>(((made = prepare_element(v, held[I]), made.has_value()) && ...));
        if (!made.has_value()) {
            return made;
        }
        ((items[I] = held[I].value()), ...);
    }
    return {};
}

/** A new owned document whose value is `v`, read before the call returns. */
template <class Traits>
result<basic_document<Traits, owned>> owned_holding(const item<Traits>& v) noexcept {
    auto made = basic_document<Traits, owned>::create();
    if (!made.has_value()) {
        return made.error();
    }
    if (const auto stored = access::tree_of(made.unsafe_value())->assign(root_id, v);
        !stored.has_value()) {
        return stored.error();
    }
    return made;
}

/** The item of an array literal of `entries`, which must outlive it. */
template <class Traits>
item<Traits> array_of(std::span<const literal_entry<Traits>> entries) noexcept {
    item<Traits> a = empty_item<Traits>(webcpp::document::kind::array);
    a.entries = entries.empty() ? nullptr : entries.data();
    a.entry_count = entries.size();
    return a;
}

// The entries are staged on the process heap, whose exhaustion ends the program, as the library
// tolerates.
// NOLINTBEGIN(bugprone-exception-escape)

/** Appends to `out` an entry copying each of the `n` elements of `array` in `from`, from `at`. */
template <class Traits>
void copy_elements(const tree<Traits>& from, std::uint64_t array, std::uint64_t at, std::uint64_t n,
                   std::vector<literal_entry<Traits>>& out) {
    const node<Traits>* e = n == 0 ? nullptr : from.element(array, at);
    for (std::uint64_t i = 0; i < n && e != nullptr; ++i, e = from.next_sibling(*e)) {
        literal_entry<Traits>& entry = out.emplace_back();
        entry.value.from = &from;
        entry.value.from_id = e->id;
    }
}

/**
 Copies the `n` elements of `array` from `at` into a new owned document, then erases them and
 inserts `items` there in one operation of the tree, so that a failure leaves both unchanged.

 The document holds the array of the copies, or, when `single`, the one element, or `undefined`
 when there is none: `pop` and `shift`.
*/
template <class Traits>
result<basic_document<Traits, owned>> splice_elements(tree<Traits>& t, std::uint64_t array,
                                                      std::uint64_t at, std::uint64_t n,
                                                      std::span<const item<Traits>> items,
                                                      bool single) noexcept {
    result<basic_document<Traits, owned>> removed = errc::type;
    if (single) {
        // With nothing to remove, it holds undefined, which JavaScript's pop of [] returns.
        item<Traits> one = empty_item<Traits>(webcpp::document::kind::undefined);
        if (n != 0) {
            one.from = &t;
            one.from_id = t.element(array, at)->id;
        }
        removed = owned_holding(one);
    } else {
        std::vector<literal_entry<Traits>> entries;
        entries.reserve(static_cast<std::size_t>(n));
        copy_elements(t, array, at, n, entries);
        removed = owned_holding(array_of<Traits>(entries));
    }
    if (!removed.has_value()) {
        return removed;
    }
    if (const auto done = t.splice(array, at, n, items); !done.has_value()) {
        return done.error();
    }
    return removed;
}

/**
 The entries of `concat`'s array, and the prepared values they point into: this array's elements,
 then each argument, an array's elements or the value.
*/
template <class Traits>
class concatenation {
public:
    /** Appends a copy of each element of `array` in `from`. */
    void add_elements(const tree<Traits>& from, std::uint64_t array) {
        copy_elements(from, array, 0, from.count(array), entries_);
    }

    /** Appends an argument: an array's elements, spread, or the value. */
    template <class T>
        requires settable<Traits, T>
    result<void> add(const T& v) {
        if constexpr (std::same_as<T, webcpp::document::array>) {
            return add_literals(v);
        } else if constexpr (std::same_as<T, literal>) {
            // A literal written as `[...]` is an array, which JavaScript's concat spreads.
            const webcpp::document::array* inner = nullptr;
            v.visit([&inner](const auto& x) {
                if constexpr (std::same_as<std::remove_cvref_t<decltype(x)>,
                                           webcpp::document::array>) {
                    inner = &x;
                }
            });
            if (inner != nullptr) {
                return add_literals(*inner);
            }
            return add_value(v);
        } else if constexpr (copy_source<Traits, T>) {
            const auto s = read_source(v);
            if (!s.has_value()) {
                return s.error();
            }
            const node<Traits>* n = s.unsafe_value().n;
            if (n != nullptr && n->kind == webcpp::document::kind::array) {
                add_elements(*s.unsafe_value().from, n->id);
                return {};
            }
            return add_value(v);
        } else {
            return add_value(v);
        }
    }

    /** The entries, valid while this object lives and is not changed. */
    [[nodiscard]] std::span<const literal_entry<Traits>> entries() const noexcept {
        return entries_;
    }

private:
    /** Appends each element of an array literal. */
    result<void> add_literals(const webcpp::document::array& a) {
        for (const literal& e : a.elements()) {
            if (const auto added = add_value(e); !added.has_value()) {
                return added;
            }
        }
        return {};
    }

    /** Appends one value. */
    template <class T>
    result<void> add_value(const T& v) {
        prepared<Traits>& p = held_.emplace_back();
        if (const auto made = prepare_element(v, p); !made.has_value()) {
            return made;
        }
        entries_.emplace_back().value = p.value();
        return {};
    }

    // A prepared value moved keeps what its item points at, so the vector may grow.
    /** The prepared values the entries point into. */
    std::vector<prepared<Traits>> held_;

    /** The array's entries, in order. */
    std::vector<literal_entry<Traits>> entries_;
};

// NOLINTEND(bugprone-exception-escape)

}  // namespace webcpp::document::detail

namespace webcpp::document {

template <class Traits, class Storage>
basic_const_value<Traits, Storage> basic_const_value<Traits, Storage>::at(
    std::int64_t index) const noexcept {
    const std::uint64_t id = id_of(webcpp::document::kind::array);
    if (id == 0) {
        return {};
    }
    const std::uint64_t length = tree_ptr()->count(id);
    const std::uint64_t k = detail::at_index(index, length);
    if (k == length) {
        return {};
    }
    return element_handle<basic_const_value>(k);
}

template <class Traits, class Storage>
basic_value<Traits, Storage> basic_value<Traits, Storage>::at(std::int64_t index) noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return {};
    }
    const std::uint64_t length = this->tree_ptr()->count(id);
    const std::uint64_t k = detail::at_index(index, length);
    if (k == length) {
        return {};
    }
    return this->template element_handle<basic_value>(k);
}

// The copies' entries are staged on the process heap, whose exhaustion ends the program, as the
// library tolerates.
// NOLINTBEGIN(bugprone-exception-escape)

template <class Traits, class Storage>
result<basic_document<Traits, owned>> basic_const_value<Traits, Storage>::slice(
    std::int64_t start, std::int64_t stop) const noexcept {
    const std::uint64_t id = id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    const detail::tree<Traits>& t = *tree_ptr();
    const std::uint64_t length = t.count(id);
    const std::uint64_t first = detail::relative_index(start, length);
    const std::uint64_t last = detail::relative_index(stop, length);
    std::vector<detail::literal_entry<Traits>> entries;
    if (last > first) {
        entries.reserve(static_cast<std::size_t>(last - first));
        detail::copy_elements(t, id, first, last - first, entries);
    }
    return detail::owned_holding(detail::array_of<Traits>(entries));
}

template <class Traits, class Storage>
template <class... T>
result<basic_document<Traits, owned>> basic_const_value<Traits, Storage>::concat(
    const T&... v) const noexcept
    requires(detail::settable<Traits, T> && ...)
{
    const std::uint64_t id = id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    detail::concatenation<Traits> c;
    c.add_elements(*tree_ptr(), id);
    if constexpr (sizeof...(T) != 0) {
        result<void> added;
        // The fold stops at the first argument that fails.
        static_cast<void>(((added = c.add(v), added.has_value()) && ...));
        if (!added.has_value()) {
            return added.error();
        }
    }
    return detail::owned_holding(detail::array_of<Traits>(c.entries()));
}

// NOLINTEND(bugprone-exception-escape)

template <class Traits, class Storage>
template <class... T>
result<std::uint64_t> basic_value<Traits, Storage>::push(const T&... v) noexcept
    requires(detail::settable<Traits, T> && ...)
{
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    std::array<detail::prepared<Traits>, sizeof...(T)> held{};
    std::array<detail::item<Traits>, sizeof...(T)> items{};
    if (const auto made =
            detail::prepare_elements(held, items, std::index_sequence_for<T...>(), v...);
        !made.has_value()) {
        return made.error();
    }
    detail::tree<Traits>* t = writable_tree();
    if (const auto done = t->insert_elements(id, t->count(id), items); !done.has_value()) {
        return done.error();
    }
    return t->count(id);
}

template <class Traits, class Storage>
result<basic_document<Traits, owned>> basic_value<Traits, Storage>::pop() noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    detail::tree<Traits>* t = writable_tree();
    const std::uint64_t length = t->count(id);
    return detail::splice_elements<Traits>(*t, id, length == 0 ? 0 : length - 1,
                                           length == 0 ? 0 : 1, {}, true);
}

template <class Traits, class Storage>
result<basic_document<Traits, owned>> basic_value<Traits, Storage>::shift() noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    detail::tree<Traits>* t = writable_tree();
    return detail::splice_elements<Traits>(*t, id, 0, t->count(id) == 0 ? 0 : 1, {}, true);
}

template <class Traits, class Storage>
template <class... T>
result<std::uint64_t> basic_value<Traits, Storage>::unshift(const T&... v) noexcept
    requires(detail::settable<Traits, T> && ...)
{
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    std::array<detail::prepared<Traits>, sizeof...(T)> held{};
    std::array<detail::item<Traits>, sizeof...(T)> items{};
    if (const auto made =
            detail::prepare_elements(held, items, std::index_sequence_for<T...>(), v...);
        !made.has_value()) {
        return made.error();
    }
    detail::tree<Traits>* t = writable_tree();
    if (const auto done = t->insert_elements(id, 0, items); !done.has_value()) {
        return done.error();
    }
    return t->count(id);
}

template <class Traits, class Storage>
template <class... T>
result<basic_document<Traits, owned>> basic_value<Traits, Storage>::splice(std::int64_t start,
                                                                           std::int64_t count,
                                                                           const T&... v) noexcept
    requires(detail::settable<Traits, T> && ...)
{
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    std::array<detail::prepared<Traits>, sizeof...(T)> held{};
    std::array<detail::item<Traits>, sizeof...(T)> items{};
    if (const auto made =
            detail::prepare_elements(held, items, std::index_sequence_for<T...>(), v...);
        !made.has_value()) {
        return made.error();
    }
    detail::tree<Traits>* t = writable_tree();
    const std::uint64_t length = t->count(id);
    const std::uint64_t at = detail::relative_index(start, length);
    return detail::splice_elements<Traits>(*t, id, at, detail::splice_count(count, at, length),
                                           items, false);
}

template <class Traits, class Storage>
result<basic_document<Traits, owned>> basic_value<Traits, Storage>::splice(
    std::int64_t start) noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    detail::tree<Traits>* t = writable_tree();
    const std::uint64_t length = t->count(id);
    const std::uint64_t at = detail::relative_index(start, length);
    return detail::splice_elements<Traits>(*t, id, at, length - at, {}, false);
}

template <class Traits, class Storage>
result<void> basic_value<Traits, Storage>::reverse() noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    return writable_tree()->reverse(id);
}

template <class Traits, class Storage>
result<void> basic_value<Traits, Storage>::set_length(std::uint64_t n) noexcept {
    const std::uint64_t id = this->id_of(webcpp::document::kind::array);
    if (id == 0) {
        return errc::type;
    }
    return writable_tree()->set_length(id, n);
}

}  // namespace webcpp::document

#endif  // WEBCPP_DOCUMENT_DETAIL_VALUE_ARRAY_HPP
