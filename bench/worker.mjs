// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The Worker of the Worker benchmarks, in Node (node.mjs) and in Chrome (page.mjs). bench.mjs asks it
// one request at a time, and it replies when the request is done, so that the time of a pass is the
// whole send. It holds what it received: a structured clone of the object, and the image of a
// document, which its own module opens in place.

import {statisticsOf} from './bench.mjs'

// Node's Worker talks through worker_threads, the browser's through self.
const threads = typeof process === 'object' && process.versions?.node
    ? await import('node:worker_threads')
    : null
const reply = message => (threads ? threads.parentPort.postMessage(message) : self.postMessage(message))
const listen = handler => {
    if (threads) threads.parentPort.on('message', handler)
    else self.onmessage = event => handler(event.data)
}

let module = null
let clone = null

const handle = async message => {
    switch (message.op) {
        case 'init': {
            const factory = (await import(message.moduleUrl)).default
            module = await factory({noInitialRun: true, print: () => {}, printErr: () => {}})
            return {}
        }
        case 'clone':
            clone = message.value
            return {}
        case 'transfer': {
            // The buffer is taken before the bytes are copied in: a growing memory replaces its view.
            const pointer = module._webcpp_bench_receive(message.size)
            module.HEAPU8.set(new Uint8Array(message.bytes), pointer)
            if (!module._webcpp_bench_open(message.size)) throw new Error('the image was refused')
            return {}
        }
        case 'answer': {
            const answer = message.which === 'clone'
                ? statisticsOf(clone).json()
                : JSON.parse(module.UTF8ToString(module._webcpp_bench_statistics()))
            return {answer}
        }
        default:
            throw new Error(`unknown request ${message.op}`)
    }
}

listen(async message => {
    try {
        reply(await handle(message))
    } catch (error) {
        reply({error: error.stack ?? String(error)})
    }
})
