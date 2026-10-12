# Working on document

webcpp's rules apply here: read the superproject's [AGENTS.md](../../AGENTS.md)
(<https://github.com/webcpporg/webcpp/blob/main/AGENTS.md>) first. This file holds only what is
specific to document.

## What document is

- **webcpp's own library** (`"port-of": null`): a JSON document whose behaviour is JavaScript's. Node
  is its oracle: `test/oracle` runs the same cases in Node and in C++, and the bytes must agree.
- **Its core never throws.** Every operation that can fail returns `result`; it finds its room in
  the heap before it allocates, so a full heap is an error value. Only the sugar (`v = x`, `as<T>()`,
  `as_string()`, `as_bool()`, `parse_or_throw`) throws, and each carries the constraint
  `detail::exceptions_enabled`, so a build without exceptions refuses a call to it.
- **One image.** A 64-byte header, then Boost.Interprocess's `rbtree_best_fit` heap, used directly;
  every pointer in it an `offset_ptr<..., std::int64_t, std::uint64_t, 8>`.

## Cautions

- Never include `managed_external_buffer.hpp`, `segment_manager.hpp`, `allocators/allocator.hpp` or
  `sync/mutex_family.hpp` from Boost.Interprocess: they throw bare and do not compile without
  exceptions.
- No floating point outside `js_traits` and its codec; no hashed container; nodes are built in place
  (`emplace`), never copied from a temporary; the allocator zeroes every block it hands out and
  frees.
- The values of `errc` and `kind`, the category's identity `0x646f63756d656e74` ("document") and the
  image's header layout never change; a layout change raises the image's format version.
- Boost.MultiIndex erases an element whose `modify` throws: a `modify` only swaps prepared values.

## The page

`doc/document.adoc` and its sections; `b2 libs/document/doc` builds it with the strict reference.
