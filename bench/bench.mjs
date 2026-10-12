// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The benchmarks of JavaScript's own objects, and the harness that measures them: the twin of
// bench.hpp, boost_json.hpp and document.hpp, which generates the same data, runs the same
// operations and measures them the same way. Node (node.mjs) and Chrome (page.mjs) both run it,
// and both run the C++ compiled to WebAssembly through runWasm.

// The files of nativejson-benchmark the benchmarks read.
export const dataFiles = ['canada.json', 'citm_catalog.json', 'twitter.json']

// The methodology: samples of at least 10 ms, and the median of 10 of them; --quick, which only
// checks the pipeline, takes 3 samples of 1 ms.
const methods = {full: {minSampleNs: 10e6, samples: 10}, quick: {minSampleNs: 1e6, samples: 3}}

// The checksum of every pass, which the run prints at its end: no pass is dead code to V8.
let sink = 0

// Mulberry32, bench.hpp's generator, bit for bit.
const mulberry32 = seed => {
    let a = seed | 0
    return () => {
        a = (a + 0x6D2B79F5) | 0
        let t = Math.imul(a ^ (a >>> 15), 1 | a)
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
        return (t ^ (t >>> 14)) >>> 0
    }
}

const letters = 'abcdefghijklmnopqrstuvwxyz'
const alphanumerics = 'abcdefghijklmnopqrstuvwxyz0123456789'

// A key of length characters, a letter then letters and digits: never an array index.
const makeKey = (rng, length) => {
    let key = letters[rng() % letters.length]
    while (key.length < length) key += alphanumerics[rng() % alphanumerics.length]
    return key
}

// Fisher and Yates' shuffle, from seed, as bench.hpp's.
const shuffle = (v, seed) => {
    const rng = mulberry32(seed)
    for (let i = v.length; i > 1; --i) {
        const j = rng() % i
        ;[v[i - 1], v[j]] = [v[j], v[i - 1]]
    }
}

// The keys deleted: every other one of the lookups' order, at most 1,000 (bench.hpp says why).
const maxDeletes = 1000
const chooseDeletes = s => {
    for (let i = 0; i < s.lookups.length && s.deletes.length < maxDeletes; i += 2) s.deletes.push(s.lookups[i])
    return s
}

// Tessil's synthetic objects: count members whose keys are length characters long.
const synthetic = (count, length) => {
    const seed = count * 100 + length
    const hits = mulberry32(seed)
    const misses = mulberry32(seed + 1)
    const s = {name: `s${count}.k${length}`, keys: [], lookups: [], misses: [], deletes: []}
    for (let i = 0; i < count; ++i) {
        s.keys.push(makeKey(hits, length))
        s.misses.push(makeKey(misses, length))
    }
    s.lookups = s.keys.slice()
    shuffle(s.lookups, seed + 2)
    return chooseDeletes(s)
}

// citm_catalog.json's events: its keys sorted, and nine-digit keys starting with 9 that miss.
const citmEvents = keys => {
    const s = {name: 'citm.events', keys: keys.slice().sort(), lookups: [], misses: [], deletes: []}
    s.lookups = s.keys.slice()
    shuffle(s.lookups, 8)
    const rng = mulberry32(7)
    for (let i = 0; i < s.keys.length; ++i) {
        let key = '9'
        while (key.length < 9) key += String(rng() % 10)
        s.misses.push(key)
    }
    return chooseDeletes(s)
}

// The dictionaries' keys, in bench.cpp's order.
const dictionaries = citm => {
    const out = []
    for (const count of [10, 1000, 100000]) for (const length of [15, 50]) out.push(synthetic(count, length))
    out.push(citmEvents(Object.keys(citm.events)))
    return out
}

// nativejson-benchmark's Statistics, the length of strings and keys in UTF-16 code units.
class Statistics {
    constructor() {
        this.objects = 0
        this.arrays = 0
        this.numbers = 0
        this.strings = 0
        this.trues = 0
        this.falses = 0
        this.nulls = 0
        this.members = 0
        this.elements = 0
        this.length = 0
    }

    sum() {
        return this.objects + this.arrays + this.numbers + this.strings + this.trues + this.falses + this.nulls +
            this.members + this.elements + this.length
    }

    json() {
        return {...this, unit: 'utf16'}
    }
}

// Adds what v holds to s, as nativejson-benchmark's GenStat counts it.
const count = (v, s) => {
    if (v === null) {
        s.nulls++
    } else if (typeof v === 'boolean') {
        if (v) s.trues++
        else s.falses++
    } else if (typeof v === 'number') {
        s.numbers++
    } else if (typeof v === 'string') {
        s.strings++
        s.length += v.length
    } else if (Array.isArray(v)) {
        for (const e of v) count(e, s)
        s.arrays++
        s.elements += v.length
    } else {
        let members = 0
        for (const key in v) {
            s.length += key.length
            count(v[key], s)
            members++
        }
        s.objects++
        s.members += members
        s.strings += members
    }
}

export const statisticsOf = v => {
    const s = new Statistics()
    count(v, s)
    return s
}

// The value of a dictionary's member as the dictionary benchmarks add it: a number, else 1.
const numberOrOne = v => (typeof v === 'number' ? v : 1)

// A 32-bit FNV-1a of an ASCII string, as bench.hpp's fnv.
const fnv = text => {
    let h = 2166136261
    for (let i = 0; i < text.length; ++i) h = Math.imul(h ^ text.charCodeAt(i), 16777619) >>> 0
    return h
}

// The passes of one part of a prepared benchmark: about 65,536 members' worth of copies.
const chunkOf = members => Math.max(1, Math.floor(65536 / Math.max(1, members)))

// A benchmark, as bench.hpp's: chunk Infinity when its passes share one input.
const benchmark = (op, dataset, unit, ops, run, answer, prepare = null, chunk = Infinity) =>
    ({op, dataset, unit, ops, run, answer, prepare, chunk})

// The whole-document benchmarks of one file.
const documents = (out, name, text, v) => {
    let parsed
    out.push(benchmark('parse', name, 'document', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            parsed = JSON.parse(text)
            sum += typeof parsed === 'object' ? 1 : 0
        }
        return sum
    }, () => statisticsOf(parsed).json()))
    let compact
    out.push(benchmark('stringify', name, 'document', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            compact = JSON.stringify(v)
            sum += compact.length
        }
        return sum
    }, () => statisticsOf(JSON.parse(compact)).json()))
    let pretty
    out.push(benchmark('prettify', name, 'document', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            pretty = JSON.stringify(v, null, 4)
            sum += pretty.length
        }
        return sum
    }, () => statisticsOf(JSON.parse(pretty)).json()))
    let statistics
    out.push(benchmark('statistics', name, 'document', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            statistics = statisticsOf(v)
            sum += statistics.sum()
        }
        return sum
    }, () => statistics.json()))
    let cloned
    out.push(benchmark('deep_copy', name, 'document', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            cloned = structuredClone(v)
            sum += typeof cloned === 'object' ? 1 : 0
        }
        return sum
    }, () => statisticsOf(cloned).json()))
    let roundTripped
    out.push(benchmark('deep_copy_json', name, 'document', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            roundTripped = JSON.parse(JSON.stringify(v))
            sum += typeof roundTripped === 'object' ? 1 : 0
        }
        return sum
    }, () => statisticsOf(roundTripped).json()))
    let received
    out.push(benchmark('send_receive', name, 'document', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            received = JSON.parse(JSON.stringify(v))
            sum += typeof received === 'object' ? 1 : 0
        }
        return sum
    }, () => statisticsOf(received).json()))
}

// simdjson's three key-path queries over twitter.json, parse excluded.
const queries = (out, statuses) => {
    let text
    out.push(benchmark('find_tweet', 'twitter', 'query', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            text = ''
            for (const tweet of statuses) {
                // simdjson's tweet, as nativejson-benchmark's twitter.json writes its id (bench.hpp).
                if (tweet.id === 505874901689851900) {
                    text = tweet.text
                    break
                }
            }
            sum += text.length
        }
        return sum
    }, () => ({text})))
    let top
    out.push(benchmark('top_tweet', 'twitter', 'query', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            let best = -1
            let found = null
            for (const tweet of statuses) {
                const retweets = tweet.retweet_count
                if (retweets <= 60 && retweets >= best) {
                    best = retweets
                    found = tweet
                }
            }
            top = {retweet_count: best, screen_name: found.user.screen_name, text: found.text}
            sum += best + top.text.length + top.screen_name.length
        }
        return sum
    }, () => top))
    let ids
    out.push(benchmark('distinct_user_id', 'twitter', 'query', 1, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            ids = []
            for (const tweet of statuses) {
                ids.push(tweet.user.id)
                if ('retweeted_status' in tweet) ids.push(tweet.retweeted_status.user.id)
            }
            ids.sort((a, b) => a - b)
            let w = 0
            for (let r = 0; r < ids.length; ++r) if (r === 0 || ids[r] !== ids[r - 1]) ids[w++] = ids[r]
            ids.length = w
            sum += ids.length
        }
        return sum
    }, () => ({count: ids.length, sum: ids.reduce((s, id) => s + id, 0)})))
}

// Looks every key of the lookups' order up in o, adding the values of those found.
const lookups = (op, ks, o) => {
    let last
    return benchmark(op, ks.name, 'lookup', ks.lookups.length, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            let found = 0
            let total = 0
            for (const key of ks.lookups) {
                const v = o[key]
                if (v !== undefined) {
                    found++
                    total += numberOrOne(v)
                }
            }
            last = {found, sum: total}
            sum += found + total
        }
        return sum
    }, () => last)
}

// The first and the last key of an object, in its order, and its size.
const ends = o => {
    const keys = Object.keys(o)
    return {size: keys.length, first: keys[0], last: keys.at(-1)}
}

// The dictionary benchmarks of one object: lookups, insert, update, delete, iteration.
const dictionary = (out, ks, synthetic, make) => {
    const n = ks.keys.length
    const chunk = chunkOf(synthetic ? n : 2048)
    const base = make()
    out.push(lookups('lookup_hit', ks, base))
    let missed
    out.push(benchmark('lookup_miss', ks.name, 'lookup', n, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            let found = 0
            for (const key of ks.misses) if (key in base) found++
            missed = found
            sum += found + 1
        }
        return sum
    }, () => ({found: missed})))
    // A pass that changes its object gets a copy of its own, made untimed.
    let copies = []
    const prepareCopies = k => {
        copies = Array.from({length: k}, make)
    }
    if (synthetic) {
        out.push(benchmark('insert', ks.name, 'insert', n, k => {
            let sum = 0
            for (let i = 0; i < k; ++i) {
                const o = copies[i]
                for (let j = 0; j < n; ++j) o[ks.keys[j]] = j
                sum += 1
            }
            return sum
        }, () => ends(copies.at(-1)), k => {
            copies = Array.from({length: k}, () => ({}))
        }, chunk))
    }
    out.push(benchmark('update', ks.name, 'update', n, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            const o = copies[i]
            for (let j = 0; j < n; ++j) o[ks.keys[j]] = 2 * j
            sum += 1
        }
        return sum
    }, () => {
        const o = copies.at(-1)
        let total = 0
        for (const key in o) total += numberOrOne(o[key])
        return {size: Object.keys(o).length, sum: total}
    }, prepareCopies, chunk))
    out.push(benchmark('delete', ks.name, 'delete', ks.deletes.length, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            const o = copies[i]
            for (const key of ks.deletes) sum += delete o[key] ? 1 : 0
        }
        return sum
    }, () => ends(copies.at(-1)), prepareCopies, chunk))
    const thinned = make()
    for (const key of ks.deletes) delete thinned[key]
    out.push(lookups('lookup_after_delete', ks, thinned))
    let iterated
    out.push(benchmark('iterate', ks.name, 'member', n, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            let members = 0
            let total = 0
            for (const key in base) {
                members++
                total += key.length + numberOrOne(base[key])
            }
            iterated = {count: members, sum: total}
            sum += members + total
        }
        return sum
    }, () => iterated))
}

// push and pop at the end of an array, 100 of each in a pass.
const pushPop = (dataset, a) => {
    let last
    return benchmark('push_pop', dataset, 'operation', 200, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            for (let j = 0; j < 100; ++j) a.push(j)
            const pushed = a.length
            for (let j = 0; j < 100; ++j) a.pop()
            last = {pushed, popped: a.length}
            sum += pushed + a.length
        }
        return sum
    }, () => last)
}

// The array benchmarks: push and pop, splice, and the array methods.
const arrays = (out, canada, statuses) => {
    const rings = canada.features[0].geometry.coordinates
    let ring = rings[0]
    for (const r of rings) if (r.length > ring.length) ring = r
    out.push(pushPop('twitter.statuses', statuses))
    out.push(pushPop('canada.ring', ring))
    let spliced
    out.push(benchmark('splice', 'canada.ring', 'operation', 2, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            const middle = Math.floor(ring.length / 2)
            ring.splice(middle, 0, 0)
            const inserted = ring.length
            ring.splice(middle, 1)
            spliced = {inserted, removed: ring.length}
            sum += inserted + ring.length
        }
        return sum
    }, () => spliced))
    let mapped
    out.push(benchmark('map', 'canada.ring', 'element', ring.length, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            mapped = ring.map(point => point[0])
            sum += mapped.length
        }
        return sum
    }, () => ({length: mapped.length, negatives: mapped.filter(x => x < 0).length})))
    let kept
    out.push(benchmark('filter', 'canada.ring', 'element', ring.length, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            kept = ring.filter(point => point[0] < -100)
            sum += kept.length
        }
        return sum
    }, () => ({length: kept.length})))
    let reduced
    out.push(benchmark('reduce', 'twitter.statuses', 'element', statuses.length, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            reduced = statuses.reduce((total, tweet) => total + tweet.retweet_count, 0)
            sum += reduced
        }
        return sum
    }, () => ({sum: reduced})))
    const name = statuses.at(-1).user.screen_name
    let found
    out.push(benchmark('find', 'twitter.statuses', 'element', statuses.length, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            found = statuses.find(tweet => tweet.user.screen_name === name)
            sum += found === undefined ? 0 : 1
        }
        return sum
    }, () => (found === undefined
        ? {index: statuses.length, id_str: ''}
        : {index: statuses.indexOf(found), id_str: found.id_str})))
    const from = Math.floor(ring.length / 4)
    const to = Math.floor(ring.length * 3 / 4)
    let part
    out.push(benchmark('slice', 'canada.ring', 'element', to - from, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            part = ring.slice(from, to)
            sum += part.length
        }
        return sum
    }, () => ({length: part.length})))
    let joined
    out.push(benchmark('join', 'twitter.statuses', 'element', statuses.length, k => {
        let sum = 0
        for (let i = 0; i < k; ++i) {
            joined = statuses.map(tweet => tweet.id_str).join(',')
            sum += joined.length
        }
        return sum
    }, () => ({length: joined.length, fnv: fnv(joined)})))
}

// Every benchmark of JavaScript's objects over the texts, in bench.cpp's order.
const benchmarks = texts => {
    const canada = JSON.parse(texts.canada)
    const citm = JSON.parse(texts.citm)
    const twitter = JSON.parse(texts.twitter)
    const out = []
    documents(out, 'canada', texts.canada, canada)
    documents(out, 'citm', texts.citm, citm)
    documents(out, 'twitter', texts.twitter, twitter)
    queries(out, twitter.statuses)
    const events = JSON.stringify(citm.events)
    for (const ks of dictionaries(citm)) {
        if (ks.name === 'citm.events') {
            dictionary(out, ks, false, () => JSON.parse(events))
        } else {
            dictionary(out, ks, true, () => {
                const o = {}
                for (let j = 0; j < ks.keys.length; ++j) o[ks.keys[j]] = j
                return o
            })
        }
    }
    arrays(out, canada, twitter.statuses)
    return out
}

// Runs k passes of b, in parts of at most b.chunk passes, each prepared untimed; nanoseconds. A
// benchmark whose run returns a promise, a Worker's, is awaited; a synchronous one is not, so that
// its time has no await in it.
const timed = async (b, k, now) => {
    let total = 0
    for (let done = 0; done < k;) {
        const part = Math.min(k - done, b.chunk)
        if (b.prepare) b.prepare(part)
        const start = now()
        const passes = b.run(part)
        const checksum = passes instanceof Promise ? await passes : passes
        sink = (sink + checksum) % 4294967296
        total += (now() - start) * 1e6
        done += part
    }
    return total
}

// Measures b as bench.hpp's measure does, with gc() before each sample.
const measure = async (b, method, now, gc) => {
    gc()
    const firstNs = (await timed(b, 1, now)) / b.ops
    const answer = await b.answer()
    let k = 1
    // Ten times the passes while a sample is too short for the clock to read it well (Chrome's reads
    // 5 us, and may read 0), then as bench.hpp's measure.
    for (let t = await timed(b, k, now); t < method.minSampleNs; gc(), t = await timed(b, k, now)) {
        k = t < method.minSampleNs / 100 ? k * 10
            : Math.max(k * 2, Math.floor(method.minSampleNs * 1.2 / Math.max(t, 1) * k))
    }
    gc()
    await timed(b, k, now)
    const samples = []
    for (let i = 0; i < method.samples; ++i) {
        gc()
        samples.push((await timed(b, k, now)) / k / b.ops)
    }
    samples.sort((x, y) => x - y)
    const median = (samples[(method.samples - 1) >> 1] + samples[method.samples >> 1]) / 2
    return {first_ns: firstNs, median_ns: median, batch: k, samples: method.samples, answer}
}

// A request to a Worker, which fails when the Worker reports an error: the reply's error, if any.
const call = async (ask, message, transfer = []) => {
    const reply = await ask(message, transfer)
    if (reply.error) throw new Error(reply.error)
    return reply
}

// The Worker benchmarks: a document passed to a Worker, either as the JavaScript object (which
// postMessage clones) or as its image (a transferred ArrayBuffer, which the Worker's own module opens
// in place). Each pass waits for the Worker's reply, so its time is the whole send: the clone or the
// copy, the transfer, and the receive. Each row answers with the Statistics the Worker holds after
// its last pass, which the run checks against the other implementations' answers. spawn() makes a
// Worker (worker.mjs) that asks answers one request at a time; factory and files are bench-module.js
// and the data, which the sending side's module reads.
export const workerBenchmarks = async (texts, {spawn, factory, files, moduleUrl}) => {
    const objects = {
        canada: JSON.parse(texts.canada),
        citm: JSON.parse(texts.citm),
        twitter: JSON.parse(texts.twitter),
    }
    const sender = await factory({noInitialRun: true, print: () => {}, printErr: () => {}})
    sender.FS.mkdir('/data')
    for (const [name, bytes] of Object.entries(files)) sender.FS.writeFile(`/data/${name}`, bytes)
    const worker = spawn()
    await call(worker.ask, {op: 'init', moduleUrl})
    // The document the sending module holds, and its image's size: sent when a row first needs it.
    let sent = {file: -1, size: 0}
    const send = file => {
        if (sent.file === file) return
        const size = sender._webcpp_bench_send(file)
        if (size === 0) throw new Error(`the sending module cannot parse file ${file}`)
        sent = {file, size}
    }
    const benchmarks = []
    ;['canada', 'citm', 'twitter'].forEach((dataset, file) => {
        const value = objects[dataset]
        benchmarks.push({
            ...benchmark('worker_clone', dataset, 'document', 1, async k => {
                let sum = 0
                for (let j = 0; j < k; ++j) {
                    await call(worker.ask, {op: 'clone', value})
                    sum += 1
                }
                return sum
            }, async () => (await call(worker.ask, {op: 'answer', which: 'clone'})).answer),
            impl: 'javascript',
        })
        benchmarks.push({
            ...benchmark('worker_transfer', dataset, 'document', 1, async k => {
                let sum = 0
                for (let j = 0; j < k; ++j) {
                    // The image is copied out of the module's memory into a buffer of its own, which
                    // the transfer hands over; the Worker copies it into its memory and opens it.
                    const at = sender._webcpp_bench_image()
                    const copy = sender.HEAPU8.slice(at, at + sent.size)
                    await call(worker.ask, {op: 'transfer', size: sent.size, bytes: copy.buffer}, [copy.buffer])
                    sum += 1
                }
                return sum
            }, async () => (await call(worker.ask, {op: 'answer', which: 'transfer'})).answer,
            () => send(file)),
            impl: 'document',
        })
    })
    return {benchmarks, close: () => worker.close()}
}

// Measures every benchmark of JavaScript's objects, one JSON line each through print, after a line
// of meta; gc is the engine's, which --expose-gc gives. With workers (spawn, factory, files and
// moduleUrl, as workerBenchmarks takes them), the Worker benchmarks follow.
export const runJavaScript = async ({texts, gc, now, print, meta, quick = false, workers = null}) => {
    if (typeof gc !== 'function') throw new Error('gc() is missing: run with --expose-gc')
    const method = quick ? methods.quick : methods.full
    print(JSON.stringify({meta}))
    const pool = workers ? await workerBenchmarks(texts, workers) : {benchmarks: [], close: () => {}}
    try {
        for (const b of [...benchmarks(texts), ...pool.benchmarks]) {
            const m = await measure(b, method, now, gc)
            print(JSON.stringify({impl: b.impl ?? 'javascript', op: b.op, dataset: b.dataset, unit: b.unit, ops: b.ops, ...m}))
        }
    } finally {
        pool.close()
    }
    print(JSON.stringify({checksum: sink}))
}

// Runs bench.cpp compiled by Emscripten (bench-module.js's factory) over the files, given as bytes
// by name, which it writes into the module's own file system; its lines go to print. Resolves to
// main's exit status.
export const runWasm = async ({factory, files, impl, print, quick = false}) => {
    const errors = []
    const module = await factory({print, printErr: line => errors.push(line), noInitialRun: true})
    module.FS.mkdir('/data')
    for (const [name, bytes] of Object.entries(files)) module.FS.writeFile(`/data/${name}`, bytes)
    const status = module.callMain(['--data', '/data', '--impl', impl, ...(quick ? ['--quick'] : [])])
    if (status !== 0) throw new Error(`bench-module exited with ${status}: ${errors.join('\n')}`)
    return status
}
