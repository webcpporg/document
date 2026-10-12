// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The oracle's cases, replayed through Document: each text parses and writes as JSON.stringify
// writes it, and each operation gives the text and the return the oracle recorded from JavaScript
// itself (test/oracle/oracle.mjs describes the case and expected files).
//
// A case the oracle marks js_only holds a number that a double does not keep. Document keeps its
// exact digits (default_traits), so its bytes differ from JSON.stringify's by design: such a case
// runs under its own name, and it agrees once the document's text is read as JavaScript reads it,
// with JSON.parse, which rounds each number to a double as the oracle does.

import assert from 'node:assert/strict'
import { readdirSync, readFileSync } from 'node:fs'
import { test } from 'node:test'
import { Document } from '../webcpp-document.mjs'

const fixtures = new URL('../fixtures/', import.meta.url)
const load = (directory, name) => JSON.parse(readFileSync(new URL(`${directory}/${name}`, fixtures), 'utf8'))
const names = readdirSync(new URL('cases/', fixtures)).filter(name => name.endsWith('.json')).sort()

// The text an operation's return is recorded as: JSON.stringify of it, null for nothing.
const recorded = value => (value === undefined ? null : JSON.stringify(value))

// The text as JavaScript reads it: its numbers rounded to doubles, as JSON.parse rounds them.
const asJavaScript = (text, indent) => (text === null ? null : JSON.stringify(JSON.parse(text), null, indent))

// Replays one case's operations, each through the API method of its name, and returns what each
// returns.
const replay = (doc, operations) => operations.map(([name, path, ...rest]) => {
    const values = json => json.map(text => JSON.parse(text))
    switch (name) {
        case 'set':
            return doc.set(path, JSON.parse(rest[0]))
        case 'erase':
            return doc.erase([...path, rest[0]])
        case 'push':
            return doc.push(path, ...values(rest[0]))
        case 'splice': {
            const [start, count, items] = rest
            return doc.splice(path, start, count, ...values(items))
        }
        case 'pop':
            return doc.pop(path)
        case 'shift':
            return doc.shift(path)
        case 'unshift':
            return doc.unshift(path, ...values(rest[0]))
        case 'reverse':
            return doc.reverse(path)
        case 'set_length':
            return doc.setLength(path, rest[0])
        case 'assign_from':
            return doc.assign(path, JSON.parse(rest[0]))
        default:
            throw new Error(`no API for ${name}`)
    }
})

for (const name of names) {
    const kase = load('cases', name)
    const expected = load('expected', name)
    const title = kase.js_only ? `${name} (documented difference: exact decimals)` : name
    test(title, () => {
        const doc = Document.parse(kase.text)
        const returns = replay(doc, kase.operations).map(recorded)
        const text = doc.stringify(kase.indent)
        if (kase.js_only) {
            assert.equal(asJavaScript(Document.parse(kase.text).stringify(kase.indent), kase.indent),
                asJavaScript(kase.text, kase.indent))
            assert.deepEqual(returns.map(value => asJavaScript(value, 0)), expected.returns.map(value => asJavaScript(value, 0)))
            assert.equal(asJavaScript(text, kase.indent), expected.text)
        } else {
            assert.equal(Document.parse(kase.text).stringify(kase.indent), JSON.stringify(JSON.parse(kase.text), null, kase.indent))
            assert.deepEqual(returns, expected.returns)
            assert.equal(text, expected.text)
        }
        doc.free()
    })
}
