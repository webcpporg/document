// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests a tarball of the package as a user receives it, on the Node that runs this:
//
//     node consume.mjs <tarball> <work dir> <tsc> <test dir>
//
// npm install of the tarball (offline: the package has no dependencies, so no registry is asked)
// into a fresh directory under <work dir>; there, smoke.mjs runs under node --test, importing
// '@webcpp/document' by its name, and tsc --noEmit checks consumer.ts against the package's
// declarations under both node's and a bundler's module resolution. run.mjs packs the staged
// package and runs this; the workflow runs it on Node 22, 24 and 26 on one tarball. The work
// directory is made again from nothing. Nothing here contacts a registry or publishes.

import { execFileSync } from 'node:child_process'
import { copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import assert from 'node:assert/strict'
import { join, resolve } from 'node:path'

const [tarball, work, tsc, tests] = process.argv.slice(2).map(argument => resolve(argument))
if (!tests) throw new Error('usage: consume.mjs <tarball> <work dir> <tsc> <test dir>')

// A failing command prints what it wrote, once, and ends the run.
const run = (command, args, options) => {
    try {
        return execFileSync(command, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...options })
    } catch (error) {
        console.error(`${command} ${args.join(' ')}\n${error.stdout ?? ''}${error.stderr ?? ''}`)
        process.exit(1)
    }
}

rmSync(work, { recursive: true, force: true })
const consumer = join(work, 'consumer')
mkdirSync(consumer, { recursive: true })
writeFileSync(join(consumer, 'package.json'), JSON.stringify({ name: 'consumer', private: true, type: 'module' }) + '\n')
run('npm', ['install', tarball, '--offline', '--no-audit', '--no-fund', '--ignore-scripts', '--no-package-lock'], { cwd: consumer })

const installed = join(consumer, 'node_modules', '@webcpp', 'document')
const manifest = JSON.parse(readFileSync(join(installed, 'package.json'), 'utf8'))
assert.equal(manifest.name, '@webcpp/document')
console.log(`node ${process.version}: ${manifest.name}@${manifest.version} installed from ${tarball}`)

for (const file of ['smoke.mjs', 'worker.mjs', 'consumer.ts', 'tsconfig.nodenext.json', 'tsconfig.bundler.json']) {
    copyFileSync(join(tests, file), join(consumer, file))
}
console.log(run('node', ['--test', '--test-reporter=spec', 'smoke.mjs'], { cwd: consumer }))
for (const config of ['tsconfig.nodenext.json', 'tsconfig.bundler.json']) {
    run('node', [tsc, '--project', config], { cwd: consumer })
    console.log(`tsc --noEmit -p ${config}: no errors`)
}
console.log(`${manifest.name}@${manifest.version}: the package works as a user receives it`)
