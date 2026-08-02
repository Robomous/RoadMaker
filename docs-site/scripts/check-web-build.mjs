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

// The gate on a versioned web build (docs-s3): every root-absolute reference
// must carry the version segment.
//
// This exists because the failure it catches is close to invisible. Astro
// prefixes the links IT generates, so the sidebar and the nav are correct and
// the site looks fine; only the links written in the guide's own Markdown are
// left bare, and those 404 the moment a reader follows one from inside a page.
// A build under `/dev/` that emits `/reference/junction` is broken in exactly
// the places a spot check does not look.
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { htmlFiles } from './relativize.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const distDir = process.argv[2] ?? join(here, '..', 'dist');
const base = process.env.RM_DOCS_BASE ?? '/';

if (!existsSync(distDir) || !statSync(distDir).isDirectory()) {
  console.error(`check-web-build: no build at ${distDir}`);
  process.exit(1);
}

const pages = htmlFiles(distDir);
if (pages.length === 0) {
  console.error('check-web-build: the build contains no HTML pages at all');
  process.exit(1);
}

const failures = [];
let checked = 0;

for (const pageRel of pages) {
  const html = readFileSync(join(distDir, pageRel), 'utf8');
  for (const match of html.matchAll(/\b(href|src|srcset)="([^"]*)"/g)) {
    const [, attr, value] = match;
    const urls = attr === 'srcset' ? value.split(',').map((p) => p.trim().split(/\s+/)[0]) : [value];
    for (const url of urls) {
      // `//host/…` is protocol-relative, i.e. genuinely off-site.
      if (!url.startsWith('/') || url.startsWith('//')) continue;
      checked += 1;
      if (!url.startsWith(base)) {
        if (failures.length < 20) failures.push(`${pageRel}: ${attr}="${url}" is missing the ${base} prefix`);
        else if (failures.length === 20) failures.push('…and more');
      }
    }
  }
}

if (failures.length > 0) {
  console.error(`check-web-build: root-absolute references outside ${base}`);
  for (const failure of failures) console.error(`  ${failure}`);
  process.exit(1);
}
console.log(`check-web-build: ${pages.length} pages, ${checked} absolute references, all under ${base}`);
