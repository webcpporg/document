// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Drives the example page in headless Chromium, through Playwright (package.json pins it, as the
// benchmarks pin it): node run.mjs <package directory>. The package is served cross-origin
// isolated, as the benchmarks serve it. It prints the page's result and fails unless the worker
// answered what the document holds and the transfer detached the buffer. The browser is the Chrome
// that the environment variable CHROME names, else Playwright's own Chromium.

import { createServer } from 'node:http'
import { existsSync, readFileSync } from 'node:fs'
import { extname, join, normalize, resolve } from 'node:path'
import { chromium } from 'playwright'

const root = resolve(process.argv[2] ?? '')
const types = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm' }

const server = createServer((request, response) => {
    const path = decodeURIComponent(new URL(request.url, 'http://localhost').pathname)
    const file = normalize(join(root, path))
    if (!file.startsWith(root) || !existsSync(file) || file.endsWith('/')) {
        response.writeHead(404).end()
        return
    }
    response.writeHead(200, {
        'content-type': types[extname(file)] ?? 'application/octet-stream',
        'cross-origin-opener-policy': 'same-origin',
        'cross-origin-embedder-policy': 'require-corp',
        'cross-origin-resource-policy': 'same-origin',
        'cache-control': 'no-store',
    }).end(readFileSync(file))
})
await new Promise(listening => server.listen(0, '127.0.0.1', listening))

const browser = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {})
try {
    const page = await browser.newPage()
    const errors = []
    page.on('pageerror', error => errors.push(error.message))
    await page.goto(`http://127.0.0.1:${server.address().port}/example/image-worker.html`)
    await page.waitForFunction(() => document.getElementById('result').textContent !== 'running', null, { timeout: 30000 })
    const text = await page.textContent('#result')
    console.log(text)
    const answer = JSON.parse(text)
    const expected = { name: 'Ada', age: 36, langs: ['C', 'Note G'], price: '1.5',
        text: '{"user":{"name":"Ada","langs":["C","Note G"],"age":36},"price":1.5}' }
    const matches = Object.entries(expected).every(([key, value]) => JSON.stringify(answer[key]) === JSON.stringify(value))
    if (!matches || answer.detached !== true || errors.length > 0) {
        throw new Error(`the page did not answer as expected${errors.length ? `: ${errors.join('; ')}` : ''}`)
    }
} finally {
    await browser.close()
    server.close()
}
