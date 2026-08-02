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

// The gate on the local reader build (ADR-0009 / docs-s2): a root-absolute
// reference resolves against the filesystem root under file://, so a single
// surviving `/…` is a dead link in the shipped manual.
//
// This checks the OUTPUT, deliberately not the transform — it would still fail
// if relativize.mjs were removed, reordered out of the build, or silently
// skipped a page. It also asserts the two other things that make the build
// "local": that the entry point exists, and that no search UI shipped without an
// index behind it.
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { htmlFiles } from './relativize.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const distDir = process.argv[2] ?? join(here, '..', 'dist');

if (!existsSync(distDir) || !statSync(distDir).isDirectory()) {
  console.error(`check-local-build: no build at ${distDir}`);
  process.exit(1);
}

const failures = [];

// 1. The reader's entry point.
if (!existsSync(join(distDir, 'index.html'))) {
  failures.push('no index.html at the root of the build — nothing to open');
}

// 2. No root-absolute href/src/srcset anywhere.
const pages = htmlFiles(distDir);
if (pages.length === 0) {
  failures.push('the build contains no HTML pages at all');
}
let absolute = 0;
for (const pageRel of pages) {
  const html = readFileSync(join(distDir, pageRel), 'utf8');
  for (const match of html.matchAll(/\b(href|src|srcset)="([^"]*)"/g)) {
    const [, attr, value] = match;
    const urls = attr === 'srcset' ? value.split(',').map((p) => p.trim().split(/\s+/)[0]) : [value];
    for (const url of urls) {
      // `//host/…` is protocol-relative and genuinely absolute — not our concern.
      if (url.startsWith('/') && !url.startsWith('//')) {
        absolute += 1;
        if (absolute <= 20) failures.push(`${pageRel}: root-absolute ${attr}="${url}"`);
      }
    }
  }
}
if (absolute > 20) {
  failures.push(`…and ${absolute - 20} more root-absolute reference(s)`);
}

// 3. Pagefind is off, so nothing may advertise a search that cannot answer.
if (existsSync(join(distDir, 'pagefind'))) {
  failures.push('a pagefind/ index shipped in the local build — search must be off (file:// cannot fetch it)');
}
for (const pageRel of pages) {
  const html = readFileSync(join(distDir, pageRel), 'utf8');
  if (html.includes('/pagefind/') || html.includes('data-open-modal')) {
    failures.push(`${pageRel}: still carries the Pagefind search UI`);
    break;
  }
}

if (failures.length > 0) {
  console.error(`check-local-build: ${failures.length} problem(s) in ${distDir}`);
  for (const failure of failures) console.error(`  ${failure}`);
  process.exit(1);
}
console.log(
  `check-local-build: ${pages.length} pages, no root-absolute references, search correctly absent`,
);
