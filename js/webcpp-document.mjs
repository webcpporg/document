// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// webcpp::document for JavaScript: Document, a JSON document in one relocatable image, and
// StreamParser, which reads documents from bytes as they arrive. The module the Emscripten build
// makes, webcpp-document-core.js, sits beside this file: the classes here wrap its handles with
// JavaScript's values and errors, so that what they throw is what JSON.parse and JSON.stringify
// throw.
//
// Numbers keep their exact decimal digits (the core's default traits). So stringify writes the
// digits a double does not keep, where JSON.stringify(JSON.parse(text)) rounds them: it writes
// 12345678901234567890 where JavaScript writes 12345678901234567000, and 1e+400 where JavaScript
// writes null, since its number is Infinity. getDecimal gives the exact decimal, and get the
// Number that JavaScript rounds it to.

import createCore from './webcpp-document-core.js'

const core = await createCore()

// Measurements of the module's memory, for the tests of the garbage collector: heapBytesInUse is
// the bytes of the wasm heap the allocator has handed out and not taken back.
export const diagnostics = Object.freeze({
    heapBytesInUse: () => core.heapBytesInUse(),
})

// Frees the handles of documents and stream parsers that were never freed: a handle is held here
// until its object is collected, and released by free() before that.
const registry = new FinalizationRegistry(native => native.delete())

const secret = Symbol('webcpp-document')

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

// A path is an array of keys (strings) and indices (non-negative integers), from the root.
const checkPath = path => {
    if (!Array.isArray(path)) throw new TypeError('a path is an array of keys and indices')
    for (const step of path) {
        if (typeof step === 'string' || (Number.isSafeInteger(step) && step >= 0)) continue
        throw new TypeError(`a path step is a string key or a non-negative integer index, not ${String(step)}`)
    }
    return path
}

// The JSON text of a value, as JSON.stringify writes it; undefined for undefined, which the
// document stores as nothing. A value that is not JSON (a function, a symbol, a BigInt) is refused.
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

// An element of an array: undefined is null there, as JSON.stringify writes it.
const elementText = value => jsonText(value) ?? 'null'

// The integer that JavaScript's splice takes for a position or a count.
const integer = value => {
    const number = Number(value)
    return Number.isNaN(number) ? 0 : Math.trunc(number)
}

const toBytes = chunk => {
    if (typeof chunk === 'string') return new TextEncoder().encode(chunk)
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk)
    if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength)
    throw new TypeError('a chunk is a string, an ArrayBuffer or a view of bytes')
}

export class Document {
    #native

    constructor(native, token) {
        if (token !== secret) throw new TypeError('a Document is made by Document.parse or Document.open')
        this.#native = native
        registry.register(this, native, this)
    }

    // JSON.parse: a new document of the text, which throws SyntaxError where JSON.parse does.
    static parse(text) {
        const native = core.parseText(String(text))
        if (native.code() !== 0) {
            const error = errorFor(native.code(), native.message())
            native.delete()
            throw error
        }
        return new Document(native, secret)
    }

    // The document whose image is the bytes: opened in place when they are a 16-aligned view of
    // wasm memory, from a copy in memory otherwise. The bytes are kept by the caller.
    static open(bytes) {
        if (!ArrayBuffer.isView(bytes) && !(bytes instanceof ArrayBuffer)) {
            throw new TypeError('an image is a typed array, a DataView or an ArrayBuffer')
        }
        const view = toBytes(bytes)
        const native = core.openBytes(view)
        if (native.code() !== 0) {
            const error = errorFor(native.code(), native.message())
            native.delete()
            throw error
        }
        return new Document(native, secret)
    }

    #live() {
        if (this.#native === null) throw new TypeError('the document is freed')
        return this.#native
    }

    get(path) {
        checkPath(path)
        return ok(this.#live().get(path))
    }

    set(path, value) {
        checkPath(path)
        const json = jsonText(value)
        ok(this.#live().set(path, json))
    }

    erase(path) {
        checkPath(path)
        return ok(this.#live().erase(path))
    }

    // push(path, ...values): the new length, as Array.prototype.push gives it.
    push(path, ...values) {
        checkPath(path)
        const native = this.#live()
        let length = ok(native.length(path))
        for (const value of values) length = ok(native.push(path, elementText(value)))
        return length
    }

    // splice(path, start, deleteCount, ...items): the removed elements, as Array.prototype.splice
    // gives them; without a deleteCount, every element from start is removed.
    splice(path, start, ...rest) {
        checkPath(path)
        const native = this.#live()
        const length = ok(native.length(path))
        const from = Math.min(Math.max(integer(start) < 0 ? length + integer(start) : integer(start), 0), length)
        const count = rest.length === 0
            ? length - from
            : Math.min(Math.max(integer(rest[0]), 0), length - from)
        const items = rest.slice(1).map(elementText)
        return ok(native.splice(path, from, count, items))
    }

    length(path) {
        checkPath(path)
        return ok(this.#live().length(path))
    }

    // The exact decimal of a number, as text, for a number the Number of get would round.
    getDecimal(path) {
        checkPath(path)
        return ok(this.#live().getDecimal(path))
    }

    // The keys of the object at path in [low, high), in key order: a prefix query is the range
    // from the prefix to the prefix followed by U+FFFF.
    keysInRange(path, low, high) {
        checkPath(path)
        if (typeof low !== 'string' || typeof high !== 'string') throw new TypeError('the bounds are strings')
        return ok(this.#live().keysInRange(path, low, high))
    }

    // JSON.stringify(value, null, indent) of the document; undefined when it holds undefined. The
    // numbers are written from their exact digits, as the module's header says.
    stringify(indent = 0) {
        const native = this.#live()
        const text = native.stringify(Math.min(10, Math.max(0, integer(indent))))
        return text === '' ? undefined : text
    }

    // A plain copy of the document's value.
    toJSON() {
        return this.get([])
    }

    get byteLength() {
        return this.#live().byteLength()
    }

    // The bytes the image can hold before a change must move it.
    get capacity() {
        return this.#live().capacity()
    }

    // Makes the image hold at least bytes, as Array.prototype would make room for its length; a
    // size past the document's limit is RangeError.
    reserve(bytes) {
        if (!Number.isSafeInteger(bytes) || bytes < 0) throw new RangeError(`a size is a non-negative integer, not ${String(bytes)}`)
        ok(this.#live().reserve(bytes))
    }

    // arr.pop() and arr.shift(): the removed element, undefined from an empty array.
    pop(path) {
        checkPath(path)
        return ok(this.#live().pop(path))
    }

    shift(path) {
        checkPath(path)
        return ok(this.#live().shift(path))
    }

    // arr.unshift(...values): the new length. The values go in one at a time, last first, so that
    // they stand in front in their order.
    unshift(path, ...values) {
        checkPath(path)
        const native = this.#live()
        let length = ok(native.length(path))
        for (const text of values.map(elementText).reverse()) length = ok(native.unshift(path, text))
        return length
    }

    // arr.reverse(), in place. JavaScript gives the array back; the document gives nothing, as
    // the oracle records it.
    reverse(path) {
        checkPath(path)
        ok(this.#live().reverse(path))
    }

    // arr.length = n: truncates, or extends with holes that JSON writes as null.
    setLength(path, n) {
        checkPath(path)
        if (!Number.isSafeInteger(n) || n < 0) throw new RangeError('Invalid array length')
        ok(this.#live().setLength(path, n))
    }

    // Object.assign(target, source): copies the members of source into the object at path, in
    // their order. null and undefined add nothing, as Object.assign ignores them; any other scalar
    // is a TypeError, and so is a target that is not an object.
    assign(path, source) {
        checkPath(path)
        if (source !== null && typeof source !== 'object' && source !== undefined) {
            throw new TypeError(`Object.assign copies an object's members, not a ${typeof source}`)
        }
        ok(this.#live().assign(path, jsonText(source) ?? 'null'))
    }

    // The image: a Uint8Array view of the wasm memory the document is in, without a copy. It is
    // valid until the memory grows or the document changes.
    image() {
        return this.#live().image()
    }

    // Writes the CRC of the image into it, which open checks; a change clears it.
    seal() {
        ok(this.#live().seal())
    }

    // Releases the document's memory now. Using it afterwards throws TypeError.
    free() {
        if (this.#native === null) return
        registry.unregister(this)
        this.#native.delete()
        this.#native = null
    }
}

// A parser of a stream of documents: write() takes the bytes as they arrive, read() gives each
// document once it is complete, and finish() ends the stream.
export class StreamParser {
    #native
    #skip
    #dropped = 0

    // framing: 'concatenated' (the default), 'lines' (NDJSON) or 'rfc7464' (JSON text sequences).
    // onError: 'stop' (the default) or 'skip', which drops a bad document and goes on, not with
    // 'concatenated', which has no place to go on from.
    constructor({ framing = 'concatenated', onError = 'stop' } = {}) {
        if (!['concatenated', 'lines', 'rfc7464'].includes(framing)) {
            throw new TypeError(`framing is 'concatenated', 'lines' or 'rfc7464', not ${String(framing)}`)
        }
        if (!['stop', 'skip'].includes(onError)) throw new TypeError(`onError is 'stop' or 'skip', not ${String(onError)}`)
        if (framing === 'concatenated' && onError === 'skip') {
            throw new TypeError("onError 'skip' needs a framing that can go on: 'lines' or 'rfc7464'")
        }
        this.#skip = onError === 'skip'
        this.#native = new core.NativeStream(framing, this.#skip)
        registry.register(this, this.#native, this)
    }

    #live() {
        if (this.#native === null) throw new TypeError('the stream parser is finished')
        return this.#native
    }

    // How many documents a skip dropped: the bad ones, whose error the stream reports in their place.
    get dropped() {
        return this.#dropped
    }

    write(chunk) {
        ok(this.#live().write(toBytes(chunk)))
    }

    // The next complete document, or undefined when none is complete yet. The error that ends a
    // stream is thrown once the documents before it are taken; a dropped document is counted.
    read() {
        const native = this.#live()
        for (;;) {
            const next = native.next()
            if (next.code() === 0) {
                if (!next.has()) {
                    next.delete()
                    return undefined
                }
                return new Document(next, secret)
            }
            const error = errorFor(next.code(), next.message())
            next.delete()
            if (!this.#skip) throw error
            this.#dropped += 1
        }
    }

    finish() {
        ok(this.#live().finish())
    }

    // The documents of a stream of chunks: a ReadableStream of bytes or strings, or any async
    // iterable of them. Each document is yielded as soon as its bytes have arrived.
    static async *from(source, options) {
        const parser = new StreamParser(options)
        const chunks = source[Symbol.asyncIterator] ? source : readerChunks(source.getReader())
        for await (const chunk of chunks) {
            // The documents a failing write completed come out before its error does.
            try {
                parser.write(chunk)
            } finally {
                yield* drain(parser)
            }
        }
        parser.finish()
        yield* drain(parser)
    }
}

async function* readerChunks(reader) {
    try {
        for (;;) {
            const { done, value } = await reader.read()
            if (done) return
            yield value
        }
    } finally {
        reader.releaseLock()
    }
}

function* drain(parser) {
    for (let document; (document = parser.read()) !== undefined;) yield document
}
