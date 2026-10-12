// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A worker of the tests: it joins the memory of the thread that started it, then answers each
// command it is sent with what its documents read, or with the error a command threw.

import { parentPort } from 'node:worker_threads'
import { SharedDocument, join, leave } from '../webcpp-document-shared.mjs'

// The documents this worker holds, by the name the test gives each.
const held = new Map()

const holding = name => {
    const doc = held.get(name)
    if (doc === undefined) throw new Error(`no document named ${name}`)
    return doc
}

// What a holder sees of a document: its values, its use count and where its image is in memory.
const describe = doc => ({
    name: doc.get(['user', 'name']),
    langs: doc.get(['user', 'langs']),
    price: doc.getDecimal(['price']),
    text: doc.stringify(),
    useCount: doc.useCount,
    address: doc.image().byteOffset,
    shared: doc.image().buffer instanceof SharedArrayBuffer,
})

const commands = {
    async join({ connection }) {
        await join(connection)
        return 'joined'
    },
    // Joins with two calls at once, as two messages that arrive together do.
    async joinTwice({ connection }) {
        await Promise.all([join(connection), join(connection)])
        return 'joined'
    },
    adopt({ token, as }) {
        held.set(as, SharedDocument.adopt(token))
        return describe(holding(as))
    },
    // Reads the document `times` times and answers whether every read gave what the first did.
    read({ as, times }) {
        const doc = holding(as)
        const first = describe(doc)
        let same = true
        for (let i = 0; i < times; i++) {
            same &&= doc.get(['user', 'name']) === first.name && doc.stringify() === first.text
        }
        return { ...first, same }
    },
    // Makes the document this worker's own and writes `name` into it, `times` times.
    write({ as, name, times }) {
        const doc = holding(as)
        const before = doc.image().byteOffset
        doc.makeMutable()
        for (let i = 0; i < times; i++) doc.set(['user', 'name'], `${name} ${i}`)
        doc.set(['user', 'name'], name)
        return { ...describe(doc), moved: doc.image().byteOffset !== before }
    },
    share({ as }) {
        return holding(as).share()
    },
    free({ as }) {
        holding(as).free()
        held.delete(as)
        return 'freed'
    },
    // Forgets the document without free(), and collects until the registry has released it.
    async forget({ as }) {
        held.delete(as)
        for (let turn = 0; turn < 50; turn++) {
            globalThis.gc()
            await new Promise(resolve => setTimeout(resolve, 5))
        }
        return 'forgotten'
    },
    leave() {
        held.clear()
        leave()
        return 'left'
    },
}

parentPort.on('message', async message => {
    try {
        parentPort.postMessage({ ok: await commands[message.command](message) })
    } catch (error) {
        parentPort.postMessage({ error: { name: error.name, message: error.message } })
    }
})
