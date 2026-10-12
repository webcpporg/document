// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The page: a document is parsed here, its image is copied and the copy's buffer is transferred to
// a worker, which opens the image in its own memory with no parse. The page reports what the
// worker answered and that the transfer detached the buffer.

import { Document } from '../webcpp-document.mjs'

const result = document.getElementById('result')
const text = '{"user":{"name":"Ada","langs":["C","Note G"]},"price":1.50}'

const doc = Document.parse(text)
doc.set(['user', 'age'], 36)
// The image of the document is a view of this page's memory, which cannot be transferred; its
// copy is an ArrayBuffer of its own.
const bytes = doc.image().slice()
doc.free()

// The worker says it is ready once its module has loaded: a message sent earlier waits for the
// module, which imports the wrapper and its module, and may not be delivered.
const worker = new Worker(new URL('./worker.mjs', import.meta.url), { type: 'module' })
worker.onmessage = ({ data }) => {
    if (data === 'ready') {
        worker.postMessage({ image: bytes.buffer }, [bytes.buffer])
        return
    }
    result.textContent = JSON.stringify({ ...data, detached: bytes.byteLength === 0 }, null, 4)
    worker.terminate()
}
worker.onerror = event => {
    result.textContent = JSON.stringify({ error: String(event.message) })
}
