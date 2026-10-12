// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A Document that is dropped without free() is released when the garbage collector finds it: the
// module's live handles count goes back. This needs the collector exposed, so the suite runs as
// `node --expose-gc --test`.

import assert from 'node:assert/strict'
import { test } from 'node:test'
import { Document, diagnostics } from '../webcpp-document.mjs'

// Makes documents and forgets them: no reference to any of them outlives this call.
const makeAndDrop = count => {
    for (let i = 0; i < count; i++) Document.parse(`{"i":${i}}`)
}

test('a Document dropped without free() is released by the garbage collector', async () => {
    assert.equal(typeof globalThis.gc, 'function', 'run with node --expose-gc')
    const before = diagnostics.liveDocuments()
    makeAndDrop(50)
    assert.equal(diagnostics.liveDocuments(), before + 50)
    for (let turn = 0; turn < 200 && diagnostics.liveDocuments() > before; turn++) {
        globalThis.gc()
        await new Promise(resolve => setTimeout(resolve, 5))
    }
    assert.equal(diagnostics.liveDocuments(), before)
})

test('a freed Document is not released a second time when the collector finds it', async () => {
    const before = diagnostics.liveDocuments()
    const doc = Document.parse('[1]')
    doc.free()
    assert.equal(diagnostics.liveDocuments(), before)
    for (let turn = 0; turn < 20; turn++) {
        globalThis.gc()
        await new Promise(resolve => setTimeout(resolve, 5))
    }
    assert.equal(diagnostics.liveDocuments(), before)
})
