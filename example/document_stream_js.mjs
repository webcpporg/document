// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The JavaScript side of document_stream_js.cpp, run by Node under webcpp.drive:
//
//     node document_stream_js.mjs <expected output> <program>
//
// It sends two streams to the C++ parser in chunks of bytes, prints what C++ delivers after each
// chunk, and fails unless the lines are the ones of the expected output. The program is the
// Emscripten module that webcpp.drive builds from document_stream_js.cpp.

import { readFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import { resolve } from 'node:path'

const [expectedPath, program] = process.argv.slice(2)
const createModule = createRequire(import.meta.url)(resolve(program))
const module = await createModule()

const lines = []
const encoder = new TextEncoder()

// tag::send[]
// Sends a text to a C++ stream in chunks of the given size and prints what each chunk completes.
// The text is encoded first, so that a chunk may end inside a character, as a socket's may.
function send(framing, text, size) {
    let chunk = 0
    const stream = new module.JsonStream(framing,
        document => lines.push(`  chunk ${chunk}: ${document}`),
        (line, message) => lines.push(`  chunk ${chunk}: line ${line} dropped, ${message}`))
    const bytes = encoder.encode(text)
    lines.push(`${framing}, in chunks of ${size} bytes:`)
    for (let at = 0; at < bytes.length; at += size) {
        chunk += 1
        stream.write(bytes.subarray(at, at + size))
    }
    chunk += 1
    stream.finish()
    stream.delete()
}
// end::send[]

// tag::streams[]
send('lines', '{"id":1,"tags":["a","b"]}\n{"id":2,"name":"Zoé"}\n{"bad":}\n[3,4.5]\n"last"', 9)
send('concatenated', '{"a":1}    {"b":[2,3]}[4]"five" 6', 4)
// end::streams[]

const printed = lines.join('\n')
console.log(printed)
if (printed !== readFileSync(expectedPath, 'utf8').trimEnd()) {
    throw new Error('the lines printed are not the expected ones')
}
