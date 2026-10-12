// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the staged package as a user receives it:
//
//     node run.mjs <stage dir> <work dir> <tsc> <test dir>
//
// 1. npm pack --dry-run lists what would ship, which must be exactly the files the manifest names,
//    and each of them must be there and not empty;
// 2. npm pack makes the tarball, <work dir>/packed/*.tgz, which the workflow also tests on other
//    versions of Node;
// 3. consume.mjs installs the tarball into a fresh directory and tests it there.
//
// The work directory is made again from nothing. Nothing here contacts a registry or publishes.

import { execFileSync } from 'node:child_process'
import { mkdirSync, readFileSync, rmSync } from 'node:fs'
import assert from 'node:assert/strict'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const [stage, work, tsc, tests] = process.argv.slice(2).map(argument => resolve(argument))
if (!tests) throw new Error('usage: run.mjs <stage dir> <work dir> <tsc> <test dir>')

// A failing command prints what it wrote, once, and ends the run.
const run = (command, args, options) => {
    try {
        return execFileSync(command, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...options })
    } catch (error) {
        console.error(`${command} ${args.join(' ')}\n${error.stdout ?? ''}${error.stderr ?? ''}`)
        process.exit(1)
    }
}

const manifest = JSON.parse(readFileSync(join(stage, 'package.json'), 'utf8'))

// 1. What ships.
const dry = JSON.parse(run('npm', ['pack', '--dry-run', '--json', '--ignore-scripts'], { cwd: stage }))[0]
const shipped = dry.files.map(file => file.path).sort()
const expected = [...manifest.files, 'package.json'].sort()
assert.deepEqual(shipped, expected, 'the files npm pack ships are the manifest\'s files and package.json')
for (const file of dry.files) assert.ok(file.size > 0, `${file.path} is empty`)
console.log(`npm pack --dry-run: ${manifest.name}@${manifest.version}, ${dry.files.length} files, ` +
    `${dry.size} bytes packed, ${dry.unpackedSize} unpacked`)
for (const file of dry.files) console.log(`  ${String(file.size).padStart(8)}  ${file.path}`)

// 2. The tarball.
rmSync(work, { recursive: true, force: true })
const packed = join(work, 'packed')
mkdirSync(packed, { recursive: true })
const tarball = join(packed, run('npm', ['pack', '--ignore-scripts', '--pack-destination', packed, '--silent'], { cwd: stage }).trim())

// 3. The user's program, and the type check, on this Node.
const consume = join(dirname(fileURLToPath(import.meta.url)), 'consume.mjs')
console.log(run('node', [consume, tarball, join(work, 'installed'), tsc, tests]))
