// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Stages the npm package @webcpp/document into a directory of the build:
//
//     node stage.mjs <stage dir> <core dir> <npm dir> <js dir> <licence>
//
// <core dir> holds the release module b2 built (webcpp-document-core.js, .wasm and .d.ts), <npm dir>
// this directory (the manifest template, the README, the declarations), and <js dir> the dev
// package, whose version the package takes and whose wrapper it ships. The stage directory is made
// again from nothing, so that nothing of an earlier stage stays in it.

import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'

const [stage, core, npm, js, licence] = process.argv.slice(2).map(argument => resolve(argument))
if (!licence) throw new Error('usage: stage.mjs <stage dir> <core dir> <npm dir> <js dir> <licence>')

const template = JSON.parse(readFileSync(join(npm, 'package.json'), 'utf8'))
const { version } = JSON.parse(readFileSync(join(js, 'package.json'), 'utf8'))
if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(version)) throw new Error(`not a version: ${version}`)

const sources = {
    'webcpp-document.mjs': join(js, 'webcpp-document.mjs'),
    'webcpp-document.d.mts': join(npm, 'webcpp-document.d.mts'),
    'webcpp-document-core.js': join(core, 'webcpp-document-core.js'),
    'webcpp-document-core.wasm': join(core, 'webcpp-document-core.wasm'),
    'webcpp-document-core.d.ts': join(core, 'webcpp-document-core.d.ts'),
    'README.md': join(npm, 'README.md'),
    'LICENSE_1_0.txt': licence,
}

// What the manifest ships is what is staged: a file listed and not staged, or staged and not listed,
// is a mistake of this script or of the template.
const listed = [...template.files].sort().join()
if (listed !== Object.keys(sources).sort().join()) {
    throw new Error(`package.json lists ${listed}, stage.mjs stages ${Object.keys(sources).sort().join()}`)
}
for (const [name, source] of Object.entries(sources)) {
    if (!existsSync(source)) throw new Error(`${name} is missing: ${source}`)
}

rmSync(stage, { recursive: true, force: true })
mkdirSync(stage, { recursive: true })
for (const [name, source] of Object.entries(sources)) cpSync(source, join(stage, name))
writeFileSync(join(stage, 'package.json'), JSON.stringify({ ...template, version }, null, 2) + '\n')
console.log(`staged @webcpp/document ${version} into ${stage}`)
