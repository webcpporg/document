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
- **The JavaScript API** (`js/`) is built by `js/Jamfile`, `js/shared/Jamfile` and `js/npm/Jamfile`
  into the build directory, and installed into `js/package` from there. `test/js/Jamfile` tests it
  as the own lane `js` (`b2 toolset=emscripten libs/document/test/js//js`), so that the lint
  compiles `js/binding.cpp`, `js/binding.hpp` and `js/shared/binding.cpp` and the CI runs the
  tests. The C++ names are snake_case; the JavaScript names (`NativeDocument`, `getDecimal`) appear
  only in the `EMSCRIPTEN_BINDINGS` registrations, which the wrappers and the tests use.
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
- `asio.hpp` is the one header that includes Boost.Asio, which builds natively only: the
  convenience header leaves it out, `test/Jamfile` compiles it alone natively, and every program
  that includes it is declared `native`.

## The page

- `doc/document.adoc` includes one file per section, in this order: `introduction.adoc`,
  `gains.adoc`, `js-module.adoc`, `javascript.adoc`, `handles.adoc`, `numbers.adoc`,
  `strings.adoc`, `order.adoc`, `arrays.adoc`, `containers.adoc`, `stringify.adoc`,
  `parsing.adoc`, `streams.adoc`, `selectors.adoc`, `traits.adoc`, `images.adoc`, `wire.adoc`,
  `sharing.adoc`, `boost-json.adoc`, `exceptions.adoc`, `errors.adoc`, `testing.adoc`,
  `benchmarks.adoc`, then the reference and the changelog.
  `b2 libs/document/doc` builds it with the strict reference.
- Each `@see "<title>"` of a Doc Comment names a section's title: "Introduction", "What C++
  gives JavaScript", "Using it from JavaScript", "From JavaScript", "Handles", "Numbers", "Strings,
  dates and bytes", "Key order", "Arrays", "Containers", "Writing JSON", "Parsing", "Streams of
  documents", "Selectors", "Traits", "Images", "Images on the wire", "Sealed images", "Sharing",
  "Compared with Boost.JSON", "The sugar that throws", "Errors", "Testing", "Benchmarks".
  Renaming a section renames its `@see` lines in the same commit; doc-check fails otherwise.
- The guide's programs are `example/document_<topic>.cpp`, each with its `.expected`, whose lines
  are what Node prints for the same JavaScript wherever the page does not say otherwise; check a
  new output against Node before committing it. The page shows their regions by tag and their
  outputs by `lines=`: a change to a program's output checks the `lines=` ranges of the sections
  that show it.
- `document_javascript.cpp` shows the sugar and declares `<exception-handling>on`;
  `document_no_exceptions.cpp` declares `<exception-handling>off`. Every other example builds both
  ways.
- `document_image.expected` holds a digest of an image, which is the same on every target: a
  change to the image's layout, or to what the example stores, changes it on every target at once.
- The README copies `example/document_quick_start.cpp[tag=program]` and its output; doc-check
  compares the copy at every doc build.
