// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A TypeScript program that uses the package's declarations, checked by tsc --noEmit in a
// directory that installed the tarball. Every line marked @ts-expect-error must be an error.

import { Document, StreamParser, diagnostics } from '@webcpp/document'
import type { Chunk, JsonValue, Path, StreamParserOptions } from '@webcpp/document'

const doc: Document = Document.parse('{"user":{"name":"Ada","langs":["C"]},"n":[1.5,null]}')
const path: Path = ['user', 'langs', 0]

const value: JsonValue | undefined = doc.get(path)
const name = doc.get(['user', 'name'])
if (typeof name === 'string') name.toUpperCase()
const length: number = doc.length(['user', 'langs'])
const exact: string = doc.getDecimal(['n', 0])
const keys: string[] = doc.keysInRange(['user'], 'a', 'z')
const text: string | undefined = doc.stringify(2)
const copy: JsonValue | undefined = doc.toJSON()
const image: Uint8Array = doc.image()
const size: number = doc.byteLength + doc.capacity

doc.set(['user', 'age'], 36)
const erased: boolean = doc.erase(['user', 'age'])
const pushed: number = doc.push(['user', 'langs'], 'Note G', { any: 'json' })
const removed: JsonValue[] = doc.splice(['user', 'langs'], 1)
const removedOne: JsonValue[] = doc.splice(['user', 'langs'], 0, 1, 'C', 'C++')
const popped: JsonValue | undefined = doc.pop(['user', 'langs'])
const shifted: JsonValue | undefined = doc.shift(['user', 'langs'])
const unshifted: number = doc.unshift(['user', 'langs'], 'first')
doc.reverse(['user', 'langs'])
doc.setLength(['user', 'langs'], 1)
doc.assign(['user'], { extra: true })
doc.reserve(4096)
doc.seal()

const opened: Document = Document.open(image.slice())
const fromBuffer: Document = Document.open(new ArrayBuffer(0).slice(0))
opened.free()

const options: StreamParserOptions = { framing: 'lines', onError: 'skip' }
const parser = new StreamParser(options)
const chunk: Chunk = '{"a":1}\n'
parser.write(chunk)
parser.write(new TextEncoder().encode(chunk))
const next: Document | undefined = parser.read()
const dropped: number = parser.dropped
parser.finish()

async function documents(source: AsyncIterable<string>): Promise<number> {
    let count = 0
    for await (const each of StreamParser.from(source, options)) {
        each.free()
        count += 1
    }
    return count
}

async function documentsOfStream(stream: ReadableStream<Uint8Array>): Promise<Document[]> {
    const out: Document[] = []
    for await (const each of StreamParser.from(stream)) out.push(each)
    return out
}

const heap: number = diagnostics.heapBytesInUse()

// @ts-expect-error a path is an array
doc.get('user')
// @ts-expect-error a path step is a string or a number
doc.get([true])
// @ts-expect-error the framing is one of three
new StreamParser({ framing: 'csv' })
// @ts-expect-error a Document is made by parse or open
new Document()
// @ts-expect-error the bounds are strings
doc.keysInRange([], 1, 2)
// @ts-expect-error a stream is not a number
StreamParser.from(3)
// @ts-expect-error the core's classes are not exported
import { NativeDocument } from '@webcpp/document'

export { value, length, exact, keys, text, copy, size, erased, pushed, removed, removedOne, popped }
export { shifted, unshifted, fromBuffer, next, dropped, documents, documentsOfStream, heap }
