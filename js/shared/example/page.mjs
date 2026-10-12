// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The page: a document parsed here is read by four workers at once, each through a token of one
// reference, in the page's own memory. One worker then makes the document its own and changes its
// copy, while the page and the others keep reading the original. The page prints what it saw,
// lines that example/run.mjs compares with shared-workers.expected.

import { Document, connection, pool } from '../webcpp-document-shared.mjs'

const lines = []
const say = line => lines.push(line)

// A worker's answer to each command, in order.
const start = () => {
    const worker = new Worker(new URL('./reader.mjs', import.meta.url), { type: 'module' })
    const answers = []
    worker.onmessage = ({ data }) => answers.shift()(data)
    return {
        send: message => new Promise(resolve => {
            answers.push(resolve)
            worker.postMessage(message)
        }),
        stop: () => worker.terminate(),
    }
}

try {
    say(`cross-origin isolated: ${crossOriginIsolated}`)
    const free = pool.available
    const doc = Document.parse('{"user":{"name":"Ada","langs":["C","Note G"]},"price":1.50}')
    const address = doc.image().byteOffset
    say(`page: parsed into one block of the pool, use count ${doc.useCount}`)

    const workers = Array.from({ length: 4 }, start)
    const adopted = await Promise.all(workers.map(worker =>
        worker.send({ command: 'adopt', connection: connection(), token: doc.share() })))
    adopted.forEach((seen, i) => say(`worker ${i + 1}: reads ${seen.name} and ${seen.langs.join(', ')}, ` +
        `at the page's address: ${seen.address === address}, in a SharedArrayBuffer: ${seen.shared}`))
    say(`page: use count ${doc.useCount}, blocks in use ${free - pool.available}`)

    const written = await workers[3].send({ command: 'write', name: 'Grace' })
    say(`worker 4: makeMutable, then set: reads ${written.name}, at the page's address: ${written.address === address}`)
    say(`page: reads ${doc.get(['user', 'name'])}, use count ${doc.useCount}, blocks in use ${free - pool.available}`)

    await Promise.all(workers.map(worker => worker.send({ command: 'free' })))
    say(`page: every worker freed its reference, use count ${doc.useCount}`)
    doc.free()
    say(`page: freed, every block back in the pool: ${pool.available === free}`)
    workers.forEach(worker => worker.stop())
} catch (error) {
    say(`error: ${error.name}: ${error.message}`)
}
document.getElementById('result').textContent = lines.join('\n')
