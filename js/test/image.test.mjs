// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Images: a document's bytes as a view of WebAssembly memory, the copy that opens in place and the
// copy that opens from bytes outside the memory, and what an image refuses.

import assert from 'node:assert/strict'
import { describe, test } from 'node:test'
import { Document } from '../webcpp-document.mjs'

const text = '{"user":{"name":"Ada","langs":["C","Note G"]},"n":[1.5,null,true]}'

describe('image', () => {
    test('is a Uint8Array view of memory: no copy on each call', () => {
        const doc = Document.parse(text)
        const first = doc.image()
        const second = doc.image()
        assert.ok(first instanceof Uint8Array)
        assert.equal(first.buffer, second.buffer)
        assert.equal(first.byteLength, doc.byteLength)
        doc.free()
    })

    test('round trips: open of the bytes gives the same document', () => {
        const doc = Document.parse(text)
        const opened = Document.open(doc.image())
        assert.equal(opened.stringify(), doc.stringify())
        assert.deepEqual(opened.toJSON(), JSON.parse(text))
        doc.free()
        opened.free()
    })

    test('open of a copy, from bytes outside the memory, reads them and copies them in', () => {
        const doc = Document.parse(text)
        const copy = doc.image().slice()
        const opened = Document.open(copy)
        // The document is its own: the bytes it was opened from may change or go.
        copy.fill(0)
        assert.equal(opened.stringify(), doc.stringify())
        assert.equal(opened.get(['user', 'name']), 'Ada')
        doc.free()
        opened.free()
    })

    test('open in place keeps the bytes it is given', () => {
        const doc = Document.parse(text)
        const view = doc.image()
        const opened = Document.open(view)
        assert.equal(opened.image().buffer, view.buffer)
        assert.equal(opened.stringify(), doc.stringify())
        doc.free()
        opened.free()
    })

    test('open refuses bytes that are not an image as TypeError', () => {
        assert.throws(() => Document.open(new Uint8Array(64)), TypeError)
        assert.throws(() => Document.open(new Uint8Array(3)), TypeError)
        assert.throws(() => Document.open(new TextEncoder().encode(text)), TypeError)
    })

    test('free releases the document; a second free does nothing', () => {
        const doc = Document.parse(text)
        const opened = Document.open(doc.image().slice())
        opened.free()
        opened.free()
        assert.throws(() => opened.get(['user']), TypeError)
        doc.free()
    })
})
