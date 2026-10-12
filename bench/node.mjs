// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Runs one implementation of the benchmarks in Node, its lines on the standard output:
//
//   node --expose-gc node.mjs js --data <dir> [--quick]
//   node --expose-gc node.mjs wasm --module <bench-module.js> --impl <name> --data <dir> [--quick]
//
// js measures JavaScript's own objects (bench.mjs); wasm runs bench.cpp compiled by Emscripten,
// whose implementation --impl names, over the same files. run.mjs runs each in a process of its own.

import {readFileSync} from 'node:fs'
import {join, resolve} from 'node:path'
import {pathToFileURL} from 'node:url'
import {dataFiles, runJavaScript, runWasm} from './bench.mjs'

const args = process.argv.slice(2)
const option = name => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined)
const mode = args[0]
const data = option('--data')
const quick = args.includes('--quick')
if (!data || (mode !== 'js' && mode !== 'wasm')) {
    throw new Error('usage: node --expose-gc node.mjs js|wasm --data <dir> [--module <js> --impl <name>] [--quick]')
}
const print = line => process.stdout.write(`${line}\n`)

if (mode === 'js') {
    const [canada, citm, twitter] = dataFiles.map(name => readFileSync(join(data, name), 'utf8'))
    runJavaScript({
        texts: {canada, citm, twitter},
        gc: globalThis.gc,
        now: () => performance.now(),
        print,
        meta: {engine: 'node', node: process.version, v8: process.versions.v8},
        quick,
    })
} else {
    const module = option('--module')
    const impl = option('--impl')
    if (!module || !impl) throw new Error('wasm needs --module <bench-module.js> and --impl <name>')
    const factory = (await import(pathToFileURL(resolve(module)).href)).default
    const files = Object.fromEntries(dataFiles.map(name => [name, readFileSync(join(data, name))]))
    await runWasm({factory, files, impl, print, quick})
}
