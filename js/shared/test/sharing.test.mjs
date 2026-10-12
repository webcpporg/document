// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Documents shared between worker threads: workers that join this thread's memory adopt one
// document and read the same image at once, the use count follows them, the last holder gives the
// block back, a writer copies on write while readers keep the original, and a token is taken over
// once. The suite runs as `node --expose-gc --test`, and the workers inherit the flag.

import assert from 'node:assert/strict'
import { after, describe, test } from 'node:test'
import { Worker } from 'node:worker_threads'
import { Document, SharedDocument, connection, pool } from '../webcpp-document-shared.mjs'

const text = '{"user":{"name":"Ada","langs":["C","Note G"]},"price":1.50}'

// A worker that has joined this thread's memory, and the commands of holder.worker.mjs sent to it,
// one at a time: each resolves with the worker's answer, or rejects with the error it threw.
class Holder {
    #worker = new Worker(new URL('./holder.worker.mjs', import.meta.url))
    #pending = Promise.resolve()

    static async start() {
        const holder = new Holder()
        assert.equal(await holder.send('join', { connection: connection() }), 'joined')
        return holder
    }

    send(command, fields = {}) {
        const answer = this.#pending.then(() => new Promise((resolve, reject) => {
            this.#worker.once('message', ({ ok, error }) => {
                if (error === undefined) {
                    resolve(ok)
                    return
                }
                reject(Object.assign(new Error(error.message), { name: error.name }))
            })
            this.#worker.postMessage({ command, ...fields })
        }))
        this.#pending = answer.catch(() => {})
        return answer
    }

    stop() {
        return this.#worker.terminate()
    }
}

const holders = await Promise.all(Array.from({ length: 4 }, () => Holder.start()))
after(() => Promise.all(holders.map(holder => holder.stop())))

const collectUntil = async done => {
    for (let turn = 0; turn < 100 && !done(); turn++) {
        globalThis.gc()
        await new Promise(resolve => setTimeout(resolve, 5))
    }
}

describe('a document shared by workers', () => {
    test('four workers adopt one document and read the same image at once', async () => {
        const free = pool.available
        const doc = Document.parse(text)
        assert.equal(pool.available, free - 1)
        assert.equal(doc.useCount, 1)

        const adopted = await Promise.all(holders.map(holder =>
            holder.send('adopt', { token: doc.share(), as: 'doc' })))
        assert.equal(doc.useCount, 5)
        assert.equal(pool.available, free - 1, 'adopting copies nothing into another block')
        for (const seen of adopted) {
            assert.equal(seen.address, doc.image().byteOffset, 'every worker reads the same bytes')
            assert.equal(seen.shared, true)
        }

        const reads = await Promise.all(holders.map(holder =>
            holder.send('read', { as: 'doc', times: 2000 })))
        for (const read of reads) {
            assert.equal(read.same, true)
            assert.equal(read.name, 'Ada')
            assert.deepEqual(read.langs, ['C', 'Note G'])
            assert.equal(read.price, '1.5')
            assert.equal(read.text, doc.stringify())
        }

        await holders[0].send('free', { as: 'doc' })
        assert.equal(doc.useCount, 4)
        await Promise.all(holders.slice(1).map(holder => holder.send('free', { as: 'doc' })))
        assert.equal(doc.useCount, 1)
        assert.equal(pool.available, free - 1)
        doc.free()
        assert.equal(pool.available, free, 'the last holder gives the block back')
    })

    test('a writer that makes the document mutable changes its copy, the readers keep the original', async () => {
        const free = pool.available
        const doc = Document.parse(text)
        const [writer, ...readers] = holders
        await Promise.all(holders.map(holder => holder.send('adopt', { token: doc.share(), as: 'doc' })))
        assert.equal(doc.useCount, 5)

        const [written, ...reads] = await Promise.all([
            writer.send('write', { as: 'doc', name: 'Grace', times: 200 }),
            ...readers.map(reader => reader.send('read', { as: 'doc', times: 2000 })),
        ])
        assert.equal(written.name, 'Grace')
        assert.equal(written.moved, true)
        assert.equal(written.useCount, 1)
        assert.notEqual(written.address, doc.image().byteOffset)
        for (const read of reads) {
            assert.equal(read.same, true)
            assert.equal(read.name, 'Ada')
        }
        assert.equal(doc.get(['user', 'name']), 'Ada')
        assert.equal(doc.useCount, 4)
        assert.equal(pool.available, free - 2)

        await Promise.all(holders.map(holder => holder.send('free', { as: 'doc' })))
        assert.equal(doc.useCount, 1)
        doc.free()
        assert.equal(pool.available, free)
    })

    test('a document a worker forgets without free() is released by its garbage collector', async () => {
        const doc = Document.parse(text)
        await holders[0].send('adopt', { token: doc.share(), as: 'doc' })
        assert.equal(doc.useCount, 2)
        await holders[0].send('forget', { as: 'doc' })
        assert.equal(doc.useCount, 1)
        doc.free()
    })

    test('a worker that leaves drops what it holds and can join again', async () => {
        const free = pool.available
        const doc = Document.parse(text)
        const holder = await Holder.start()
        try {
            await holder.send('adopt', { token: doc.share(), as: 'doc' })
            assert.equal(doc.useCount, 2)
            assert.equal(await holder.send('leave'), 'left')
            assert.equal(doc.useCount, 1)
            await assert.rejects(holder.send('read', { as: 'doc', times: 1 }), /no document/)
            assert.equal(await holder.send('join', { connection: connection() }), 'joined')
            await holder.send('adopt', { token: doc.share(), as: 'doc' })
            assert.equal(doc.useCount, 2)
            await holder.send('free', { as: 'doc' })
            await holder.send('leave')
        } finally {
            await holder.stop()
        }
        doc.free()
        assert.equal(pool.available, free)
    })
})

describe('joining', () => {
    // The slots claimed at this moment: each worker that joined holds one.
    const claimed = () => {
        const { memory, states, slots } = connection()
        return new Int32Array(memory.buffer, states, slots.length).reduce((sum, word) => sum + word, 0)
    }

    test('each worker claims a slot; joining twice at once claims one', async () => {
        const before = claimed()
        assert.ok(before >= holders.length)
        const holder = new Holder()
        try {
            assert.equal(await holder.send('joinTwice', { connection: connection() }), 'joined')
            assert.equal(claimed(), before + 1)
            await holder.send('leave')
            assert.equal(claimed(), before)
        } finally {
            await holder.stop()
        }
    })

    test('join refuses what is not a connection', async () => {
        const holder = new Holder()
        try {
            await assert.rejects(holder.send('join', { connection: { key: 'x' } }), { name: 'TypeError' })
            const token = { offset: 1, ticket: 0, generation: 1 }
            await assert.rejects(holder.send('adopt', { token, as: 'doc' }), /has not joined/)
        } finally {
            await holder.stop()
        }
    })
})

describe('a token', () => {
    test('is a small structured-cloneable object that keeps one reference until it is adopted', () => {
        const doc = Document.parse(text)
        const token = doc.share()
        assert.deepEqual(structuredClone(token), token)
        assert.equal(doc.useCount, 2)
        const adopted = SharedDocument.adopt(structuredClone(token))
        assert.equal(doc.useCount, 2)
        assert.equal(adopted.image().byteOffset, doc.image().byteOffset)
        adopted.free()
        assert.equal(doc.useCount, 1)
        doc.free()
    })

    test('is taken over once: a spent token is refused, in this thread and in a worker', async () => {
        const doc = Document.parse(text)
        const token = doc.share()
        await holders[0].send('adopt', { token, as: 'doc' })
        assert.throws(() => SharedDocument.adopt(token), TypeError)
        await assert.rejects(holders[1].send('adopt', { token, as: 'doc' }), { name: 'TypeError' })
        assert.equal(doc.useCount, 2)
        await holders[0].send('free', { as: 'doc' })
        doc.free()
    })

    test('of a dropped document is refused, even when its block holds another document', () => {
        const free = pool.available
        const doc = Document.parse(text)
        const token = doc.share()
        SharedDocument.drop(token)
        assert.equal(doc.useCount, 1)
        assert.throws(() => SharedDocument.adopt(token), TypeError)
        assert.throws(() => SharedDocument.drop(token), TypeError)
        const address = doc.image().byteOffset
        doc.free()
        assert.equal(pool.available, free)

        const next = Document.parse('{"user":{"name":"Grace"}}')
        assert.equal(next.image().byteOffset, address, 'the pool hands the same block out again')
        assert.throws(() => SharedDocument.adopt(token), TypeError)
        assert.equal(next.useCount, 1)
        next.free()
    })

    test('that is not a token is refused', () => {
        for (const token of [undefined, null, 42, 'x', {}, { offset: 1, ticket: 0, generation: 1 }]) {
            assert.throws(() => SharedDocument.adopt(token), TypeError, String(token))
        }
    })
})

describe('the pool', () => {
    test('a document dropped without free() goes back to the pool when the collector finds it', async () => {
        const free = pool.available
        const forget = () => {
            for (let i = 0; i < 3; i++) Document.parse(text)
        }
        forget()
        assert.equal(pool.available, free - 3)
        await collectUntil(() => pool.available === free)
        assert.equal(pool.available, free)
    })

    test('a document is the pool\'s block: a change past its room is a RangeError', () => {
        const doc = Document.parse(text)
        assert.ok(doc.byteLength <= pool.blockBytes)
        assert.throws(() => doc.set(['big'], 'x'.repeat(pool.blockBytes)), RangeError)
        assert.equal(doc.get(['user', 'name']), 'Ada')
        doc.free()
    })

    test('a freed document throws TypeError, and a second free does nothing', () => {
        const doc = Document.parse(text)
        doc.free()
        doc.free()
        assert.throws(() => doc.get(['user']), TypeError)
        assert.throws(() => doc.share(), TypeError)
    })

    test('parse throws SyntaxError where JSON.parse does', () => {
        const free = pool.available
        for (const bad of ['{', '[1,]', '']) assert.throws(() => Document.parse(bad), SyntaxError, bad)
        assert.equal(pool.available, free)
    })
})
