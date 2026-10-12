// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A Document that is dropped without free() is released when the garbage collector finds it: the
// heap's bytes in use go back to where they were. This needs the collector exposed, so the suite
// runs as `node --expose-gc --test`.

import assert from 'node:assert/strict'
import { test } from 'node:test'
import { Document, diagnostics } from '../webcpp-document.mjs'

// Each document holds about 32 KiB, so 50 of them are far above the heap's other allocations.
const sizeable = i => JSON.stringify({ i, text: 'x'.repeat(32 * 1024) })
const documentBytes = 32 * 1024

// The bytes the heap may keep above its baseline once the collector has run: allocator noise,
// not a document.
const margin = 4 * 1024

// Makes documents and forgets them: no reference to any of them outlives this call.
const makeAndDrop = count => {
    for (let i = 0; i < count; i++) Document.parse(sizeable(i))
}

// Runs the collector until the heap is back within the margin of the baseline, or gives up.
const collectUntilBack = async baseline => {
    for (let turn = 0; turn < 200 && diagnostics.heapBytesInUse() > baseline + margin; turn++) {
        globalThis.gc()
        await new Promise(resolve => setTimeout(resolve, 5))
    }
}

test('a Document dropped without free() is released by the garbage collector', async () => {
    assert.equal(typeof globalThis.gc, 'function', 'run with node --expose-gc')
    const before = diagnostics.heapBytesInUse()
    makeAndDrop(50)
    assert.ok(diagnostics.heapBytesInUse() >= before + 50 * documentBytes)
    await collectUntilBack(before)
    assert.ok(diagnostics.heapBytesInUse() <= before + margin)
})

test('a freed Document is not released a second time when the collector finds it', async () => {
    const before = diagnostics.heapBytesInUse()
    const doc = Document.parse(sizeable(0))
    assert.ok(diagnostics.heapBytesInUse() >= before + documentBytes)
    doc.free()
    const afterFree = diagnostics.heapBytesInUse()
    assert.ok(afterFree <= before + margin)
    for (let turn = 0; turn < 20; turn++) {
        globalThis.gc()
        await new Promise(resolve => setTimeout(resolve, 5))
    }
    assert.equal(diagnostics.heapBytesInUse(), afterFree)
})
