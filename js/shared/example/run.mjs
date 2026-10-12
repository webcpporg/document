// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Drives the example page in headless Chromium, through Playwright (js/package.json pins it):
// node run.mjs <package directory> <expected lines>. The package is served cross-origin isolated,
// which a SharedArrayBuffer needs. It prints the page's lines and fails unless they are the
// expected ones and the page raised no error. The browser is the Chrome that the
// environment variable CHROME names, else Playwright's own Chromium.

import { createServer } from 'node:http'
import { existsSync, readFileSync } from 'node:fs'
import { extname, join, normalize, resolve } from 'node:path'
import { chromium } from 'playwright'

const root = resolve(process.argv[2] ?? '')
const expected = readFileSync(process.argv[3] ?? '', 'utf8').trimEnd()
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
    await page.goto(`http://127.0.0.1:${server.address().port}/example/shared-workers.html`)
    await page.waitForFunction(() => document.getElementById('result').textContent !== 'running', null, { timeout: 30000 })
    const text = await page.textContent('#result')
    console.log(text)
    if (text !== expected || errors.length > 0) {
        throw new Error(`the page did not print the expected lines${errors.length ? `: ${errors.join('; ')}` : ''}`)
    }
} finally {
    await browser.close()
    server.close()
}
