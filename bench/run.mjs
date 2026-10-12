// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Measures every implementation of document's benchmarks on this machine, checks that all of them
// answered alike, and writes the results page, doc/benchmark-results.adoc:
//
//   node run.mjs --native <bench-native> --module <bench-module.js> --data <dir> --out <file> [--quick]
//
// `b2 libs/document/bench//results` runs it with the programs it built in the release variant.
// Each implementation runs in a process, or a page, of its own, one after the other:
//
// | where   | what                                                                   |
// | native  | bench-native --impl boost.json, document, document.js_traits           |
// | Node    | node.mjs wasm --impl (the same three); node.mjs js, with its Workers   |
// | Chrome  | bench.html, through Playwright's headless Chromium: the same four       |
//
// Node and Chrome run with --expose-gc. Chrome loads the page from a server of this process on
// 127.0.0.1, which sends COOP and COEP so that the page is cross-origin isolated and
// performance.now reads 5 us. A disagreement of any two answers fails the run, naming them, and
// writes nothing. --quick measures with 3 samples of 1 ms, to check the pipeline only; --places runs
// only the places it names, checks their answers and writes nothing.

import {spawn, execFileSync} from 'node:child_process'
import {existsSync, readFileSync, renameSync, writeFileSync} from 'node:fs'
import {createServer} from 'node:http'
import os from 'node:os'
import {dirname, extname, join, normalize, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const args = process.argv.slice(2)
const option = name => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined)
const native = option('--native')
const module = option('--module')
const data = option('--data')
const out = option('--out')
const quick = args.includes('--quick')
const places = (option('--places') ?? 'native,node,chrome').split(',')
if (!native || !module || !data || !out || places.some(p => !['native', 'node', 'chrome'].includes(p))) {
    throw new Error('usage: node run.mjs --native <bench-native> --module <bench-module.js> --data <dir> --out <file> ' +
        '[--quick] [--places native,node,chrome]')
}
const implementations = ['boost.json', 'document', 'document.js_traits']
const say = text => process.stderr.write(`${text}\n`)

// ---- running one implementation -------------------------------------------------------------------

// What a run printed: its meta line, and its results, each named by where it ran.
const collect = (where, lines) => {
    const run = {meta: null, results: []}
    for (const line of lines) {
        if (!line.trim()) continue
        const parsed = JSON.parse(line)
        if (parsed.meta) run.meta = parsed.meta
        else if (parsed.op) run.results.push({...parsed, where})
    }
    return run
}

// Runs a program to its end, echoing its progress; resolves to its lines.
const runProcess = (command, words, where) => new Promise((done, fail) => {
    const child = spawn(command, words, {stdio: ['ignore', 'pipe', 'pipe']})
    const lines = []
    let pending = ''
    let errors = ''
    child.stdout.on('data', chunk => {
        pending += chunk
        const parts = pending.split('\n')
        pending = parts.pop()
        for (const line of parts) {
            lines.push(line)
            progress(where, line)
        }
    })
    child.stderr.on('data', chunk => (errors = (errors + chunk).slice(-4000)))
    child.on('error', fail)
    child.on('exit', (code, signal) => {
        if (pending) lines.push(pending)
        if (code === 0) done(lines)
        else fail(new Error(`${where}: ${command} ${words.join(' ')} ended with ${code ?? signal}:\n${errors}`))
    })
})

const progress = (where, line) => {
    try {
        const r = JSON.parse(line)
        if (r.op) say(`  ${where} ${r.impl} ${r.op} ${r.dataset}: ${time(r.median_ns)} (first ${time(r.first_ns)})`)
    } catch {
        // A line that is not JSON is reported by collect.
    }
}

// ---- Chrome ------------------------------------------------------------------------------------------

const types = {'.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json'}

// Serves the page, its modules, the Emscripten module and the data, cross-origin isolated.
const serve = () => new Promise(listening => {
    const roots = [['/build/', dirname(resolve(module))], ['/data/', resolve(data)], ['/', here]]
    const pageFiles = new Set(['/bench.html', '/page.mjs', '/bench.mjs', '/worker.mjs'])
    const server = createServer((request, response) => {
        const path = decodeURIComponent(new URL(request.url, 'http://localhost').pathname)
        const [prefix, root] = roots.find(([p]) => path.startsWith(p))
        const file = normalize(join(root, path.slice(prefix.length)))
        if ((prefix === '/' && !pageFiles.has(path)) || !file.startsWith(root) || !existsSync(file)) {
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
    server.listen(0, '127.0.0.1', () => listening(server))
})

// Runs the four implementations in headless Chromium, each in a page of its own.
// Playwright's own headless shell, of the Chromium this Playwright pins, launched with gc() exposed;
// installed first when it is missing. Playwright 1.59 under Node 26 may download it and then never
// finish extracting it (measured), so the install is bounded and its failure names what to do.
const launch = async chromium => {
    const options = {args: ['--js-flags=--expose-gc']}
    try {
        return await chromium.launch(options)
    } catch (error) {
        if (!/Executable doesn't exist/.test(error.message)) throw error
    }
    say('installing Playwright\'s chromium-headless-shell')
    const cli = join(here, 'node_modules/playwright/cli.js')
    try {
        execFileSync(process.execPath, [cli, 'install', 'chromium-headless-shell'],
            {stdio: ['ignore', 'inherit', 'inherit'], timeout: 15 * 60 * 1000})
    } catch (error) {
        throw new Error(`\`node ${cli} install chromium-headless-shell\` failed (${error.message}); when it ` +
            'downloads but never extracts, unzip the archive it downloaded into the directory it names, beside an ' +
            'empty file INSTALLATION_COMPLETE')
    }
    return chromium.launch(options)
}

const runChrome = async () => {
    const {chromium} = await import('playwright')
    const browser = await launch(chromium)
    const server = await serve()
    const base = `http://127.0.0.1:${server.address().port}`
    try {
        const session = await browser.newBrowserCDPSession()
        const version = await session.send('Browser.getVersion')
        const runs = []
        for (const [what, impl] of [...implementations.map(i => ['wasm', i]), ['js', 'javascript']]) {
            const where = 'chrome'
            const context = await browser.newContext()
            const page = await context.newPage()
            const problems = []
            page.on('pageerror', error => problems.push(error.message))
            page.on('console', message => {
                if (message.type() === 'log') progress(where, message.text())
                else if (message.type() === 'error') problems.push(message.text())
            })
            await page.goto(`${base}/bench.html`)
            let lines
            try {
                lines = await page.evaluate(options => window.runBench(options), {what, impl, quick})
            } catch (error) {
                throw new Error(`chrome: ${impl}: ${error.message}\n${problems.join('\n')}`)
            }
            await context.close()
            const run = collect(where, lines)
            run.meta = {...run.meta, chrome: version.product, v8: version.jsVersion, browser: browser.version()}
            runs.push(run)
        }
        return runs
    } finally {
        await browser.close()
        server.close()
    }
}

// ---- checking -----------------------------------------------------------------------------------------

// JSON with its keys sorted, so that two answers compare whatever their keys' order.
const canonical = v => JSON.stringify(v, (_, x) => (x && typeof x === 'object' && !Array.isArray(x)
    ? Object.fromEntries(Object.entries(x).sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)))
    : x))

// Whether two answers agree, as bench.hpp's agree: a Statistics' length counts UTF-8 bytes or UTF-16
// code units, and two of different units are compared without it.
const agree = (a, b) => {
    if (a && b && typeof a === 'object' && typeof b === 'object' && a.unit && b.unit && a.unit !== b.unit) {
        const strip = ({unit, length, ...rest}) => rest
        return canonical(strip(a)) === canonical(strip(b))
    }
    return canonical(a) === canonical(b)
}

const key = r => `${r.op} ${r.dataset}`

// Every disagreement and every gap: each row's answers alike, and each implementation measured on
// every row it has, in every place it ran.
const check = results => {
    const faults = []
    const rows = new Map()
    for (const r of results) {
        if (!rows.has(key(r))) rows.set(key(r), [])
        rows.get(key(r)).push(r)
    }
    for (const [row, given] of rows) {
        for (const r of given) {
            if (!agree(r.answer, given[0].answer)) {
                faults.push(`${row}: ${r.where} ${r.impl} answers ${canonical(r.answer)}, ` +
                    `${given[0].where} ${given[0].impl} ${canonical(given[0].answer)}`)
            }
        }
    }
    // The Worker rows exist only in Node and Chrome: they are compared between those two places alone.
    const isWorker = r => r.op.startsWith('worker_')
    const keysOf = (impl, where) => new Set(results.filter(r => r.impl === impl && r.where === where && !isWorker(r)).map(key))
    const workerKeysOf = (impl, where) => new Set(results.filter(r => r.impl === impl && r.where === where && isWorker(r)).map(key))
    const same = (a, b) => a.size === b.size && [...a].every(k => b.has(k))
    for (const impl of implementations) {
        for (const where of ['node', 'chrome']) {
            if (!same(keysOf(impl, 'native'), keysOf(impl, where))) faults.push(`${impl} in ${where} measured other rows than natively`)
        }
    }
    if (!same(keysOf('javascript', 'node'), keysOf('javascript', 'chrome'))) faults.push('javascript measured other rows in Node and Chrome')
    for (const impl of ['javascript', 'document']) {
        if (!same(workerKeysOf(impl, 'node'), workerKeysOf(impl, 'chrome'))) faults.push(`${impl} measured other Worker rows in Node and Chrome`)
    }
    if (!same(keysOf('boost.json', 'native'), keysOf('document', 'native'))) faults.push('boost.json and document measured other rows')
    const js = keysOf('javascript', 'node')
    for (const k of keysOf('document', 'native')) if (!js.has(k)) faults.push(`javascript has no ${k}`)
    return faults
}

// ---- the page --------------------------------------------------------------------------------------------

// Three significant digits, in the unit that keeps them readable.
const significant = x => (x >= 100 ? x.toFixed(0) : x >= 10 ? x.toFixed(1) : x.toFixed(2))
function time(ns) {
    if (ns < 1e3) return `${significant(ns)} ns`
    if (ns < 1e6) return `${significant(ns / 1e3)} µs`
    if (ns < 1e9) return `${significant(ns / 1e6)} ms`
    return `${significant(ns / 1e9)} s`
}
const thousands = n => n.toLocaleString('en-US')

// The columns: where it ran, and which implementation.
const columns = [
    ['native', 'boost', 'Boost.JSON native'],
    ['native', 'document', 'document native'],
    ['node', 'boost', 'Boost.JSON wasm, Node'],
    ['node', 'document', 'document wasm, Node'],
    ['node', 'js', 'JavaScript, Node'],
    ['chrome', 'boost', 'Boost.JSON wasm, Chrome'],
    ['chrome', 'document', 'document wasm, Chrome'],
    ['chrome', 'js', 'JavaScript, Chrome'],
]

// The datasets as a reader knows them; sizes names the lengths of the two arrays.
const datasetLabel = (dataset, sizes) => {
    const files = {canada: 'canada.json', citm: 'citm_catalog.json', twitter: 'twitter.json'}
    if (files[dataset]) return files[dataset]
    if (dataset === 'citm.events') return `citm_catalog.json \`events\` (${sizes.events} keys)`
    if (dataset === 'canada.ring') return `canada.json's largest ring (${thousands(sizes.ring)} points)`
    if (dataset === 'twitter.statuses') return `twitter.json \`statuses\` (${sizes.statuses})`
    const synthetic = /^s([0-9]+)\.k([0-9]+)$/.exec(dataset)
    if (synthetic) return `${thousands(Number(synthetic[1]))} members, ${synthetic[2]}-character keys`
    return dataset
}

// The sections of the page, in the research's order (section 5 of the benchmark suites' survey),
// each with its sources, what one operation is, and the implementations whose form is the closest
// one rather than the operation itself, whose cells carry a *.
const documentRows = (op, jsTraits) => ['canada', 'citm', 'twitter'].flatMap(d => [
    {op, dataset: d},
    ...(jsTraits ? [{op, dataset: d, variant: 'js_traits'}] : []),
])
const dictionaryRows = (ops, synthetic = true) => ['s10.k15', 's10.k50', 's1000.k15', 's1000.k50',
    's100000.k15', 's100000.k50', 'citm.events']
    .filter(d => synthetic || d === 'citm.events')
    .flatMap(d => ops.map(op => ({op, dataset: d})))
const sections = [
    // Binary transmission comes first: the case the image is built for, where no suite gives a form.
    {id: 'send-receive', title: 'Send and receive a document', sources: 'none of the surveyed suites: binary transmission, which document is built for',
        per: 'per document', rows: [...documentRows('send_receive'), ...documentRows('send_receive_sealed')]},
    {id: 'worker', title: 'Pass a document to a Worker', sources: 'none of the surveyed suites: the Worker of a web application',
        per: 'per document, each pass answered by the Worker', rows: [...documentRows('worker_clone'), ...documentRows('worker_transfer')]},
    {id: 'parse', title: '1. Parse', sources: 'nativejson-benchmark, Boost.JSON bench, serde json-benchmark, nlohmann, JetStream 2.1',
        per: 'per document', rows: documentRows('parse', true)},
    {id: 'stringify', title: '2. Stringify, compact', sources: 'nativejson-benchmark, Boost.JSON bench, nlohmann, simdjson, JetStream 2.1',
        per: 'per document', rows: documentRows('stringify', true)},
    {id: 'prettify', title: '3. Prettify, indent 4', sources: 'nativejson-benchmark, nlohmann, simdjson',
        per: 'per document', rows: documentRows('prettify'), marked: ['boost']},
    {id: 'statistics', title: '4. Full traversal (Statistics)', sources: 'nativejson-benchmark',
        per: 'per document', rows: documentRows('statistics')},
    {id: 'queries', title: '5. Key-path queries', sources: 'simdjson (find_tweet, top_tweet, distinct_user_id)',
        per: 'per query, parse excluded', rows: ['find_tweet', 'top_tweet', 'distinct_user_id'].map(op => ({op, dataset: 'twitter'}))},
    {id: 'lookup', title: '6. Lookup, hit and miss', sources: 'Tessil, martinus, V8 PropertyQueries, abseil',
        per: 'per lookup', rows: dictionaryRows(['lookup_hit', 'lookup_miss'])},
    {id: 'insert', title: '7. Insert a member', sources: 'Tessil, abseil TryEmplace, V8 Map-Set',
        per: 'per insertion', rows: dictionaryRows(['insert']).filter(r => r.dataset !== 'citm.events')},
    {id: 'update', title: '8. Update a member', sources: 'abseil InsertOrAssign, martinus',
        per: 'per assignment', rows: dictionaryRows(['update'])},
    {id: 'delete', title: '9. Delete a member, then lookups', sources: 'Tessil, martinus, V8 Map-Delete, Dromaeo removeChild',
        per: 'per deletion, and per lookup after them', rows: dictionaryRows(['delete', 'lookup_after_delete']), marked: ['boost']},
    {id: 'iterate', title: '10. Iterate entries', sources: 'Tessil, martinus, abseil, V8 Keys and Object, Dromaeo traverse',
        per: 'per member', rows: dictionaryRows(['iterate'])},
    {id: 'push-pop', title: '11. Array push and pop', sources: 'Dromaeo appendChild and removeChild, martinus',
        per: 'per push or pop', rows: ['twitter.statuses', 'canada.ring'].map(dataset => ({op: 'push_pop', dataset}))},
    {id: 'splice', title: '12. Array splice in the middle', sources: 'Dromaeo insertBefore and removeChild',
        per: 'per splice', rows: [{op: 'splice', dataset: 'canada.ring'}], marked: ['boost']},
    {id: 'array-methods', title: '13. Array methods', sources: 'V8 js-perf-test Array',
        per: 'per element', rows: [['map', 'canada.ring'], ['filter', 'canada.ring'], ['reduce', 'twitter.statuses'],
            ['find', 'twitter.statuses'], ['slice', 'canada.ring'], ['join', 'twitter.statuses']].map(([op, dataset]) => ({op, dataset})),
        marked: ['boost', 'document'], markedOps: {slice: ['boost'], join: ['boost', 'document', 'js']}},
    {id: 'deep-copy', title: '14. Deep copy', sources: 'martinus Copy, abseil CopyCtor, Dromaeo cloneNode',
        per: 'per document', rows: [...documentRows('deep_copy'), ...documentRows('deep_copy_json')]},
]

// The implementation a column reads for a row; none where it has no such row.
const implOf = (family, row) => {
    if (row.op === 'deep_copy_json') return family === 'js' ? 'javascript' : null
    if (row.variant === 'js_traits') return family === 'document' ? 'document.js_traits' : null
    return {boost: 'boost.json', document: 'document', js: 'javascript'}[family]
}

// A row's label: its dataset, and what the row measures when the section has several kinds.
const rowLabel = (row, sizes, ops) => {
    const label = datasetLabel(row.dataset, sizes)
    if (row.variant === 'js_traits') return `${label}, \`js_traits\``
    switch (row.op) {
        case 'deep_copy': return `${label}, \`structuredClone\` in JavaScript`
        case 'deep_copy_json': return `${label}, \`JSON.parse(JSON.stringify(v))\``
        case 'send_receive': return `${label}, sent and received`
        case 'send_receive_sealed': return `${label}, sent sealed and received`
        case 'worker_clone': return `${label}, posted to a Worker`
        case 'worker_transfer': return `${label}, image transferred to a Worker`
        case 'lookup_hit': return `${label}, hit`
        case 'lookup_miss': return `${label}, miss`
        case 'delete': return `${label}, ${thousands(ops)} deletions`
        case 'lookup_after_delete': return `${label}, lookups after them`
        case 'find_tweet': case 'top_tweet': case 'distinct_user_id':
        case 'map': case 'filter': case 'reduce': case 'find': case 'slice': case 'join':
            return `\`${row.op}\`, ${label}`
        default: return label
    }
}

const table = (section, find, sizes) => {
    const lines = []
    lines.push('[cols="3,2,2,2,2,2,2,2,2",options="header"]', '|===')
    lines.push(`| ${['Dataset', ...columns.map(c => c[2])].join(' | ')}`)
    for (const row of section.rows) {
        const any = find('native', 'document', row) ?? find('node', 'javascript', row)
        const cells = columns.map(([where, family]) => {
            const impl = implOf(family, row)
            const r = impl && find(where, impl, row)
            if (!r) return '-'
            const marks = (section.markedOps?.[row.op] ?? section.marked ?? []).includes(family) ? ' *' : ''
            return `${time(r.median_ns)}${marks} +\n_${time(r.first_ns)}_`
        })
        lines.push('', `| ${rowLabel(row, sizes, any?.ops ?? 0)}`, ...cells.map(c => `| ${c}`))
    }
    lines.push('|===')
    return lines.join('\n')
}

const page = (results, meta) => {
    const index = new Map(results.map(r => [`${r.where} ${r.impl} ${key(r)}`, r]))
    const find = (where, impl, row) => index.get(`${where} ${impl} ${row.op} ${row.dataset}`)
    const opsOf = (op, dataset) => find('node', 'javascript', {op, dataset})?.ops ?? 0
    const sizes = {ring: opsOf('map', 'canada.ring'), statuses: opsOf('reduce', 'twitter.statuses'),
        events: opsOf('iterate', 'citm.events')}
    const lines = [
        '// Copyright (c) 2026 WebCpp.org',
        '//',
        '// Distributed under the Boost Software License, Version 1.0. (See',
        '// accompanying file LICENSE_1_0.txt or copy at',
        '// https://www.boost.org/LICENSE_1_0.txt)',
        '//',
        '// Generated by libs/document/bench/run.mjs, which `b2 libs/document/bench//results` runs: do',
        '// not edit it; measure again.',
        '',
        `Measured on ${meta.date}${quick ? ', with --quick: 3 samples of 1 ms, a check of the pipeline, not a measurement' : ''}, on this machine:`,
        '',
        '[cols="1,3",options="header"]',
        '|===',
        '| | Version',
        `| Machine | ${meta.cpu}, ${meta.cores} cores, ${meta.memory} GB`,
        `| Operating system | ${meta.os}`,
        `| Native compiler | ${meta.native}, release variant (\`-O3\`)`,
        `| Boost | ${meta.boost}`,
        `| Emscripten | ${meta.emscripten} (${meta.emscriptenClang}), release variant, linked with \`-O3\``,
        `| Node.js | ${meta.node}, V8 ${meta.nodeV8}`,
        `| Chrome | ${meta.chrome}, Playwright's headless shell, V8 ${meta.chromeV8}`,
        '|===',
        '',
        'Each cell is the median time of one operation, and below it, in italics, the time of the first',
        'one, before any warm-up. A `*` marks the closest form of an operation that the implementation has',
        'no direct form of (<<document-benchmarks-operations>>); a `-`, an implementation that the row does',
        'not concern.',
    ]
    for (const section of sections) {
        lines.push('', `[#document-benchmarks-${section.id}]`, `==== ${section.title}`, '',
            `From ${section.sources}; time ${section.per}.`, '', table(section, find, sizes))
    }
    return `${lines.join('\n')}\n`
}

// ---- the machine ----------------------------------------------------------------------------------------

const machine = () => {
    let cpu = os.cpus()[0]?.model ?? 'unknown'
    let system = `${os.type()} ${os.release()}`
    if (process.platform === 'darwin') {
        cpu = execFileSync('sysctl', ['-n', 'machdep.cpu.brand_string'], {encoding: 'utf8'}).trim()
        const version = execFileSync('sw_vers', ['-productVersion'], {encoding: 'utf8'}).trim()
        const build = execFileSync('sw_vers', ['-buildVersion'], {encoding: 'utf8'}).trim()
        system = `macOS ${version} (${build}), ${os.arch()}`
    } else if (existsSync('/etc/os-release')) {
        const name = /^PRETTY_NAME="?([^"\n]*)"?$/m.exec(readFileSync('/etc/os-release', 'utf8'))?.[1]
        if (name) system = `${name}, Linux ${os.release()}, ${os.arch()}`
    }
    return {cpu, os: system, cores: os.cpus().length, memory: Math.round(os.totalmem() / 2 ** 30)}
}

// ---- the run -----------------------------------------------------------------------------------------------

const runs = []
const quickWords = quick ? ['--quick'] : []
if (places.includes('native')) {
    for (const impl of implementations) {
        say(`native ${impl}`)
        runs.push(collect('native', await runProcess(resolve(native), ['--data', data, '--impl', impl, ...quickWords], 'native')))
    }
}
if (places.includes('node')) {
    for (const impl of implementations) {
        say(`node wasm ${impl}`)
        runs.push(collect('node', await runProcess(process.execPath,
            ['--expose-gc', join(here, 'node.mjs'), 'wasm', '--module', module, '--impl', impl, '--data', data, ...quickWords],
            'node')))
    }
    say('node javascript')
    runs.push(collect('node', await runProcess(process.execPath,
        ['--expose-gc', join(here, 'node.mjs'), 'js', '--module', module, '--data', data, ...quickWords], 'node')))
}
if (places.includes('chrome')) {
    say('chrome')
    runs.push(...await runChrome())
}

const results = runs.flatMap(r => r.results)
const faults = places.length === 3 ? check(results) : check(results).filter(f => / answers /.test(f))
if (faults.length) {
    say(`run.mjs: the implementations disagree; ${out} is left as it was:\n${faults.join('\n')}`)
    process.exit(1)
}
if (places.length !== 3) {
    say(`run.mjs: ${results.length} results of ${runs.length} runs agree; --places writes nothing`)
    process.exit(0)
}
const nativeMeta = runs[0].meta
const wasmMeta = runs[implementations.length].meta
const nodeMeta = runs[2 * implementations.length].meta
const chromeMeta = runs.at(-1).meta
const meta = {
    ...machine(),
    date: new Date().toISOString().slice(0, 10),
    native: nativeMeta.compiler,
    boost: nativeMeta.boost.replace('_', '.'),
    emscripten: wasmMeta.emscripten,
    // Emscripten's clang names its LLVM commit after its version, which says enough.
    emscriptenClang: wasmMeta.compiler.replace(/ \(.*$/, ''),
    node: nodeMeta.node,
    nodeV8: nodeMeta.v8,
    chrome: chromeMeta.chrome.replace('HeadlessChrome/', 'Chrome '),
    chromeV8: chromeMeta.v8,
}
const temporary = `${out}.tmp`
writeFileSync(temporary, page(results, meta))
renameSync(temporary, out)
say(`run.mjs: ${results.length} results of ${runs.length} runs agree; wrote ${out}`)
