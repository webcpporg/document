# @webcpp/document

C++ gives JavaScript a JSON document that is exact, ordered and relocatable, and this package
brings those wonders to JavaScript programmers without the risks of C++: no pointers, no
undefined behaviour, no build step. It is [webcpp::document](https://webcpporg.github.io/libs/document/),
compiled to WebAssembly, behind two small classes that keep JavaScript's values and
JavaScript's errors.

- **Exact decimals.** Numbers keep every digit of their text: `0.1234567890123456789012345678901234`
  and `12345678901234567890` come back as they were written, and `getDecimal` hands them to you.
- **Ordered keys.** Objects keep JavaScript's own key order (array indices ascending, then the
  other keys as they were created), and `keysInRange` queries them in key order.
- **Binary images, opened in place.** A document is one block of bytes. `image()` is a view of
  it with no copy, and `Document.open(bytes)` reads such a block back with no parse.
- **Documents transferred to workers.** Send the image to a Worker as a transferable
  `ArrayBuffer`: the worker opens it with no parse and no copy of the values.
- **Streams.** `StreamParser` reads documents from NDJSON, JSON text sequences or concatenated
  JSON as the bytes arrive, from a `ReadableStream` or any async iterable.
- **Memory you can free.** `doc.free()` releases a document at once; one you drop is released
  when the garbage collector finds it.

It runs in Node, in browsers and in Workers, has no dependencies, and ships its own TypeScript
declarations.

## Install

    npm install @webcpp/document

The package is an ES module and needs Node 22 or newer (tested on 22, 24 and 26), or a browser or
bundler that supports top-level `await` (the module loads its WebAssembly before it exports
anything).

## Node

```js
import { Document } from '@webcpp/document'

const doc = Document.parse('{"user":{"name":"Ada"},"price":1.5}')
doc.set(['user', 'langs'], ['C', 'Note G'])
doc.push(['user', 'langs'], 'JavaScript')

doc.get(['user', 'name'])     // 'Ada'
doc.length(['user', 'langs']) // 3
doc.stringify()               // '{"user":{"name":"Ada","langs":["C","Note G","JavaScript"]},"price":1.5}'

// The image is the document's bytes. Copy it to keep it, and open it later with no parse.
const bytes = doc.image().slice()
const again = Document.open(bytes)
again.get(['price'])          // 1.5
doc.free()
again.free()
```

## Browser

Bundlers such as Vite, webpack and esbuild resolve the `.wasm` file because the module finds it
with `new URL('webcpp-document-core.wasm', import.meta.url)`. Without a bundler, serve
`node_modules/@webcpp/document/` and import the wrapper by its path:

```html
<script type="module">
    import { Document } from './node_modules/@webcpp/document/webcpp-document.mjs'

    const doc = Document.parse('{"price":0.1234567890123456789012345678901234}')
    document.body.textContent = doc.getDecimal(['price']) // 0.1234567890123456789012345678901234
</script>
```

Send a document to a Worker without parsing it again:

```js
// page
const bytes = doc.image().slice()
worker.postMessage({ image: bytes.buffer }, [bytes.buffer])

// worker
import { Document } from '@webcpp/document'
self.onmessage = ({ data }) => {
    const doc = Document.open(new Uint8Array(data.image))
    self.postMessage(doc.get(['price']))
}
```

A bundler needs a target that supports top-level `await` (for Vite, `build.target: 'es2022'` or
newer).

## Streams

```js
import { StreamParser } from '@webcpp/document'

const response = await fetch('/events.ndjson')
for await (const doc of StreamParser.from(response.body, { framing: 'lines' })) {
    console.log(doc.get(['id']))
    doc.free()
}
```

## Exact numbers, and their caveat

`get` returns the `Number` that JavaScript would, so a number with more digits than a double keeps
is rounded there. `stringify` writes the digits the document holds instead, where
`JSON.stringify(JSON.parse(text))` rounds them:

```js
const doc = Document.parse('{"n":12345678901234567890,"big":[1e400]}')
doc.getDecimal(['n'])                 // '12345678901234567890'
doc.get(['n'])                        // 12345678901234567000
doc.stringify()                       // '{"n":12345678901234567890,"big":[1e+400]}'
JSON.stringify(JSON.parse('[1e400]')) // '[null]', since the Number is Infinity
```

So the text a document writes is exact, and `JSON.parse` of it is only as exact as a `Number`:
read it back with this package, or with a decimal-aware parser, when the digits matter.

## More

The full guide, with the C++ library behind this package, its numbers, key order, images,
streams and the benchmarks, is at <https://webcpporg.github.io/libs/document/>; the part
about JavaScript is <https://webcpporg.github.io/libs/document/#document-js>.
The source is at <https://github.com/webcpporg/document> (directory `js`).

Distributed under the Boost Software License, Version 1.0 (`LICENSE_1_0.txt`).
