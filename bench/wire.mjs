// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The sizes on the wire of each file, for the page's transmission rows: the JSON text that
// JSON.stringify writes, the text Boost.JSON serializes, and document's image after shrink_to_fit,
// each as it is, gzipped and brotli-compressed, with Node's zlib at its default levels. The Boost.JSON
// text and the image are written by `bench-native --wire <dir>`, which must run first. Not timed: a
// size is not a time.
//
//   node wire.mjs --data <dir> --wire <dir>

import {readFileSync} from 'node:fs'
import {join} from 'node:path'
import {brotliCompressSync, gzipSync} from 'node:zlib'

const args = process.argv.slice(2)
const option = name => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined)
const data = option('--data')
const wire = option('--wire')
if (!data || !wire) throw new Error('usage: node wire.mjs --data <dir> --wire <dir>')

// The size of `bytes` as it is, gzipped and brotli-compressed.
const sizes = bytes => ({
    bytes: bytes.length,
    gzip: gzipSync(bytes).length,
    brotli: brotliCompressSync(bytes).length,
})

const files = [['canada', 'canada.json'], ['citm', 'citm_catalog.json'], ['twitter', 'twitter.json']]
const out = {}
for (const [name, file] of files) {
    const text = readFileSync(join(data, file), 'utf8')
    out[name] = {
        json_stringify: sizes(Buffer.from(JSON.stringify(JSON.parse(text)), 'utf8')),
        boost_json: sizes(readFileSync(join(wire, `${name}.boost.json`))),
        image: sizes(readFileSync(join(wire, `${name}.image`))),
    }
}
console.log(JSON.stringify(out, null, 4))
