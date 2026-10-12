// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A worker of the example: it joins the page's memory, adopts the document whose token the page
// sent and reads it in place. It answers where the image it reads is, which is where the page's
// is: the same bytes, never copied.

import { SharedDocument, join } from '../webcpp-document-shared.mjs'

let doc = null

const describe = () => ({
    name: doc.get(['user', 'name']),
    langs: doc.get(['user', 'langs']),
    address: doc.image().byteOffset,
    shared: doc.image().buffer instanceof SharedArrayBuffer,
})

const commands = {
    async adopt({ connection, token }) {
        await join(connection)
        doc = SharedDocument.adopt(token)
        return describe()
    },
    // The document made this worker's own, a copy in a block of its own, then changed.
    write({ name }) {
        doc.makeMutable()
        doc.set(['user', 'name'], name)
        return describe()
    },
    free() {
        doc.free()
        return {}
    },
}

onmessage = async ({ data }) => {
    postMessage(await commands[data.command](data))
}
