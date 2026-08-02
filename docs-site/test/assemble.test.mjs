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
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import {
  assemble,
  buildVersionsManifest,
  existingSegments,
  pagesIn,
  semverKey,
  sortReleases,
} from '../scripts/assemble.mjs';

/** A throwaway built site with a couple of pages. */
function makeBuild(label) {
  const dir = mkdtempSync(join(tmpdir(), 'rm-build-'));
  writeFileSync(join(dir, 'index.html'), `<p>${label}</p>`);
  mkdirSync(join(dir, 'reference'), { recursive: true });
  writeFileSync(join(dir, 'reference', 'junction.html'), `<p>${label} junction</p>`);
  return dir;
}

function makeRoot() {
  return mkdtempSync(join(tmpdir(), 'rm-publish-'));
}

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

// ------------------------------------------------------------------- semver

test('latest is the highest semver, not the most recent tag', () => {
  // The whole point: v0.10.0 comes AFTER v0.9.0, and a patch on an old line
  // (v0.9.1, tagged later) must not become `latest`.
  assert.deepEqual(sortReleases(['v0.9.0', 'v0.10.0', 'v0.9.1', 'v1.0.0']), [
    'v1.0.0',
    'v0.10.0',
    'v0.9.1',
    'v0.9.0',
  ]);
  assert.equal(semverKey('dev'), null);
  assert.deepEqual(semverKey('v1.2.3'), [1, 2, 3]);
});

// ------------------------------------------------------- today: dev alone

test('with only dev, the tree stands up and the root points at it', () => {
  const root = makeRoot();
  const build = makeBuild('dev');
  try {
    const { manifest } = assemble({ publishRoot: root, buildDir: build, segment: 'dev' });

    assert.equal(manifest.latest, null, 'no release exists yet');
    assert.equal(manifest.default, 'dev');
    assert.equal(manifest.versions.length, 1);
    assert.ok(existsSync(join(root, 'dev', 'index.html')));
    assert.ok(!existsSync(join(root, 'latest')), 'latest cannot exist without a release');

    const redirect = readFileSync(join(root, 'index.html'), 'utf8');
    assert.match(redirect, /url=\.\/dev\//);
    assert.match(redirect, /<a href="\.\/dev\/">/, 'a no-JS reader still gets a link');
  } finally {
    rmSync(root, { recursive: true, force: true });
    rmSync(build, { recursive: true, force: true });
  }
});

// ------------------------------------------------------------- a release

test('a release recomputes latest and repoints the root', () => {
  const root = makeRoot();
  const dev = makeBuild('dev');
  const rel = makeBuild('v0.1.0');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    const { manifest } = assemble({ publishRoot: root, buildDir: rel, segment: 'v0.1.0' });

    assert.equal(manifest.latest, 'v0.1.0');
    assert.equal(manifest.default, 'v0.1.0');
    assert.equal(
      readFileSync(join(root, 'latest', 'index.html'), 'utf8'),
      readFileSync(join(root, 'v0.1.0', 'index.html'), 'utf8'),
      'latest is a copy of the highest release',
    );
    assert.match(readFileSync(join(root, 'index.html'), 'utf8'), /url=\.\/v0\.1\.0\//);
    // dev survived a release being published.
    assert.equal(readFileSync(join(root, 'dev', 'index.html'), 'utf8'), '<p>dev</p>');
  } finally {
    for (const d of [root, dev, rel]) rmSync(d, { recursive: true, force: true });
  }
});

test('rebuilding one version disturbs no other', () => {
  const root = makeRoot();
  const v1 = makeBuild('v0.1.0');
  const v2 = makeBuild('v0.2.0');
  const dev1 = makeBuild('dev-old');
  const dev2 = makeBuild('dev-new');
  try {
    assemble({ publishRoot: root, buildDir: v1, segment: 'v0.1.0' });
    assemble({ publishRoot: root, buildDir: v2, segment: 'v0.2.0' });
    assemble({ publishRoot: root, buildDir: dev1, segment: 'dev' });

    const { manifest } = assemble({ publishRoot: root, buildDir: dev2, segment: 'dev' });

    assert.equal(readFileSync(join(root, 'dev', 'index.html'), 'utf8'), '<p>dev-new</p>');
    assert.equal(readFileSync(join(root, 'v0.1.0', 'index.html'), 'utf8'), '<p>v0.1.0</p>');
    assert.equal(readFileSync(join(root, 'v0.2.0', 'index.html'), 'utf8'), '<p>v0.2.0</p>');
    assert.equal(manifest.latest, 'v0.2.0', 'a dev rebuild must not move latest');
    assert.deepEqual(
      manifest.versions.map((v) => v.id),
      ['v0.2.0', 'v0.1.0', 'dev'],
      'releases newest-first, dev last',
    );
  } finally {
    for (const d of [root, v1, v2, dev1, dev2]) rmSync(d, { recursive: true, force: true });
  }
});

test('a version directory this run knows nothing about survives', () => {
  // The assembler is handed one build; everything else it must discover from
  // disk. A version published by an earlier run is exactly that case.
  const root = makeRoot();
  const dev = makeBuild('dev');
  try {
    mkdirSync(join(root, 'v9.9.9'), { recursive: true });
    writeFileSync(join(root, 'v9.9.9', 'index.html'), '<p>ancient</p>');

    const { manifest } = assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });

    assert.equal(readFileSync(join(root, 'v9.9.9', 'index.html'), 'utf8'), '<p>ancient</p>');
    assert.equal(manifest.latest, 'v9.9.9');
  } finally {
    rmSync(root, { recursive: true, force: true });
    rmSync(dev, { recursive: true, force: true });
  }
});

// ------------------------------------------------------------- idempotence

test('assembling the same inputs twice changes nothing the second time', () => {
  const root = makeRoot();
  const dev = makeBuild('dev');
  const rel = makeBuild('v0.1.0');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    assemble({ publishRoot: root, buildDir: rel, segment: 'v0.1.0' });
    const before = {
      versions: readFileSync(join(root, 'versions.json'), 'utf8'),
      index: readFileSync(join(root, 'index.html'), 'utf8'),
    };

    const second = assemble({ publishRoot: root, buildDir: rel, segment: 'v0.1.0' });

    assert.equal(readFileSync(join(root, 'versions.json'), 'utf8'), before.versions);
    assert.equal(readFileSync(join(root, 'index.html'), 'utf8'), before.index);
    assert.equal(
      second.changed.includes('versions.json'),
      false,
      'versions.json must not be rewritten when it would be byte-identical',
    );
  } finally {
    for (const d of [root, dev, rel]) rmSync(d, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------- the dry run

test('a dry run writes only inside its scratch prefix', () => {
  const root = makeRoot();
  const dev = makeBuild('dev');
  const candidate = makeBuild('v9.9.9-candidate');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    const realBefore = {
      versions: readFileSync(join(root, 'versions.json'), 'utf8'),
      index: readFileSync(join(root, 'index.html'), 'utf8'),
      dev: readFileSync(join(root, 'dev', 'index.html'), 'utf8'),
    };

    const { manifest } = assemble({
      publishRoot: root,
      buildDir: candidate,
      segment: 'v9.9.9',
      scratch: '_dryrun',
    });

    // The dry run produced a complete tree of its own...
    assert.ok(existsSync(join(root, '_dryrun', 'v9.9.9', 'index.html')));
    assert.ok(existsSync(join(root, '_dryrun', 'latest', 'index.html')));
    assert.ok(existsSync(join(root, '_dryrun', 'versions.json')));
    assert.equal(manifest.latest, 'v9.9.9');
    assert.equal(manifest.root, '/_dryrun/');

    // ...and touched nothing real.
    assert.equal(readFileSync(join(root, 'versions.json'), 'utf8'), realBefore.versions);
    assert.equal(readFileSync(join(root, 'index.html'), 'utf8'), realBefore.index);
    assert.equal(readFileSync(join(root, 'dev', 'index.html'), 'utf8'), realBefore.dev);
    assert.ok(!existsSync(join(root, 'latest')), 'the real tree still has no release');
    assert.ok(!existsSync(join(root, 'v9.9.9')), 'the candidate is not in the real tree');
  } finally {
    for (const d of [root, dev, candidate]) rmSync(d, { recursive: true, force: true });
  }
});

test('the scratch tree is not mistaken for a version', () => {
  const root = makeRoot();
  const dev = makeBuild('dev');
  const candidate = makeBuild('v9.9.9');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    assemble({ publishRoot: root, buildDir: candidate, segment: 'v9.9.9', scratch: '_dryrun' });

    // Re-assembling the real tree must not adopt _dryrun's contents.
    const { manifest } = assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    assert.deepEqual(manifest.versions.map((v) => v.id), ['dev']);
    assert.equal(manifest.latest, null);
    assert.deepEqual(existingSegments(root), ['dev']);
  } finally {
    for (const d of [root, dev, candidate]) rmSync(d, { recursive: true, force: true });
  }
});

// ------------------------------------------------------------------ refusals

test('a segment that is neither dev nor a release is refused', () => {
  const root = makeRoot();
  const build = makeBuild('x');
  try {
    for (const segment of ['latest', '../escape', 'main', 'v1.2', 'versions.json', '']) {
      assert.throws(
        () => assemble({ publishRoot: root, buildDir: build, segment }),
        /refusing to publish segment/,
        `segment '${segment}' must be refused`,
      );
    }
  } finally {
    rmSync(root, { recursive: true, force: true });
    rmSync(build, { recursive: true, force: true });
  }
});

test('a directory that is not a built site is refused', () => {
  const root = makeRoot();
  const empty = mkdtempSync(join(tmpdir(), 'rm-empty-'));
  try {
    assert.throws(
      () => assemble({ publishRoot: root, buildDir: empty, segment: 'dev' }),
      /that is not a built site/,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
    rmSync(empty, { recursive: true, force: true });
  }
});

// ------------------------------------------------------------- the manifest

test('every version carries the page list the dropdown needs', () => {
  const root = makeRoot();
  const dev = makeBuild('dev');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    const manifest = readJson(join(root, 'versions.json'));
    const entry = manifest.versions.find((v) => v.id === 'dev');
    assert.deepEqual(entry.pages, ['index.html', 'reference/junction.html']);
    assert.equal(entry.release, false);
  } finally {
    rmSync(root, { recursive: true, force: true });
    rmSync(dev, { recursive: true, force: true });
  }
});

test('pagesIn lists html only, sorted, recursively', () => {
  const dir = makeBuild('x');
  try {
    writeFileSync(join(dir, 'style.css'), 'body{}');
    assert.deepEqual(pagesIn(dir), ['index.html', 'reference/junction.html']);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('buildVersionsManifest reports the root path it was given', () => {
  const root = makeRoot();
  const dev = makeBuild('dev');
  try {
    assemble({ publishRoot: root, buildDir: dev, segment: 'dev' });
    assert.equal(buildVersionsManifest(root, '/').root, '/');
  } finally {
    rmSync(root, { recursive: true, force: true });
    rmSync(dev, { recursive: true, force: true });
  }
});
