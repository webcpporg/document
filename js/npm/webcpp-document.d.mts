// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Declarations of webcpp-document.mjs, written by hand: embind's own (webcpp-document-core.d.ts)
// describe the module the wrapper uses, not the wrapper.

/** A JSON value, as JSON.parse gives it. */
export type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue }

/**
 * A place in a document, from its root: string keys and non-negative integer indices. The empty
 * path is the root itself. A step that is neither throws TypeError.
 */
export type Path = ReadonlyArray<string | number>

/** What a stream parser takes: text, which is encoded as UTF-8, or bytes. */
export type Chunk = string | ArrayBuffer | ArrayBufferView

/** How the documents of a stream are separated. */
export type Framing = 'concatenated' | 'lines' | 'rfc7464'

export interface StreamParserOptions {
    /** 'concatenated' (the default), 'lines' (NDJSON) or 'rfc7464' (JSON text sequences). */
    framing?: Framing
    /**
     * 'stop' (the default) ends the stream with the error; 'skip' drops the bad document and goes
     * on, and needs a framing that can go on: 'lines' or 'rfc7464'.
     */
    onError?: 'stop' | 'skip'
}

/** What a ReadableStream gives, without needing the DOM's or Node's types. */
export interface ChunkReadable {
    getReader(): {
        read(): Promise<{ done: boolean; value?: Chunk }>
        releaseLock(): void
    }
}

/** Measurements of the module's memory, for tests of the garbage collector. */
export const diagnostics: Readonly<{
    /** The bytes of the WebAssembly heap that the allocator has handed out and not taken back. */
    heapBytesInUse(): number
}>

/**
 * A JSON document in one relocatable image, in WebAssembly memory. Numbers keep their exact
 * decimal digits and objects keep JavaScript's key order. Errors are the ones JSON.parse and the
 * array methods throw: SyntaxError for text that is not JSON, RangeError for a size or an index
 * out of range, TypeError for a path or a value of the wrong kind and for a document that was freed.
 */
export class Document {
    private constructor()

    /** JSON.parse: a new document of the text; throws the SyntaxError JSON.parse throws. */
    static parse(text: string): Document

    /**
     * The document whose image is the bytes: opened in place when they are a 16-aligned view of
     * WebAssembly memory, from a copy otherwise. Throws TypeError for bytes that are not an image.
     */
    static open(bytes: ArrayBuffer | ArrayBufferView): Document

    /**
     * The value at path: a scalar as its JavaScript value, a container as a copy, and undefined
     * where the path leads to nothing.
     */
    get(path: Path): JsonValue | undefined

    /** Writes what JSON.stringify writes for the value; undefined erases a member. */
    set(path: Path, value: unknown): void

    /** Removes a member, and says whether it was there. */
    erase(path: Path): boolean

    /** Array.prototype.push: the new length. */
    push(path: Path, ...values: unknown[]): number

    /** Array.prototype.splice: the removed elements. Without a count, every element from start. */
    splice(path: Path, start: number): JsonValue[]
    splice(path: Path, start: number, deleteCount: number, ...items: unknown[]): JsonValue[]

    /** The length of the array at path. */
    length(path: Path): number

    /**
     * The exact decimal of the number at path, as text: the digits that get, which gives the
     * Number JavaScript rounds them to, does not keep.
     */
    getDecimal(path: Path): string

    /**
     * The keys of the object at path in [low, high), in key order. A prefix query is the range
     * from the prefix to the prefix followed by '￿'.
     */
    keysInRange(path: Path, low: string, high: string): string[]

    /**
     * JSON.stringify(value, null, indent) of the document, written from the exact digits of its
     * numbers; undefined when it holds undefined. The indent is clamped to 0 to 10 spaces.
     */
    stringify(indent?: number): string | undefined

    /** A plain copy of the document's value, so that JSON.stringify(document) works. */
    toJSON(): JsonValue | undefined

    /** The size of the image in bytes. */
    readonly byteLength: number

    /** The bytes the image can hold before a change must move it. */
    readonly capacity: number

    /** Makes the image hold at least this many bytes; RangeError past the document's limit. */
    reserve(bytes: number): void

    /** Array.prototype.pop: the removed element, undefined from an empty array. */
    pop(path: Path): JsonValue | undefined

    /** Array.prototype.shift: the removed element, undefined from an empty array. */
    shift(path: Path): JsonValue | undefined

    /** Array.prototype.unshift: the new length. */
    unshift(path: Path, ...values: unknown[]): number

    /** Array.prototype.reverse, in place; the document gives nothing back. */
    reverse(path: Path): void

    /** array.length = n: truncates, or extends with holes that JSON writes as null. */
    setLength(path: Path, n: number): void

    /**
     * Object.assign(target, source): copies the members of source into the object at path, in
     * their order. null and undefined add nothing.
     */
    assign(path: Path, source: unknown): void

    /**
     * The image: a view of the WebAssembly memory the document is in, with no copy. It is valid
     * until the memory grows or the document changes; slice() it to keep or transfer it.
     */
    image(): Uint8Array

    /** Writes the CRC of the image into it, which Document.open checks; a change clears it. */
    seal(): void

    /** Releases the document's memory now. A freed document throws TypeError when used. */
    free(): void
}

/**
 * A parser of a stream of documents: write() takes the bytes as they arrive, read() gives each
 * document once it is complete, and finish() ends the stream.
 */
export class StreamParser {
    constructor(options?: StreamParserOptions)

    /** How many documents onError 'skip' dropped. */
    readonly dropped: number

    /** Feeds a chunk; throws when the stream cannot go on. */
    write(chunk: Chunk): void

    /**
     * The next complete document, or undefined when none is complete yet. The error that ends a
     * stream is thrown once the documents before it have been taken.
     */
    read(): Document | undefined

    /** Ends the stream; throws if it ends inside a document. */
    finish(): void

    /**
     * The documents of a stream of chunks, a ReadableStream or any async iterable of strings or
     * bytes. Each document is yielded as soon as its bytes have arrived.
     */
    static from(
        source: ChunkReadable | AsyncIterable<Chunk>,
        options?: StreamParserOptions,
    ): AsyncGenerator<Document, void, undefined>
}
