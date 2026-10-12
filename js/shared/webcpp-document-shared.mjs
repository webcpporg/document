// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// webcpp::document for JavaScript, shared between workers: documents in a pool of blocks of one
// WebAssembly memory, a SharedArrayBuffer, which every worker that joins it reads in place, with no
// transfer and no copy. Its module, webcpp-document-shared-core.js, sits beside this file.
//
// The main thread (a page, or Node's main thread) starts the module when it imports this file. A
// worker the program creates, with new Worker(...), receives connection() in a message and awaits
// join(connection); from then on it runs the same module on the same memory, on a stack of its own,
// until it calls leave(), which drops what it holds and gives the stack to the next worker.
//
//     // the page
//     const doc = Document.parse(text)
//     worker.postMessage({ connection: connection(), token: doc.share() })
//
//     // the worker
//     onmessage = async ({ data }) => {
//         await join(data.connection)
//         const doc = SharedDocument.adopt(data.token)
//         doc.get(['user', 'name'])
//     }
//
// A Document holds one reference to its document, as std::shared_ptr does: share() gives a token,
// a small object that keeps one reference more until a thread adopts it, once. Every holder reads
// the same bytes; a write through one is every holder's, and makeMutable() first makes the
// document the writer's own copy when others hold it. The last reference dropped, by free() or by
// the garbage collector, gives the block back to the pool. Document and SharedDocument are the same
// class: every document of this module is in the pool.
//
// It needs a SharedArrayBuffer: a page served cross-origin isolated (Cross-Origin-Opener-Policy:
// same-origin and Cross-Origin-Embedder-Policy: require-corp), or Node.

import createCore from './webcpp-document-shared-core.js'

// The memory every thread shares, laid out once by the main thread: the pool's blocks (the largest
// image a document has), the tokens in flight at once, and the workers joined at once. The module's
// memory is 128 MiB and does not grow (its Jamfile).
const layout = Object.freeze({
    blockBytes: 512 * 1024,
    blocks: 64,
    tickets: 1024,
    workers: 32,
    stackBytes: 64 * 1024,
})

// The JavaScript error of a failed operation: errc's values, as the C++ side numbers them.
const errorFor = (code, message) => {
    switch (code) {
        case 1: return new SyntaxError(message)
        case 3: // range
        case 4: // capacity
            return new RangeError(message)
        default: // type, not_a_document, incompatible_document, damaged_document
            return new TypeError(message)
    }
}

const ok = outcome => {
    if (outcome.code !== 0) throw errorFor(outcome.code, outcome.message)
    return outcome.value
}

const checkPath = path => {
    if (!Array.isArray(path)) throw new TypeError('a path is an array of keys and indices')
    for (const step of path) {
        if (typeof step === 'string' || (Number.isSafeInteger(step) && step >= 0)) continue
        throw new TypeError(`a path step is a string key or a non-negative integer index, not ${String(step)}`)
    }
    return path
}

const jsonText = value => {
    switch (typeof value) {
        case 'function':
        case 'symbol':
        case 'bigint':
            throw new TypeError(`a ${typeof value} is not a JSON value`)
        case 'undefined':
            return undefined
        default:
            return JSON.stringify(value)
    }
}

const isMainThread = globalThis.process?.versions?.node
    ? (await import('node:worker_threads')).isMainThread
    : globalThis.WorkerGlobalScope === undefined

// This thread's runtime: the module, its view of the shared memory, the connection it gives to
// workers, and, in a worker, the slot it claimed and the join under way.
let core = null
let space = null
let shared = null
let claimed = -1
let joining = null

// The handles this thread holds and has not freed: free() and leave() delete them, and the
// registry those the garbage collector finds first.
const handles = new Set()
const release = native => {
    if (handles.delete(native)) native.delete()
}
const registry = new FinalizationRegistry(release)

const running = () => {
    if (space === null) {
        throw new TypeError(isMainThread ? 'the module did not start' : 'this worker has not joined: await join(connection) first')
    }
    return space
}

const attach = () => {
    const native = new core.NativeSpace(shared.pool.address, shared.pool.bytes, shared.tickets.address,
        shared.tickets.count)
    if (native.code() !== 0) {
        native.delete()
        throw new TypeError('the connection names no pool of this module')
    }
    space = native
}

if (isMainThread) {
    core = await createCore()
    const made = ok(core.NativeSpace.create(layout.blockBytes, layout.blocks, layout.tickets, layout.workers,
        layout.stackBytes))
    shared = Object.freeze({ key: crypto.randomUUID(), ...core.webcppShared(), ...made })
    attach()
}

// What a worker needs to join this thread's memory: the module, the memory, the addresses of what
// they share and the workers' slots. It is structured-cloneable, and the same for every worker.
export function connection() {
    running()
    return shared
}

const isConnection = value =>
    value !== null && typeof value === 'object' && typeof value.key === 'string' &&
    value.module instanceof WebAssembly.Module && value.memory instanceof WebAssembly.Memory &&
    value.memory.buffer instanceof SharedArrayBuffer && typeof value.pool?.address === 'number' &&
    typeof value.tickets?.address === 'number' && typeof value.states === 'number' && Array.isArray(value.slots)

// Joins a worker to the memory of the connection: it claims a free slot, a stack with its
// thread-local storage and its pthread, instantiates the module and runs it there. Joining the same
// memory again, while the first join is under way or after it, waits for that one.
export async function join(connection) {
    if (!isConnection(connection)) throw new TypeError('join takes the connection() of the thread that started the module')
    if (joining !== null || shared !== null) {
        if ((joining?.key ?? shared.key) !== connection.key) {
            throw new TypeError('this thread already runs a module of another memory')
        }
        return joining?.done
    }
    joining = { key: connection.key, done: joinNow(connection) }
    try {
        await joining.done
    } finally {
        joining = null
    }
}

async function joinNow(connection) {
    const words = new Int32Array(connection.memory.buffer)
    const count = connection.slots.length
    let slot = 0
    while (slot < count && Atomics.compareExchange(words, connection.states / 4 + slot, 0, 1) !== 0) slot++
    if (slot === count) throw new RangeError(`every one of the ${count} workers' slots is taken`)
    try {
        const joined = await createCore({ webcppJoining: true })
        joined.webcppJoin(connection.module, connection.memory, connection.slots[slot])
        core = joined
        shared = connection
        claimed = slot
        attach()
    } catch (error) {
        core = shared = null
        claimed = -1
        Atomics.store(words, connection.states / 4 + slot, 0)
        throw error
    }
}

// Leaves the memory a worker joined: drops every reference the worker holds and gives its slot
// back for another worker. The worker may join again afterwards.
export function leave() {
    if (isMainThread) throw new TypeError('the main thread started the module and cannot leave it')
    running()
    for (const native of handles) release(native)
    space.delete()
    Atomics.store(new Int32Array(shared.memory.buffer), shared.states / 4 + claimed, 0)
    core = space = shared = null
    claimed = -1
}

// The pool every document lives in, the same in every thread.
export const pool = Object.freeze({
    // The blocks free at this moment, which other threads may change at any moment.
    get available() {
        return running().available()
    },
    get blocks() {
        return running().blocks()
    },
    // The room of each block: the largest image a document has.
    get blockBytes() {
        return running().blockBytes()
    },
})

const secret = Symbol('webcpp-document-shared')

const checked = native => {
    if (native.code() !== 0) {
        const error = errorFor(native.code(), native.message())
        native.delete()
        throw error
    }
    return native
}

const isToken = token =>
    token !== null && typeof token === 'object' &&
    ['offset', 'ticket', 'generation'].every(key => Number.isSafeInteger(token[key]) && token[key] >= 0)

export class Document {
    #native

    constructor(native, token) {
        if (token !== secret) throw new TypeError('a Document is made by Document.parse or SharedDocument.adopt')
        this.#native = native
        handles.add(native)
        registry.register(this, native, this)
    }

    // JSON.parse into a block of the pool: a new document, this holder its only one.
    static parse(text) {
        return new Document(checked(running().parse(String(text))), secret)
    }

    // Takes over the reference a token keeps, in any thread joined to the same memory. A token is
    // taken over once: one already adopted or dropped is refused with TypeError, as is one whose
    // document is gone, even when its block holds another document since.
    static adopt(token) {
        if (!isToken(token)) throw new TypeError('a token is what share() returns')
        const native = running().adopt(token)
        if (native.code() === 3) {
            native.delete()
            throw new TypeError('the token is spent: adopted or dropped already, or not of this memory')
        }
        return new Document(checked(native), secret)
    }

    // Gives the reference a token keeps back without adopting it, as a receiver that does not want
    // the document does; TypeError for a token spent already.
    static drop(token) {
        if (!isToken(token)) throw new TypeError('a token is what share() returns')
        const outcome = running().drop(token)
        if (outcome.code === 3) throw new TypeError('the token is spent: adopted or dropped already, or not of this memory')
        ok(outcome)
    }

    #live() {
        if (this.#native === null || !handles.has(this.#native)) throw new TypeError('the document is freed')
        return this.#native
    }

    // A token that keeps one reference more, for another thread to adopt: { offset, ticket,
    // generation }, structured-cloneable.
    share() {
        return ok(running().share(this.#live()))
    }

    // How many references the document has, in every thread, tokens in flight included.
    get useCount() {
        return this.#live().useCount()
    }

    // Makes the document this holder's alone to change: the same block when no one else holds it,
    // else a copy in a new block, while the others keep the document as it was. RangeError when
    // the pool has no free block.
    makeMutable() {
        ok(this.#live().makeMutable())
    }

    get(path) {
        checkPath(path)
        return ok(this.#live().get(path))
    }

    // The exact decimal of a number, as text.
    getDecimal(path) {
        checkPath(path)
        return ok(this.#live().getDecimal(path))
    }

    length(path) {
        checkPath(path)
        return ok(this.#live().length(path))
    }

    // Writes in place, where every holder reads it: call makeMutable() first to write a copy of
    // this holder's own. A change past the block's room is RangeError.
    set(path, value) {
        checkPath(path)
        ok(this.#live().set(path, jsonText(value)))
    }

    // JSON.stringify(value, null, indent) of the document.
    stringify(indent = 0) {
        const number = Math.trunc(Number(indent)) || 0
        const text = this.#live().stringify(Math.min(10, Math.max(0, number)))
        return text === '' ? undefined : text
    }

    toJSON() {
        return this.get([])
    }

    get byteLength() {
        return this.#live().byteLength()
    }

    // The image: a Uint8Array view of the shared memory, at the same address in every thread.
    image() {
        return this.#live().image()
    }

    // Drops this holder's reference now; the last one gives the block back. Using the document
    // afterwards throws TypeError.
    free() {
        if (this.#native === null) return
        registry.unregister(this)
        release(this.#native)
        this.#native = null
    }
}

export { Document as SharedDocument }
