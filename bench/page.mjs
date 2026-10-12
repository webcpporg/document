// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The benchmarks in Chrome: bench.html loads this module, and run.mjs calls window.runBench through
// Playwright, once per implementation, in a page of its own. run.mjs serves the page cross-origin
// isolated (COOP and COEP), so that performance.now reads 5 us, not 100; the data under /data/
// and the Emscripten module under /build/.

import {dataFiles, runJavaScript, runWasm} from './bench.mjs'

// Resolves to every line the implementation printed: `js`, JavaScript's objects, or `wasm`,
// bench.cpp compiled by Emscripten, its implementation `impl`.
window.runBench = async ({what, impl, quick}) => {
    if (!crossOriginIsolated) throw new Error('the page is not cross-origin isolated: performance.now reads 100 us')
    const lines = []
    const print = line => {
        lines.push(line)
        console.log(line)
    }
    const bytes = await Promise.all(dataFiles.map(async name => {
        const response = await fetch(`/data/${name}`)
        if (!response.ok) throw new Error(`/data/${name}: ${response.status}`)
        return new Uint8Array(await response.arrayBuffer())
    }))
    if (what === 'js') {
        const decoder = new TextDecoder()
        const [canada, citm, twitter] = bytes.map(b => decoder.decode(b))
        runJavaScript({
            texts: {canada, citm, twitter},
            gc: globalThis.gc,
            now: () => performance.now(),
            print,
            meta: {engine: 'chrome', userAgent: navigator.userAgent, crossOriginIsolated},
            quick,
        })
    } else {
        const factory = (await import('/build/bench-module.js')).default
        const files = Object.fromEntries(dataFiles.map((name, i) => [name, bytes[i]]))
        await runWasm({factory, files, impl, print, quick})
    }
    return lines
}
