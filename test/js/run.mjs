// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Runs one check of document's JavaScript API under webcpp.drive:
//
//     node run.mjs <argument>... <program>
//
// node runs with the arguments, which name a test runner, a script or both, and its output and exit
// status are the check's. The last argument is the Emscripten module that webcpp.drive builds and
// passes to every script; the check reads the package directory of the build instead, so it is
// dropped here.

import { spawnSync } from 'node:child_process'

const argumentsOfNode = process.argv.slice(2, -1)
if (argumentsOfNode.length === 0) throw new Error('usage: run.mjs <argument>... <program>')

const finished = spawnSync(process.execPath, argumentsOfNode, { stdio: 'inherit' })
if (finished.error) throw finished.error
process.exit(finished.status ?? 1)
