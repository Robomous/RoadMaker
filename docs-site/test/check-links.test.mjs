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

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { checkRoot, checkPublishRoot, findRoots, resolveRef } from '../scripts/check-links.mjs';
import { assemble } from '../scripts/assemble.mjs';
import { collectExternalLinks } from '../scripts/report-external-links.mjs';

/** A built version directory whose links are all sound. */
function makeVersion(label, extraBody = '') {
  const dir = mkdtempSync(join(tmpdir(), 'rm-ver-'));
  mkdirSync(join(dir, 'reference'), { recursive: true });
  mkdirSync(join(dir, 'img'), { recursive: true });
  writeFileSync(join(dir, 'img', 'shot.png'), '');
  writeFileSync(
    join(dir, 'index.html'),
    `<p>${label}</p><a href="/${label}/reference/junction.html">j</a>${extraBody}`,
  );
  writeFileSync(
    join(dir, 'reference', 'junction.html'),
    `<a href="../index.html">home</a><img src="../img/shot.png">` +
      `<a href="https://example.invalid/x">out</a><a href="#top">here</a>` +
      `<a href="rmmanual:tutorials/x">bridge</a>`,
  );
  return dir;
}

test('a sound build passes and counts what it checked', () => {
  const dir = makeVersion('dev');
  try {
    const result = checkRoot({ label: 'dev', dir, base: '/dev/' });
    assert.deepEqual(result.problems, []);
    assert.equal(result.pages, 2);
    assert.ok(result.refs > 0);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('external, in-page, and rmmanual references are not treated as files', () => {
  // rmmanual: is the in-app bridge scheme — it resolves at runtime against the
  // packaged manual and can never be a file in this tree. Reporting any of
  // these as broken would be a flood of false positives.
  for (const url of [
    'https://x.invalid/y',
    'http://x.invalid/y',
    'mailto:someone@example.invalid',
    'rmmanual:tutorials/getting-around',
    '//cdn.invalid/lib.js',
  ]) {
    assert.equal(
      resolveRef('/nowhere', '/dev/', 'index.html', url).kind,
      'external',
      `${url} must be treated as external`,
    );
  }
  assert.equal(resolveRef('/nowhere', '/dev/', 'index.html', '#top').kind, 'in-page');
});

test('a dangling relative link is reported', () => {
  const dir = makeVersion('dev', '<a href="reference/gone.html">x</a>');
  try {
    const { problems } = checkRoot({ label: 'dev', dir, base: '/dev/' });
    assert.equal(problems.length, 1);
    assert.match(problems[0], /gone\.html/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('a missing image is reported', () => {
  const dir = makeVersion('dev', '<img src="img/absent.png">');
  try {
    const { problems } = checkRoot({ label: 'dev', dir, base: '/dev/' });
    assert.equal(problems.length, 1);
    assert.match(problems[0], /absent\.png/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('an absolute link that leaves this version is reported', () => {
  // The exact shape of a cross-version link written by hand: it would silently
  // send a v0.1.0 reader into dev.
  const dir = makeVersion('dev', '<a href="/v0.1.0/reference/junction.html">other</a>');
  try {
    const { problems } = checkRoot({ label: 'dev', dir, base: '/dev/' });
    assert.equal(problems.length, 1);
    assert.match(problems[0], /outside the base/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('a relative link climbing out of the version root is reported', () => {
  const dir = makeVersion('dev', '<a href="../../etc/passwd">x</a>');
  try {
    const { problems } = checkRoot({ label: 'dev', dir, base: '/dev/' });
    assert.equal(problems.length, 1);
    assert.match(problems[0], /escapes the version root/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('the assembled tree is checked version by version, latest included', () => {
  const root = mkdtempSync(join(tmpdir(), 'rm-pub-'));
  const dev = makeVersion('dev');
  const rel = makeVersion('v0.1.0');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    assemble({ publishRoot: root, buildDir: rel, segment: 'v0.1.0' });

    const roots = findRoots(root);
    assert.deepEqual(
      roots.map((r) => r.label).sort(),
      ['dev', 'latest', 'v0.1.0'],
      'every version directory, plus latest/ — which readers land on',
    );

    // latest/ is a BYTE COPY of v0.1.0, so its pages carry v0.1.0's base.
    // findRoots has to hand it that base or every link in it reads as broken.
    assert.equal(roots.find((r) => r.label === 'latest').base, '/v0.1.0/');

    for (const one of roots) {
      assert.deepEqual(checkRoot(one).problems, [], `${one.label} should be clean`);
    }
    assert.deepEqual(checkPublishRoot(root), []);
  } finally {
    for (const d of [root, dev, rel]) rmSync(d, { recursive: true, force: true });
  }
});

test('a publish root whose redirect disagrees with the manifest is reported', () => {
  const root = mkdtempSync(join(tmpdir(), 'rm-pub-'));
  const dev = makeVersion('dev');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    writeFileSync(join(root, 'index.html'), '<p>nowhere in particular</p>');
    const problems = checkPublishRoot(root);
    assert.equal(problems.length, 1);
    assert.match(problems[0], /root redirect/);
  } finally {
    rmSync(root, { recursive: true, force: true });
    rmSync(dev, { recursive: true, force: true });
  }
});

test('external links are collected with the pages that use them', () => {
  const dir = makeVersion('dev');
  try {
    const found = collectExternalLinks(dir);
    assert.ok(found.has('https://example.invalid/x'));
    assert.deepEqual([...found.get('https://example.invalid/x')], ['reference/junction.html']);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
