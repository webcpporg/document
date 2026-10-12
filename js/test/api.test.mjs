// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The handles of Document: reading, writing, the errors that are JavaScript's own, numbers, the
// key index and the copy that toJSON makes.

import assert from 'node:assert/strict'
import { describe, test } from 'node:test'
import { Document } from '../webcpp-document.mjs'

describe('parse', () => {
    test('throws SyntaxError where JSON.parse does', () => {
        for (const text of ['{', '[1,]', '{"a":1,}', '', 'nul', '{"a" 1}', '01']) {
            assert.throws(() => Document.parse(text), SyntaxError, text)
            assert.throws(() => JSON.parse(text), SyntaxError, text)
        }
    })

    test('reads what JSON.parse reads, and toJSON copies it out', () => {
        const text = '{"user":{"name":"Ada","langs":["C","Note G"]},"ok":true,"none":null}'
        const doc = Document.parse(text)
        assert.deepEqual(doc.toJSON(), JSON.parse(text))
        assert.equal(doc.stringify(), JSON.stringify(JSON.parse(text)))
        assert.equal(doc.stringify(2), JSON.stringify(JSON.parse(text), null, 2))
        assert.equal(doc.stringify(11), JSON.stringify(JSON.parse(text), null, 11))
        doc.free()
    })
})

describe('get and set', () => {
    test('get reads scalars as JavaScript values and containers as copies', () => {
        const doc = Document.parse('{"user":{"name":"Ada","age":36,"langs":["C"]},"n":null,"t":false}')
        assert.equal(doc.get(['user', 'name']), 'Ada')
        assert.equal(doc.get(['user', 'age']), 36)
        assert.deepEqual(doc.get(['user', 'langs']), ['C'])
        assert.equal(doc.get(['n']), null)
        assert.equal(doc.get(['t']), false)
        assert.equal(doc.get(['missing', 'deeper']), undefined)
        assert.equal(doc.get(['user', 'langs', 0]), 'C')
        doc.free()
    })

    test('set writes what JSON.stringify writes for the value', () => {
        const doc = Document.parse('{"user":{"name":"Ada"},"langs":[]}')
        doc.set(['user', 'age'], 30)
        doc.set(['user', 'tags'], { a: [1, 'two', null] })
        doc.set(['pi'], 3.14159)
        doc.set(['big'], 1e21)
        doc.set(['zero'], -0)
        assert.equal(doc.get(['user', 'age']), 30)
        assert.equal(doc.stringify(), JSON.stringify({
            user: { name: 'Ada', age: 30, tags: { a: [1, 'two', null] } },
            langs: [], pi: 3.14159, big: 1e21, zero: 0,
        }))
        doc.set([], [true])
        assert.equal(doc.stringify(), '[true]')
        doc.free()
    })

    test('set stores undefined as JSON.stringify drops it', () => {
        const doc = Document.parse('{"a":1,"b":2}')
        doc.set(['b'], undefined)
        assert.equal(doc.stringify(), '{"a":1}')
        doc.free()
    })

    test('erase removes a member and says whether it was there', () => {
        const doc = Document.parse('{"user":{"name":"Ada","age":30}}')
        assert.equal(doc.erase(['user', 'age']), true)
        assert.equal(doc.erase(['user', 'age']), false)
        assert.equal(doc.stringify(), '{"user":{"name":"Ada"}}')
        doc.free()
    })
})

describe('arrays', () => {
    test('push returns the new length and splice returns what it removed', () => {
        const doc = Document.parse('{"langs":["C","Rust","Go"]}')
        assert.equal(doc.push(['langs'], 'Note G'), 4)
        assert.equal(doc.length(['langs']), 4)
        assert.deepEqual(doc.splice(['langs'], 0, 1), ['C'])
        assert.deepEqual(doc.splice(['langs'], -1, 1, 'JS', 'Lisp'), ['Note G'])
        assert.equal(doc.stringify(), JSON.stringify({ langs: ['Rust', 'Go', 'JS', 'Lisp'] }))
        doc.free()
    })

    test('splice with negative and clamped arguments as Array.prototype.splice', () => {
        const values = [1, 2, 3, 4, 5]
        const doc = Document.parse(JSON.stringify(values))
        const expected = [...values]
        assert.deepEqual(doc.splice([], -2, 10), expected.splice(-2, 10))
        assert.equal(doc.stringify(), JSON.stringify(expected))
        doc.free()
    })
})

describe('numbers', () => {
    test('getDecimal gives the exact decimal, get the JavaScript Number of it', () => {
        const text = '0.1234567890123456789012345678901234'
        const doc = Document.parse(`{"price":${text},"small":1.50}`)
        assert.equal(doc.getDecimal(['price']), text)
        assert.equal(doc.get(['price']), Number(text))
        assert.equal(doc.getDecimal(['small']), '1.5')
        assert.equal(doc.get(['small']), 1.5)
        doc.free()
    })
})

describe('keysInRange', () => {
    test('gives the keys of a range in key order', () => {
        const doc = Document.parse('{"users":{"user_2":{},"user_10":{},"admin":{},"user_1":{},' +
            '"user_":{},"user`":{}}}')
        assert.deepEqual(doc.keysInRange(['users'], 'user_', 'user_\uffff'),
            ['user_', 'user_1', 'user_10', 'user_2'])
        assert.deepEqual(doc.keysInRange(['users'], 'x', 'y'), [])
        doc.free()
    })
})

describe('errors', () => {
    test('what JavaScript throws as TypeError', () => {
        const doc = Document.parse('{"obj":{"a":1},"arr":[1,2],"num":5,"str":"s"}')
        assert.throws(() => doc.set(['missing', 'x'], 1), TypeError)
        assert.throws(() => doc.push(['obj'], 1), TypeError)
        assert.throws(() => doc.erase(['arr', 0]), TypeError)
        assert.throws(() => doc.splice(['num'], 0, 1), TypeError)
        assert.throws(() => doc.getDecimal(['str']), TypeError)
        assert.throws(() => doc.keysInRange(['arr'], 'a', 'b'), TypeError)
        assert.throws(() => doc.set(['f'], () => 1), TypeError)
        assert.throws(() => doc.set(['f'], Symbol('x')), TypeError)
        assert.throws(() => doc.set(['f'], 1n), TypeError)
        assert.throws(() => doc.get(['arr', 1.5]), TypeError)
        assert.throws(() => doc.get(['arr', -1]), TypeError)
        assert.throws(() => doc.get('arr'), TypeError)
        doc.free()
    })

    test('a freed document refuses every operation with TypeError', () => {
        const doc = Document.parse('[1]')
        doc.free()
        assert.throws(() => doc.get([]), TypeError)
        assert.throws(() => doc.stringify(), TypeError)
        assert.throws(() => doc.image(), TypeError)
        doc.free()
    })
})

describe('size and seal', () => {
    test('byteLength is the image, and seal makes it checkable', () => {
        const doc = Document.parse('{"a":[1,2,3]}')
        assert.equal(doc.byteLength, doc.image().byteLength)
        assert.ok(doc.byteLength > 0)
        assert.equal(doc.seal(), undefined)
        const image = doc.image().slice()
        assert.equal(Document.open(image).stringify(), '{"a":[1,2,3]}')
        image[image.length - 1] ^= 0xff
        assert.throws(() => Document.open(image), TypeError)
        doc.free()
    })
})

describe('reserve and capacity', () => {
    test('reserve makes room for a size, and capacity reports the image size', () => {
        const doc = Document.parse('{"a":1}')
        const before = doc.capacity
        assert.ok(before >= doc.byteLength)
        assert.equal(doc.reserve(before * 4), undefined)
        assert.ok(doc.capacity >= before * 4)
        assert.equal(doc.stringify(), '{"a":1}')
        doc.free()
    })

    test('a reservation past the limit is RangeError, and a bad size is RangeError too', () => {
        const doc = Document.parse('[1]')
        assert.throws(() => doc.reserve(2 ** 40), RangeError)
        assert.throws(() => doc.reserve(-1), RangeError)
        assert.throws(() => doc.reserve(1.5), RangeError)
        doc.free()
    })
})

describe('array methods', () => {
    test('pop and shift give the removed element, undefined from an empty array', () => {
        const doc = Document.parse('{"a":[1,"two",{"x":null}],"e":[]}')
        assert.deepEqual(doc.pop(['a']), { x: null })
        assert.equal(doc.shift(['a']), 1)
        assert.equal(doc.pop(['e']), undefined)
        assert.equal(doc.shift(['e']), undefined)
        assert.equal(doc.stringify(), '{"a":["two"],"e":[]}')
        doc.free()
    })

    test('unshift puts its values in front, in order, and gives the length', () => {
        const doc = Document.parse('{"a":[3]}')
        assert.equal(doc.unshift(['a'], 1, 2), 3)
        assert.equal(doc.stringify(), '{"a":[1,2,3]}')
        assert.equal(doc.unshift(['a']), 3)
        doc.free()
    })

    test('reverse reverses in place, setLength truncates and extends with null', () => {
        const doc = Document.parse('{"a":[1,2,3]}')
        assert.equal(doc.reverse(['a']), undefined)
        assert.equal(doc.stringify(), '{"a":[3,2,1]}')
        doc.setLength(['a'], 2)
        assert.equal(doc.stringify(), '{"a":[3,2]}')
        doc.setLength(['a'], 4)
        assert.equal(doc.stringify(), '{"a":[3,2,null,null]}')
        doc.free()
    })

    test('setLength refuses what is no array length as RangeError', () => {
        const doc = Document.parse('[1]')
        assert.throws(() => doc.setLength([], -1), RangeError)
        assert.throws(() => doc.setLength([], 1.5), RangeError)
        assert.throws(() => doc.setLength([], 'x'), RangeError)
        doc.free()
    })

    test('assign copies the members of an object in, as Object.assign does', () => {
        const doc = Document.parse('{"user":{"name":"Ada","age":1}}')
        assert.equal(doc.assign(['user'], { age: 36, langs: ['C'] }), undefined)
        assert.equal(doc.stringify(), '{"user":{"name":"Ada","age":36,"langs":["C"]}}')
        doc.assign(['user'], null)
        doc.assign(['user'], undefined)
        assert.equal(doc.stringify(), '{"user":{"name":"Ada","age":36,"langs":["C"]}}')
        assert.throws(() => doc.assign(['user'], 5), TypeError)
        assert.throws(() => doc.assign(['user', 'name'], {}), TypeError)
        doc.free()
    })

    test('the array methods refuse other kinds as TypeError', () => {
        const doc = Document.parse('{"obj":{"a":1},"arr":[1]}')
        assert.throws(() => doc.pop(['obj']), TypeError)
        assert.throws(() => doc.shift(['obj']), TypeError)
        assert.throws(() => doc.unshift(['obj'], 1), TypeError)
        assert.throws(() => doc.reverse(['obj']), TypeError)
        assert.throws(() => doc.setLength(['obj'], 0), TypeError)
        assert.throws(() => doc.assign(['arr'], {}), TypeError)
        doc.free()
    })
})

describe('numbers keep their digits', () => {
    test('stringify writes the digits a double does not keep, where JSON.stringify rounds them', () => {
        const doc = Document.parse('{"n":12345678901234567890}')
        assert.equal(doc.stringify(), '{"n":12345678901234567890}')
        assert.equal(JSON.stringify(JSON.parse('{"n":12345678901234567890}')), '{"n":12345678901234567000}')
        doc.free()
    })

    test('stringify writes 1e+400 where JSON.stringify writes null', () => {
        const doc = Document.parse('[1e400]')
        assert.equal(doc.stringify(), '[1e+400]')
        assert.equal(JSON.stringify(JSON.parse('[1e400]')), '[null]')
        doc.free()
    })
})
