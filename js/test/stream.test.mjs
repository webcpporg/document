// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Streams of documents: a ReadableStream of NDJSON chunks yields documents, and a sender written in
// JavaScript that sends a series of JSON texts, chunk by chunk, to the parser compiled from C++,
// which parses them and delivers each document as it completes.

import assert from 'node:assert/strict'
import { describe, test } from 'node:test'
import { Document, StreamParser } from '../webcpp-document.mjs'

const encoder = new TextEncoder()
const docs = [{ a: 1 }, { b: 'caf\u00e9 \u{1f600}' }, [1, 2, { c: null }], { d: 'x\ny' }, { e: [] }]
const lines = docs.map(doc => JSON.stringify(doc)).join('\n') + '\n'

// A ReadableStream whose chunks are the bytes of text cut every `size` bytes, so that a chunk ends
// inside a multi-byte character and a line.
const chunked = (bytes, size) => new ReadableStream({
    start(controller) {
        for (let i = 0; i < bytes.length; i += size) controller.enqueue(bytes.slice(i, i + size))
        controller.close()
    },
})

// The sender: a JavaScript function that hands the texts to a parser chunk by chunk, and yields
// the documents the parser delivers after each chunk.
function* send(parser, chunks) {
    for (const chunk of chunks) {
        parser.write(chunk)
        for (let doc; (doc = parser.read()) !== undefined;) yield doc
    }
    parser.finish()
    for (let doc; (doc = parser.read()) !== undefined;) yield doc
}

describe('StreamParser.from', () => {
    test('yields the documents of an NDJSON ReadableStream, whatever the chunks', async () => {
        for (const size of [1, 3, 7, 4096]) {
            const out = []
            for await (const doc of StreamParser.from(chunked(encoder.encode(lines), size), { framing: 'lines' })) {
                assert.ok(doc instanceof Document)
                out.push(doc.toJSON())
            }
            assert.deepEqual(out, docs, `chunks of ${size} bytes`)
        }
    })

    test('a bad line throws SyntaxError after the documents before it', async () => {
        const seen = []
        await assert.rejects(async () => {
            for await (const doc of StreamParser.from(chunked(encoder.encode('{"a":1}\n{bad\n[2]\n'), 5),
                { framing: 'lines' })) {
                seen.push(doc.toJSON())
            }
        }, SyntaxError)
        assert.deepEqual(seen, [{ a: 1 }])
    })

    test('on error skip, a bad line is dropped and the stream goes on', async () => {
        const seen = []
        for await (const doc of StreamParser.from(chunked(encoder.encode('{"a":1}\n{bad\n[2]\n'), 5),
            { framing: 'lines', onError: 'skip' })) {
            seen.push(doc.toJSON())
        }
        assert.deepEqual(seen, [{ a: 1 }, [2]])
    })

    test('concatenated framing reads documents with no separator between them', async () => {
        const out = []
        for await (const doc of StreamParser.from(chunked(encoder.encode('{"a":1}{"b":[2]}\n [3]'), 2))) {
            out.push(doc.toJSON())
        }
        assert.deepEqual(out, [{ a: 1 }, { b: [2] }, [3]])
    })
})

describe('the sender', () => {
    test('a series of JSON texts sent chunk by chunk reaches the C++ parser and is delivered', () => {
        const texts = docs.map(doc => JSON.stringify(doc))
        const bytes = encoder.encode(texts.join('\n') + '\n')
        const chunks = []
        for (let i = 0; i < bytes.length; i += 6) chunks.push(bytes.slice(i, i + 6))
        const parser = new StreamParser({ framing: 'lines' })
        const delivered = [...send(parser, chunks)].map(doc => doc.stringify())
        assert.deepEqual(delivered, texts.map(text => JSON.stringify(JSON.parse(text))))
    })

    test('a document is delivered only once its text is complete', () => {
        const parser = new StreamParser({ framing: 'lines' })
        parser.write(encoder.encode('{"a":'))
        assert.equal(parser.read(), undefined)
        parser.write(encoder.encode('1}\n'))
        assert.deepEqual(parser.read().toJSON(), { a: 1 })
        assert.equal(parser.read(), undefined)
        parser.finish()
    })
})
