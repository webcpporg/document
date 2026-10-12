// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Runs every document case on JavaScript itself and writes what it produced.
// test/oracle_test.cpp runs the same cases on webcpp::document and compares
// its output with these files.
//
// Usage: node oracle.mjs <cases directory> <expected directory>
//
// A case, cases/<name>.json:
//
//     {
//         "text": "<JSON text>",
//         "indent": <JSON.stringify's third argument, 0 to 10; more is clamped to 10>,
//         "js_only": <bool>,
//         "operations": [<operation>, ...]
//     }
//
// Every value an operation carries is JSON text in a string, so that neither
// side reads a number through a double: "1.50", "\"x\"", "[1,{\"a\":null}]".
// A path is a list of steps from the root, each a key (a string) of an object
// or an index (a non-negative integer) of an array; [] is the root. The
// operations, and what JavaScript runs for each, with t the value at path:
//
//     ["set", path, "<json>"]                 t's parent[last step] = JSON.parse(json);
//                                             path is not [], its last step names the place
//     ["erase", path, "<key>"]                delete t[key]; key is the key itself, not JSON
//     ["push", path, ["<json>", ...]]         t.push(...values)
//     ["pop", path]                           t.pop()
//     ["shift", path]                         t.shift()
//     ["unshift", path, ["<json>", ...]]      t.unshift(...values)
//     ["splice", path, start, count, ["<json>", ...]]
//                                             t.splice(start, count, ...values); start and
//                                             count are integers, either may be negative
//     ["reverse", path]                       t.reverse()
//     ["set_length", path, n]                 t.length = n
//     ["assign_from", path, "<json>"]         Object.assign(t, JSON.parse(json))
//
// Its expected file, expected/<name>.json, written by this script only, with
// JSON.stringify(x, null, 4) and a final newline:
//
//     {
//         "text": "<JSON.stringify(result, null, indent)>",
//         "returns": [<one per operation>]
//     }
//
// A return is the compact JSON.stringify text of what the operation returns,
// or null when it returns undefined or nothing: push and unshift give the new
// length ("7"); pop and shift the removed value (null for an empty array);
// splice the removed array ("[1,2]"); erase "true" or "false", whether the
// key was an own property before the delete; set, reverse, set_length and
// assign_from null.
//
// js_only is true exactly when a number of the case, in its text or in an
// operation's value, is one a double does not keep: its exact decimal value
// differs from the value of what JavaScript writes for it (18 to 30 digits,
// 9007199254740993, 0.10000000000000001). The C++ side runs such a case with
// js_traits alone, and every other case with default_traits and js_traits.
//
// A case holds only what both sides do alike, and this script refuses one
// that holds anything else, since the two differ there by design or
// JavaScript throws: a lone surrogate; a number past a double's range, below
// its smallest, or of more than 34 significant digits written; a path through
// a missing member or element, a key step on an array or an index step on an
// object; a method on what is not an array; erase or assign_from on what is
// not an object, assign_from from a scalar; the key __proto__ anywhere in an
// operation (assigning to it sets a prototype); pop or shift of a hole, which
// JavaScript reads as undefined where the document holds null. A hole
// itself, left by set past the end or set_length, is allowed: JSON.stringify
// writes it null, as the document stores it.

import { mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const OPERATIONS = {
  set: ['json'],
  erase: ['key'],
  push: ['values'],
  pop: [],
  shift: [],
  unshift: ['values'],
  splice: ['integer', 'integer', 'values'],
  reverse: [],
  set_length: ['length'],
  assign_from: ['json']
};

// An array index is below 2^32 - 1; a larger one is a property of the array.
const INDEX_LIMIT = 2 ** 32 - 1;

const MAX_DIGITS = 34;

const NUMBER = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;

function fail(message) {
  throw new Error(message);
}

function isObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

// A number's exact decimal value as one string, sign, significant digits and
// the decimal point's place, "0" for any zero; and how many digits it is
// written with, leading zeros aside.
function exact(token) {
  const m = /^(-?)([0-9]+)(?:\.([0-9]+))?(?:[eE]([+-]?[0-9]+))?$/.exec(token);
  if (m === null) {
    fail(`${token} is not a JSON number`);
  }
  let digits = m[2] + (m[3] ?? '');
  let point = m[2].length + Number(m[4] ?? '0');
  const leading = /^0*/.exec(digits)[0].length;
  digits = digits.slice(leading);
  point -= leading;
  const written = digits.length;
  digits = digits.replace(/0+$/, '');
  if (digits === '') {
    return { value: '0', written };
  }
  return { value: `${m[1]}${digits}e${point}`, written };
}

// Every number token of a JSON text, which JSON.parse has accepted.
function numbersOf(text) {
  const found = [];
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    if (c === '"') {
      ++i;
      while (text[i] !== '"') {
        i += text[i] === '\\' ? 2 : 1;
      }
      ++i;
    } else if (c === '-' || (c >= '0' && c <= '9')) {
      NUMBER.lastIndex = i;
      const token = NUMBER.exec(text)[0];
      found.push(token);
      i += token.length;
    } else {
      ++i;
    }
  }
  return found;
}

// Whether a double does not keep the number token; refuses one that both
// sides cannot hold.
function beyondDouble(token) {
  const { value, written } = exact(token);
  if (written > MAX_DIGITS) {
    fail(`${token} has more than ${MAX_DIGITS} significant digits`);
  }
  const v = Number(token);
  if (!Number.isFinite(v)) {
    fail(`${token} is past a double's range`);
  }
  if (v === 0 && value !== '0') {
    fail(`${token} is below a double's smallest value`);
  }
  return exact(String(v)).value !== value;
}

// Refuses a lone surrogate or, in an operation's value, the key __proto__,
// anywhere in v.
function checkValue(v, inOperation) {
  if (typeof v === 'string') {
    if (!v.isWellFormed()) {
      fail('a string holds a lone surrogate');
    }
  } else if (Array.isArray(v)) {
    for (const element of v) {
      checkValue(element, inOperation);
    }
  } else if (isObject(v)) {
    for (const key of Object.keys(v)) {
      if (!key.isWellFormed()) {
        fail('a key holds a lone surrogate');
      }
      if (inOperation && key === '__proto__') {
        fail('an operation holds the key __proto__');
      }
      checkValue(v[key], inOperation);
    }
  }
}

// The value a JSON text holds, checked; and whether a number of it needs
// js_only.
function parsed(text, inOperation) {
  if (typeof text !== 'string') {
    fail(`${JSON.stringify(text)} is not JSON text in a string`);
  }
  let value;
  try {
    value = JSON.parse(text);
  } catch (error) {
    fail(`${JSON.stringify(text)} is not JSON: ${error.message}`);
  }
  checkValue(value, inOperation);
  const jsOnly = numbersOf(text).some(beyondDouble);
  return { value, jsOnly };
}

function checkStep(step) {
  if (typeof step === 'string') {
    if (step === '__proto__') {
      fail('a path holds the key __proto__');
    }
    if (!step.isWellFormed()) {
      fail('a path holds a lone surrogate');
    }
  } else if (!Number.isSafeInteger(step) || step < 0) {
    fail(`the step ${JSON.stringify(step)} is neither a key nor an index`);
  }
}

// The value at path, which every step must reach: a key of an object that
// has it, or an index of an array that holds an element there.
function resolve(root, path) {
  if (!Array.isArray(path)) {
    fail(`${JSON.stringify(path)} is not a path`);
  }
  let t = root;
  for (const step of path) {
    checkStep(step);
    if (Array.isArray(t)) {
      if (typeof step !== 'number') {
        fail(`the key step ${JSON.stringify(step)} is on an array`);
      }
      if (!(step in t)) {
        fail(`the path reaches no element at ${step}`);
      }
    } else if (isObject(t)) {
      if (typeof step !== 'string') {
        fail(`the index step ${step} is on an object`);
      }
      if (!Object.hasOwn(t, step)) {
        fail(`the path reaches no member ${JSON.stringify(step)}`);
      }
    } else {
      fail(`the path goes through ${JSON.stringify(t)}`);
    }
    t = t[step];
  }
  return t;
}

function arrayAt(root, path, name) {
  const t = resolve(root, path);
  if (!Array.isArray(t)) {
    fail(`${name} on ${JSON.stringify(t)}, which is not an array`);
  }
  return t;
}

function values(list, name) {
  if (!Array.isArray(list)) {
    fail(`${name}'s values are not a list`);
  }
  return list.map((text) => parsed(text, true).value);
}

function returned(v) {
  return v === undefined ? null : JSON.stringify(v);
}

// Applies one operation to the document root in place, as JavaScript does,
// and gives its return as the expected file writes it. Throws on an
// operation that the case format does not allow.
export function apply(root, operation) {
  if (!Array.isArray(operation) || !Object.hasOwn(OPERATIONS, operation[0])) {
    fail(`${JSON.stringify(operation)} is not an operation`);
  }
  const [name, path, ...rest] = operation;
  if (rest.length !== OPERATIONS[name].length) {
    fail(`${name} takes ${OPERATIONS[name].length + 1} arguments`);
  }
  switch (name) {
    case 'set': {
      if (!Array.isArray(path) || path.length === 0) {
        fail('set needs a path to a member or an element');
      }
      const parent = resolve(root, path.slice(0, -1));
      const last = path[path.length - 1];
      checkStep(last);
      const v = parsed(rest[0], true).value;
      if (Array.isArray(parent)) {
        if (typeof last !== 'number' || last >= INDEX_LIMIT) {
          fail(`set of ${JSON.stringify(last)} on an array`);
        }
      } else if (isObject(parent)) {
        if (typeof last !== 'string') {
          fail(`set of the index ${last} on an object`);
        }
      } else {
        fail(`set inside ${JSON.stringify(parent)}`);
      }
      parent[last] = v;
      return null;
    }
    case 'erase': {
      const t = resolve(root, path);
      const key = rest[0];
      if (!isObject(t)) {
        fail(`erase on ${JSON.stringify(t)}, which is not an object`);
      }
      if (typeof key !== 'string') {
        fail('erase needs a key');
      }
      checkStep(key);
      const had = Object.hasOwn(t, key);
      delete t[key];
      return returned(had);
    }
    case 'push':
      return returned(arrayAt(root, path, name).push(...values(rest[0], name)));
    case 'unshift':
      return returned(arrayAt(root, path, name).unshift(...values(rest[0], name)));
    case 'pop': {
      const t = arrayAt(root, path, name);
      if (t.length > 0 && !(t.length - 1 in t)) {
        fail('pop of a hole, which JavaScript reads as undefined and the document as null');
      }
      return returned(t.pop());
    }
    case 'shift': {
      const t = arrayAt(root, path, name);
      if (t.length > 0 && !(0 in t)) {
        fail('shift of a hole, which JavaScript reads as undefined and the document as null');
      }
      return returned(t.shift());
    }
    case 'splice': {
      const t = arrayAt(root, path, name);
      const [start, count, list] = rest;
      if (!Number.isSafeInteger(start) || !Number.isSafeInteger(count)) {
        fail('splice needs an integer start and count');
      }
      return returned(t.splice(start, count, ...values(list, name)));
    }
    case 'reverse':
      arrayAt(root, path, name).reverse();
      return null;
    case 'set_length': {
      const t = arrayAt(root, path, name);
      const n = rest[0];
      if (!Number.isSafeInteger(n) || n < 0 || n >= INDEX_LIMIT) {
        fail('set_length needs a length');
      }
      t.length = n;
      return null;
    }
    case 'assign_from': {
      const t = resolve(root, path);
      if (!isObject(t)) {
        fail(`assign_from on ${JSON.stringify(t)}, which is not an object`);
      }
      const source = parsed(rest[0], true).value;
      if (source !== null && typeof source !== 'object') {
        fail(`assign_from from the scalar ${rest[0]}`);
      }
      Object.assign(t, source);
      return null;
    }
    default:
      return fail(`${name} is not an operation`);
  }
}

const MEMBERS = ['text', 'indent', 'js_only', 'operations'];

// The case's document, checked: its text parsed, its indent, and whether its
// numbers need js_only (computed, whatever the case says).
export function check(theCase) {
  if (!isObject(theCase)) {
    fail('a case is an object');
  }
  for (const key of Object.keys(theCase)) {
    if (!MEMBERS.includes(key)) {
      fail(`a case has no member ${JSON.stringify(key)}`);
    }
  }
  for (const key of MEMBERS) {
    if (!Object.hasOwn(theCase, key)) {
      fail(`the case has no ${key}`);
    }
  }
  const { indent, operations } = theCase;
  if (!Number.isSafeInteger(indent) || indent < 0 || indent >= 2 ** 32) {
    fail('indent is a non-negative integer');
  }
  if (typeof theCase.js_only !== 'boolean') {
    fail('js_only is true or false');
  }
  if (!Array.isArray(operations)) {
    fail('operations is a list');
  }
  const { value, jsOnly } = parsed(theCase.text, false);
  const operationsJsOnly = operations.some((operation) => {
    if (!Array.isArray(operation)) {
      return false;
    }
    const texts = [];
    if (['set', 'assign_from'].includes(operation[0])) {
      texts.push(operation[2]);
    } else if (['push', 'unshift'].includes(operation[0]) && Array.isArray(operation[2])) {
      texts.push(...operation[2]);
    } else if (operation[0] === 'splice' && Array.isArray(operation[4])) {
      texts.push(...operation[4]);
    }
    return texts.some((text) => parsed(text, true).jsOnly);
  });
  return { root: value, jsOnly: jsOnly || operationsJsOnly };
}

// The expected result of a case: the case checked, js_only as its numbers
// need, and every operation applied in order.
export function run(theCase) {
  const { root, jsOnly } = check(theCase);
  if (theCase.js_only !== jsOnly) {
    fail(jsOnly
      ? 'js_only is false, and a number of the case is one a double does not keep'
      : 'js_only is true, and every number of the case is one a double keeps');
  }
  const returns = theCase.operations.map((operation, i) => {
    try {
      return apply(root, operation);
    } catch (error) {
      return fail(`operation ${i}: ${error.message}`);
    }
  });
  return { text: JSON.stringify(root, null, theCase.indent), returns };
}

// A case file's text: one member per line, each operation on a line of its
// own.
export function layout(theCase) {
  const lines = MEMBERS.filter((key) => key !== 'operations')
    .map((key) => `    ${JSON.stringify(key)}: ${JSON.stringify(theCase[key])}`);
  const operations = theCase.operations.length === 0
    ? '[]'
    : `[\n${theCase.operations.map((operation) => `        ${JSON.stringify(operation)}`).join(',\n')}\n    ]`;
  lines.push(`    "operations": ${operations}`);
  return `{\n${lines.join(',\n')}\n}\n`;
}

function main() {
  const [casesDirectory, expectedDirectory] = process.argv.slice(2);
  if (!casesDirectory || !expectedDirectory) {
    console.error('usage: node oracle.mjs <cases directory> <expected directory>');
    process.exit(2);
  }
  mkdirSync(expectedDirectory, { recursive: true });
  const files = readdirSync(casesDirectory).filter((name) => name.endsWith('.json')).sort();
  if (files.length === 0) {
    console.error(`oracle: ${casesDirectory} holds no case`);
    process.exit(1);
  }
  for (const file of files) {
    let expected;
    try {
      expected = run(JSON.parse(readFileSync(join(casesDirectory, file), 'utf8')));
    } catch (error) {
      console.error(`oracle: ${file}: ${error.message}`);
      process.exit(1);
    }
    writeFileSync(join(expectedDirectory, file), `${JSON.stringify(expected, null, 4)}\n`);
  }
}

// generate.mjs imports this module; only a run of it as a script writes.
if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  main();
}
