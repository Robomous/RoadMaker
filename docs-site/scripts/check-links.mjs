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

// Internal link and image check over a BUILT tree (ADR-0009 / docs-s4).
//
// Run it on one build, or on the whole assembled publish tree — in which case
// it checks `dev/` and every version directory, each against its own base, and
// the root redirect and manifest too. That is the point: the adapter's check
// runs on source, and a per-build check cannot see a cross-version link or a
// version directory that was published months ago and is still being served.
//
// EXTERNAL LINKS ARE NOT FETCHED HERE. Link rot in somebody else's URL is not a
// reason a merge cannot happen; report-external-links.mjs reports on those
// without failing anything.
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, posix, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { htmlFiles } from './relativize.mjs';

/** Name of the stamp each build drops so its base is knowable afterwards. */
export const STAMP = '.rm-docs-build.json';

/**
 * The base a built directory was produced with.
 *
 * Read from the stamp the build writes, NOT from the environment: this script
 * runs in its own shell in CI and in a contributor's terminal, where whatever
 * `RM_DOCS_BASE` the build used is long gone. Inferring it from the output
 * would be guesswork, and guessing wrong turns every link into a false
 * positive.
 */
export function baseOf(dir, fallback = '/') {
  const stamp = join(dir, STAMP);
  if (existsSync(stamp)) {
    try {
      const parsed = JSON.parse(readFileSync(stamp, 'utf8'));
      if (typeof parsed.base === 'string' && parsed.base.startsWith('/')) return parsed.base;
    } catch {
      // A corrupt stamp is not worth failing over; fall through.
    }
  }
  return fallback;
}

/** Roots to check: each version directory of a publish tree, or the build itself. */
export function findRoots(dir) {
  const manifest = join(dir, 'versions.json');
  if (existsSync(manifest)) {
    const parsed = JSON.parse(readFileSync(manifest, 'utf8'));
    const roots = (parsed.versions ?? [])
      .map((version) => version.path)
      .filter((path) => existsSync(join(dir, path)))
      .map((path) => ({
        label: path,
        dir: join(dir, path),
        base: baseOf(join(dir, path), `/${path}/`),
      }));
    // `latest/` is a byte copy of the highest version, so its pages — and its
    // stamp — carry THAT version's base, not `/latest/`. Checking it against
    // `/latest/` would report every link in it as broken.
    //
    // The consequence is worth knowing: a reader who opens `/latest/` is moved
    // to the pinned version's URL on their first click. Every link resolves —
    // `/latest/` is an entry point, not a browsable mirror.
    if (existsSync(join(dir, 'latest'))) {
      const fallback = parsed.latest ? `/${parsed.latest}/` : '/latest/';
      roots.push({
        label: 'latest',
        dir: join(dir, 'latest'),
        base: baseOf(join(dir, 'latest'), fallback),
      });
    }
    return roots;
  }
  return [{ label: '.', dir, base: baseOf(dir, process.env.RM_DOCS_BASE ?? '/') }];
}

/**
 * Resolve one reference against a page.
 *
 * Handles all three link shapes the two builds produce: relative (the offline
 * reader), root-absolute under a base (the published site), and the
 * extensionless slugs the adapter emits for content links.
 */
export function resolveRef(root, base, pageRel, value) {
  // Off-tree schemes answer for themselves. The caller filters these too, to
  // keep them out of the reference count — but this function is exported, and
  // one that silently called every external URL "broken" would be a trap.
  if (/^(https?:|mailto:|tel:|data:|javascript:|rmmanual:)/i.test(value)) {
    return { kind: 'external' };
  }
  const withoutHash = value.split('#')[0].split('?')[0];
  if (withoutHash === '') return { kind: 'in-page' };

  let target;
  if (withoutHash.startsWith('/')) {
    if (withoutHash.startsWith('//')) return { kind: 'external' };
    if (!withoutHash.startsWith(base)) {
      return { kind: 'broken', reason: `outside the base ${base}` };
    }
    target = withoutHash.slice(base.length);
  } else {
    target = posix.normalize(posix.join(posix.dirname(pageRel), withoutHash));
    if (target.startsWith('..')) {
      return { kind: 'broken', reason: 'escapes the version root' };
    }
  }

  const clean = target.replace(/\/+$/, '');
  const candidates =
    clean === '' ? ['index.html'] : [clean, `${clean}.html`, `${clean}/index.html`];
  for (const candidate of candidates) {
    const full = join(root, candidate);
    if (existsSync(full) && statSync(full).isFile()) return { kind: 'ok', file: candidate };
  }
  return { kind: 'broken', reason: 'matches no file' };
}

/** Check one built root. Returns the problems found. */
export function checkRoot({ label, dir, base }) {
  const problems = [];
  let refs = 0;
  const pages = htmlFiles(dir);
  if (pages.length === 0) {
    problems.push(`${label}: contains no HTML pages at all`);
    return { problems, pages: 0, refs };
  }

  for (const pageRel of pages) {
    const html = readFileSync(join(dir, pageRel), 'utf8');
    for (const match of html.matchAll(/\b(href|src|srcset)="([^"]*)"/g)) {
      const [, attr, value] = match;
      const urls =
        attr === 'srcset' ? value.split(',').map((p) => p.trim().split(/\s+/)[0]) : [value];
      for (const url of urls) {
        if (/^(https?:|mailto:|tel:|data:|javascript:|rmmanual:)/i.test(url)) continue;
        refs += 1;
        const outcome = resolveRef(dir, base, pageRel, url);
        if (outcome.kind === 'broken') {
          problems.push(`${label}/${pageRel}: ${attr}="${url}" — ${outcome.reason}`);
        }
      }
    }
  }
  return { problems, pages: pages.length, refs };
}

/** The publish tree's own derived files, which no page links but readers hit. */
export function checkPublishRoot(dir) {
  const problems = [];
  const manifest = join(dir, 'versions.json');
  if (!existsSync(manifest)) return problems;

  const parsed = JSON.parse(readFileSync(manifest, 'utf8'));
  for (const version of parsed.versions ?? []) {
    if (!existsSync(join(dir, version.path, 'index.html'))) {
      problems.push(`versions.json lists '${version.path}', which has no index.html`);
    }
  }
  if (parsed.latest && !existsSync(join(dir, 'latest', 'index.html'))) {
    problems.push(`versions.json names latest='${parsed.latest}' but latest/ has no index.html`);
  }

  const rootIndex = join(dir, 'index.html');
  if (!existsSync(rootIndex)) {
    problems.push('the publish root has no index.html — nothing to redirect a visitor');
  } else if (parsed.default) {
    const html = readFileSync(rootIndex, 'utf8');
    if (!html.includes(`./${parsed.default}/`)) {
      problems.push(
        `the root redirect does not point at '${parsed.default}/', which versions.json calls the default`,
      );
    }
  }
  return problems;
}

// ------------------------------------------------------------------ as a script

const here = dirname(fileURLToPath(import.meta.url));
if (process.argv[1] && join(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const dir = process.argv[2] ?? join(here, '..', 'dist');
  if (!existsSync(dir) || !statSync(dir).isDirectory()) {
    console.error(`check-links: nothing at ${dir}`);
    process.exit(1);
  }

  const roots = findRoots(dir);
  const problems = [...checkPublishRoot(dir)];
  let pages = 0;
  let refs = 0;
  for (const root of roots) {
    const result = checkRoot(root);
    problems.push(...result.problems);
    pages += result.pages;
    refs += result.refs;
  }

  if (problems.length > 0) {
    console.error(`check-links: ${problems.length} broken reference(s)`);
    for (const problem of problems.slice(0, 50)) console.error(`  ${problem}`);
    if (problems.length > 50) console.error(`  …and ${problems.length - 50} more`);
    process.exit(1);
  }
  console.log(
    `check-links: ${roots.length} root(s) [${roots.map((r) => r.label).join(', ')}], ` +
      `${pages} pages, ${refs} internal references, all resolve`,
  );
}
