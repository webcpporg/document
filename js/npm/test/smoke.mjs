// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What a user of the package does, run in a directory that installed the tarball: every import is
// of '@webcpp/document', never of a path in the source tree. The examples of the README are here
// with their results.

import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { test } from 'node:test'
import { Worker } from 'node:worker_threads'
import { Document, StreamParser, diagnostics } from '@webcpp/document'

test('parse, set, push, get and stringify (README, Node)', () => {
    const doc = Document.parse('{"user":{"name":"Ada"},"price":1.5}')
    doc.set(['user', 'langs'], ['C', 'Note G'])
    doc.push(['user', 'langs'], 'JavaScript')
    assert.equal(doc.get(['user', 'name']), 'Ada')
    assert.equal(doc.length(['user', 'langs']), 3)
    assert.equal(doc.stringify(), '{"user":{"name":"Ada","langs":["C","Note G","JavaScript"]},"price":1.5}')
    assert.equal(doc.stringify(2), JSON.stringify(JSON.parse(doc.stringify()), null, 2))
    assert.deepEqual(doc.toJSON(), JSON.parse(doc.stringify()))
    doc.free()
})

test('keys keep JavaScript order, and keysInRange queries them', () => {
    const doc = Document.parse('{"b":1,"2":2,"a":3,"1":4,"user_a":0,"user_b":0}')
    assert.deepEqual(Object.keys(doc.get([])), ['1', '2', 'b', 'a', 'user_a', 'user_b'])
    assert.deepEqual(doc.keysInRange([], 'user_', 'user_￿'), ['user_a', 'user_b'])
    doc.free()
})

test('exact numbers, and their caveat (README)', () => {
    const doc = Document.parse('{"n":12345678901234567890,"big":[1e400],"price":0.1234567890123456789012345678901234}')
    assert.equal(doc.getDecimal(['n']), '12345678901234567890')
    assert.equal(doc.get(['n']), 12345678901234567000)
    assert.equal(doc.getDecimal(['price']), '0.1234567890123456789012345678901234')
    assert.equal(doc.stringify(), '{"n":12345678901234567890,"big":[1e+400],"price":0.1234567890123456789012345678901234}')
    assert.equal(doc.get(['big', 0]), Infinity)
    assert.equal(JSON.stringify(JSON.parse('[1e400]')), '[null]')
    doc.free()
})

test('an image round trips, in place and from a copy', () => {
    const doc = Document.parse('{"user":{"name":"Ada"},"price":1.5}')
    const view = doc.image()
    assert.ok(view instanceof Uint8Array)
    assert.equal(view.byteLength, doc.byteLength)
    const inPlace = Document.open(view)
    assert.equal(inPlace.stringify(), doc.stringify())
    const bytes = doc.image().slice()
    const again = Document.open(bytes)
    bytes.fill(0)
    assert.equal(again.get(['price']), 1.5)
    assert.equal(again.stringify(), doc.stringify())
    assert.throws(() => Document.open(new Uint8Array(64)), TypeError)
    for (const each of [doc, inPlace, again]) each.free()
})

test('a sealed image is checked by open', () => {
    const doc = Document.parse('[1,2,3]')
    doc.seal()
    const copy = doc.image().slice()
    assert.equal(Document.open(copy).stringify(), '[1,2,3]')
    copy[copy.length - 1] ^= 0xff
    assert.throws(() => Document.open(copy), TypeError)
    doc.free()
})

test('a document is transferred to a worker, which opens it with no parse (README)', async () => {
    const doc = Document.parse('{"price":1.5,"n":12345678901234567890}')
    const bytes = doc.image().slice()
    doc.free()
    const worker = new Worker(new URL('./worker.mjs', import.meta.url))
    const answered = new Promise((resolve, reject) => {
        worker.once('message', resolve)
        worker.once('error', reject)
    })
    worker.postMessage({ image: bytes.buffer }, [bytes.buffer])
    assert.equal(bytes.byteLength, 0, 'the transfer detached the buffer')
    assert.equal(await answered, '{"price":1.5,"n":12345678901234567890}')
    await worker.terminate()
})

test('StreamParser reads NDJSON from chunks and from a ReadableStream (README)', async () => {
    const parser = new StreamParser({ framing: 'lines' })
    parser.write('{"id":1}\n{"i')
    assert.equal(parser.read()?.get(['id']), 1)
    assert.equal(parser.read(), undefined)
    parser.write('d":2}\n')
    assert.equal(parser.read()?.get(['id']), 2)
    parser.finish()

    const encoder = new TextEncoder()
    const stream = new ReadableStream({
        start(controller) {
            for (const chunk of ['{"id":1}\nnot json\n{"i', 'd":3}\n']) controller.enqueue(encoder.encode(chunk))
            controller.close()
        },
    })
    const ids = []
    for await (const doc of StreamParser.from(stream, { framing: 'lines', onError: 'skip' })) {
        ids.push(doc.get(['id']))
        doc.free()
    }
    assert.deepEqual(ids, [1, 3])

    const skipping = new StreamParser({ framing: 'lines', onError: 'skip' })
    skipping.write('{"id":1}\nnot json\n{"id":2}\n')
    skipping.finish()
    assert.equal(skipping.read()?.get(['id']), 1)
    assert.equal(skipping.read()?.get(['id']), 2)
    assert.equal(skipping.dropped, 1)
})

test('errors are the ones JavaScript throws', () => {
    assert.throws(() => Document.parse('{'), SyntaxError)
    const doc = Document.parse('{"a":[1]}')
    assert.throws(() => doc.get('a'), TypeError)
    assert.throws(() => doc.length(['a', 0]), TypeError)
    assert.throws(() => doc.setLength(['a'], -1), RangeError)
    doc.free()
    assert.throws(() => doc.get([]), TypeError)
    doc.free()
})

test('the package ships its manifest, its licence and a module that finds its wasm by URL', () => {
    const manifest = JSON.parse(readFileSync(new URL(import.meta.resolve('@webcpp/document/package.json'))))
    assert.equal(manifest.name, '@webcpp/document')
    assert.match(manifest.version, /^\d+\.\d+\.\d+/)
    assert.equal(manifest.license, 'BSL-1.0')
    const core = readFileSync(new URL('./webcpp-document-core.js', import.meta.resolve('@webcpp/document')), 'utf8')
    assert.match(core, /new URL\("webcpp-document-core\.wasm",import\.meta\.url\)/)
    assert.equal(typeof diagnostics.heapBytesInUse(), 'number')
    assert.throws(() => import.meta.resolve('@webcpp/document/webcpp-document-core.js'), /not exported|ERR_PACKAGE_PATH_NOT_EXPORTED/)
})
