// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Writes the random cases, random-000.json to random-199.json, in the case
// format oracle.mjs describes. It is run once and its output committed; the
// fixed seed makes a second run write the same bytes.
//
// Usage: node generate.mjs <cases directory>
//
// Each document is an object or an array, nested up to five levels deep, its
// keys drawn from a set that holds array-index look-alikes; its numbers are
// doubles written by JSON.stringify, and in every fourth case (js_only) also
// numbers of 18 to 30 significant digits, which a double does not keep. Each
// case has 1 to 10 operations, each kept only once oracle.mjs has applied it
// in JavaScript without refusing it.

import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { apply, check, layout } from './oracle.mjs';

const SEED = 20261012;
const CASES = 200;
const MAX_DEPTH = 5;
const MAX_OPERATIONS = 10;
// Tries to find an operation the oracle accepts, before the case stops
// growing.
const TRIES = 100;

const KEYS = ['0', '1', '2', '01', '-0', '1e2', '4294967294', '4294967295', '', 'a', 'b', 'c', 'z', 'é'];
const CHARACTERS = ['a', 'b', 'z', '0', ' ', '"', '\\', '/', '\n', '\t', '\u0001', '\u001f', '\u007f',
  'é', ' ', ' ', '\u{1f600}'];

// Marsaglia's xorshift32.
function generator(seed) {
  let x = seed >>> 0;
  return () => {
    x ^= x << 13;
    x >>>= 0;
    x ^= x >>> 17;
    x ^= x << 5;
    x >>>= 0;
    return x;
  };
}

const next = generator(SEED);

function below(n) {
  return next() % n;
}

function chance(numerator, denominator) {
  return below(denominator) < numerator;
}

function pick(list) {
  return list[below(list.length)];
}

// A number's JSON text, held as it is written, never through a double.
class NumberText {
  constructor(text) {
    this.text = text;
  }
}

function doubleText() {
  switch (below(4)) {
    case 0:
      return String(below(2001) - 1000);
    case 1:
      return JSON.stringify((below(200001) - 100000) / 10 ** below(7));
    case 2: {
      // A random significand of up to 17 digits, at a random scale.
      const significand = (next() * 2 ** 21 + (next() >>> 11)) / 2 ** 53;
      const sign = chance(1, 2) ? -1 : 1;
      return JSON.stringify(sign * significand * 10 ** (below(61) - 30));
    }
    default:
      return pick(['0', '1', '-1', '0.5', '1e+21', '1e-7', '100000000000000000000', '9007199254740992',
        '1.7976931348623157e+308', '0.1', '123.45']);
  }
}

// A number of 18 to 30 significant digits, its first and last digits not 0.
function longText() {
  const count = 18 + below(13);
  let digits = String(1 + below(9));
  for (let i = 2; i < count; ++i) {
    digits += String(below(10));
  }
  digits += String(1 + below(9));
  const sign = chance(1, 3) ? '-' : '';
  switch (below(3)) {
    case 0:
      return sign + digits;
    case 1: {
      const point = 1 + below(count - 1);
      return `${sign}${digits.slice(0, point)}.${digits.slice(point)}`;
    }
    default:
      return `${sign}${digits[0]}.${digits.slice(1)}e${pick(['', '+', '-'])}${below(40)}`;
  }
}

function numberText(long) {
  return new NumberText(long && chance(1, 2) ? longText() : doubleText());
}

function stringValue() {
  let s = '';
  for (let n = below(7); n > 0; --n) {
    s += pick(CHARACTERS);
  }
  return s;
}

function scalar(long) {
  switch (below(6)) {
    case 0:
      return null;
    case 1:
      return chance(1, 2);
    case 2:
    case 3:
      return numberText(long);
    default:
      return stringValue();
  }
}

// A value whose containers sit at depth and below, MAX_DEPTH at most.
function value(depth, long, container = false) {
  if (!container && (depth >= MAX_DEPTH || chance(1, 2))) {
    return scalar(long);
  }
  if (chance(1, 2)) {
    return Array.from({ length: below(depth >= MAX_DEPTH ? 1 : 5) }, () => value(depth + 1, long));
  }
  const entries = [];
  for (let n = below(depth >= MAX_DEPTH ? 1 : 5); n > 0; --n) {
    entries.push([pick(KEYS), value(depth + 1, long)]);
  }
  return { entries };
}

// JSON text of a value; space, when not empty, follows each colon and comma.
function text(v, space) {
  if (v instanceof NumberText) {
    return v.text;
  }
  if (Array.isArray(v)) {
    return `[${v.map((element) => text(element, space)).join(`,${space}`)}]`;
  }
  if (v !== null && typeof v === 'object') {
    const members = v.entries.map(([key, member]) => `${JSON.stringify(key)}:${space}${text(member, space)}`);
    return `{${members.join(`,${space}`)}}`;
  }
  return JSON.stringify(v);
}

// Every array and object of the document with its path, holes skipped.
function containers(v, path = [], found = []) {
  if (Array.isArray(v)) {
    found.push({ path, v });
    v.forEach((element, i) => containers(element, [...path, i], found));
  } else if (v !== null && typeof v === 'object') {
    found.push({ path, v });
    for (const key of Object.keys(v)) {
      containers(v[key], [...path, key], found);
    }
  }
  return found;
}

function operationValue(long) {
  return text(value(MAX_DEPTH - 2, long), '');
}

function operationValues(long) {
  return Array.from({ length: below(4) }, () => operationValue(long));
}

function operation(root, long) {
  const all = containers(root);
  const arrays = all.filter((c) => Array.isArray(c.v));
  const objects = all.filter((c) => !Array.isArray(c.v));
  const names = ['set', 'set'];
  if (arrays.length > 0) {
    names.push('push', 'pop', 'shift', 'unshift', 'splice', 'splice', 'reverse', 'set_length');
  }
  if (objects.length > 0) {
    names.push('erase', 'erase', 'assign_from');
  }
  const name = pick(names);
  if (name === 'set') {
    const { path, v } = pick(all);
    const last = Array.isArray(v) ? below(v.length + 3) : pick(KEYS);
    return ['set', [...path, last], operationValue(long)];
  }
  if (name === 'erase') {
    const { path, v } = pick(objects);
    const existing = Object.keys(v);
    return ['erase', path, existing.length > 0 && chance(3, 4) ? pick(existing) : pick(KEYS)];
  }
  if (name === 'assign_from') {
    const { path } = pick(objects);
    const kind = below(10);
    let source = null;
    if (kind < 7) {
      source = { entries: Array.from({ length: below(5) }, () => [pick(KEYS), value(MAX_DEPTH - 1, long)]) };
    } else if (kind < 9) {
      source = Array.from({ length: below(4) }, () => value(MAX_DEPTH - 1, long));
    }
    return ['assign_from', path, text(source, '')];
  }
  const { path, v } = pick(arrays);
  switch (name) {
    case 'push':
    case 'unshift':
      return [name, path, operationValues(long)];
    case 'splice':
      return ['splice', path, below(2 * v.length + 5) - v.length - 2, below(v.length + 3) - 1,
        operationValues(long)];
    case 'set_length':
      return ['set_length', path, below(v.length + 4)];
    default:
      return [name, path];
  }
}

function randomCase(long) {
  for (;;) {
    const root = value(0, long, true);
    const theCase = {
      text: text(root, chance(1, 4) ? ' ' : ''),
      indent: below(11),
      js_only: long,
      operations: []
    };
    // A js_only case holds a number a double does not keep, in its text.
    if (check(theCase).jsOnly !== long) {
      continue;
    }
    let state = JSON.parse(theCase.text);
    const count = 1 + below(MAX_OPERATIONS);
    for (let tries = 0; theCase.operations.length < count && tries < TRIES; ++tries) {
      const candidate = operation(state, long);
      const applied = structuredClone(state);
      try {
        apply(applied, candidate);
      } catch {
        continue;
      }
      theCase.operations.push(candidate);
      state = applied;
    }
    if (theCase.operations.length === count) {
      return theCase;
    }
  }
}

function main() {
  const [casesDirectory] = process.argv.slice(2);
  if (!casesDirectory) {
    console.error('usage: node generate.mjs <cases directory>');
    process.exit(2);
  }
  mkdirSync(casesDirectory, { recursive: true });
  for (let i = 0; i < CASES; ++i) {
    const theCase = randomCase(i % 4 === 3);
    if (check(theCase).jsOnly !== theCase.js_only) {
      console.error(`generate: random-${i}: js_only is not what its numbers need`);
      process.exit(1);
    }
    writeFileSync(join(casesDirectory, `random-${String(i).padStart(3, '0')}.json`), layout(theCase));
  }
}

main();
