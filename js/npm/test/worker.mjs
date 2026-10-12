// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The worker of smoke.mjs: it opens the image it is sent and answers with the document's text.

import { parentPort } from 'node:worker_threads'
import { Document } from '@webcpp/document'

parentPort.once('message', ({ image }) => {
    const doc = Document.open(new Uint8Array(image))
    parentPort.postMessage(doc.stringify())
    doc.free()
})
