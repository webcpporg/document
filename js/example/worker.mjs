// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The worker: it opens the image it receives, which is a document of its own memory, and answers
// with what the document holds. Nothing is parsed here.

import { Document } from '../webcpp-document.mjs'

self.onmessage = ({ data }) => {
    const doc = Document.open(new Uint8Array(data.image))
    self.postMessage({
        name: doc.get(['user', 'name']),
        age: doc.get(['user', 'age']),
        langs: doc.get(['user', 'langs']),
        price: doc.getDecimal(['price']),
        text: doc.stringify(),
    })
    doc.free()
}
self.postMessage('ready')
