// Copyright 2026 Robomous
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// node:test — the runner Node ships. The site's licence gate makes every added
// package a permanent obligation, so a test runner is not worth one.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { relativize, relativizePage, resolveTarget, htmlFiles } from '../scripts/relativize.mjs';

/** A throwaway dist/ with the shapes the real build produces. */
function fixture() {
  const dir = mkdtempSync(join(tmpdir(), 'rm-relativize-'));
  mkdirSync(join(dir, 'reference'), { recursive: true });
  mkdirSync(join(dir, '_astro'), { recursive: true });
  writeFileSync(join(dir, '_astro', 'index.css'), 'body{}');
  writeFileSync(join(dir, '_astro', 'shot.webp'), '');
  writeFileSync(join(dir, 'favicon.svg'), '<svg/>');
  writeFileSync(join(dir, 'index.html'), '<a href="/reference/junction.html">j</a>');
  writeFileSync(
    join(dir, 'reference', 'junction.html'),
    [
      '<link href="/_astro/index.css">',
      '<a href="/index.html">home</a>',
      '<a href="/reference/create-road.html">sibling</a>',
      // The adapter emits Starlight slugs, so content links arrive extensionless.
      '<a href="/reference/create-road">slug form</a>',
      '<a href="/reference/create-road#lanes">with an anchor</a>',
      '<img src="/_astro/shot.webp" srcset="/_astro/shot.webp 1x, /_astro/shot.webp 2x">',
      '<a href="https://example.invalid/x">external</a>',
      '<a href="#top">in-page</a>',
    ].join('\n'),
  );
  writeFileSync(join(dir, 'reference', 'create-road.html'), '<p>hi</p>');
  return dir;
}

test('resolveTarget accepts the built filename, the slug, and a directory', () => {
  const dir = fixture();
  try {
    assert.equal(resolveTarget(dir, '/reference/junction.html'), 'reference/junction.html');
    assert.equal(resolveTarget(dir, '/reference/junction'), 'reference/junction.html');
    assert.equal(resolveTarget(dir, '/'), 'index.html');
    assert.equal(resolveTarget(dir, '/reference/nope'), '');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('a nested page gets ../-relative references and keeps its anchors', () => {
  const dir = fixture();
  try {
    const page = 'reference/junction.html';
    const { text, unresolved } = relativizePage(dir, page, readFileSync(join(dir, page), 'utf8'));
    assert.deepEqual(unresolved, []);
    assert.match(text, /href="\.\.\/_astro\/index\.css"/);
    assert.match(text, /href="\.\.\/index\.html"/);
    assert.match(text, /href="\.\/create-road\.html"/);
    assert.match(text, /href="\.\/create-road\.html#lanes"/);
    assert.match(text, /srcset="\.\.\/_astro\/shot\.webp 1x, \.\.\/_astro\/shot\.webp 2x"/);
    // Untouched: neither is root-absolute.
    assert.match(text, /href="https:\/\/example\.invalid\/x"/);
    assert.match(text, /href="#top"/);
    assert.equal(text.includes('="/'), false);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('running it twice equals running it once', () => {
  const dir = fixture();
  try {
    const first = relativize(dir);
    assert.deepEqual(first.problems, []);
    assert.ok(first.touched > 0, 'the first pass must actually change something');
    const after = htmlFiles(dir).map((rel) => readFileSync(join(dir, rel), 'utf8'));

    const second = relativize(dir);
    assert.deepEqual(second.problems, []);
    assert.equal(second.touched, 0, 'the second pass must rewrite nothing');
    assert.deepEqual(
      htmlFiles(dir).map((rel) => readFileSync(join(dir, rel), 'utf8')),
      after,
      'byte-identical after a second pass',
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('a reference matching no file is reported, not silently rewritten', () => {
  const dir = fixture();
  try {
    writeFileSync(join(dir, 'reference', 'junction.html'), '<a href="/reference/renamed-away">x</a>');
    const { problems } = relativize(dir);
    assert.equal(problems.length, 1);
    assert.match(problems[0], /renamed-away/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
