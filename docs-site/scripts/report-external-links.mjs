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

// Reports outbound links that look dead (ADR-0009 / docs-s4).
//
// DELIBERATELY NON-BLOCKING. It always exits 0, even when every request fails.
// A third party rearranging their site, or rate-limiting a CI runner, is not a
// reason a contributor's merge cannot happen — and a gate that fails for
// reasons nobody in this repository can fix is a gate people learn to ignore,
// which costs more than the link rot it was meant to catch.
//
// The output is a report to read, not a status to satisfy.
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { htmlFiles } from './relativize.mjs';

const TIMEOUT_MS = 15_000;
const CONCURRENCY = 8;

/** Every distinct external URL in a built tree, with the pages that use it. */
export function collectExternalLinks(dir) {
  const found = new Map();
  for (const pageRel of htmlFiles(dir)) {
    const html = readFileSync(join(dir, pageRel), 'utf8');
    for (const match of html.matchAll(/\bhref="(https?:\/\/[^"]+)"/g)) {
      const url = match[1].split('#')[0];
      if (!found.has(url)) found.set(url, new Set());
      found.get(url).add(pageRel);
    }
  }
  return found;
}

/** HEAD, falling back to GET — a fair number of hosts refuse HEAD outright. */
async function probe(url) {
  for (const method of ['HEAD', 'GET']) {
    try {
      const response = await fetch(url, {
        method,
        redirect: 'follow',
        signal: AbortSignal.timeout(TIMEOUT_MS),
        headers: { 'user-agent': 'RoadMaker-docs-link-report' },
      });
      if (response.ok) return { ok: true, status: response.status };
      if (method === 'GET') return { ok: false, status: String(response.status) };
    } catch (error) {
      if (method === 'GET') return { ok: false, status: error.name ?? 'error' };
    }
  }
  return { ok: false, status: 'unknown' };
}

async function main() {
  const here = dirname(fileURLToPath(import.meta.url));
  const dir = process.argv[2] ?? join(here, '..', 'dist');
  if (!existsSync(dir) || !statSync(dir).isDirectory()) {
    console.log(`external-links: nothing at ${dir} — skipping the report`);
    return;
  }

  const links = [...collectExternalLinks(dir).entries()];
  console.log(`external-links: probing ${links.length} distinct outbound URL(s)`);

  const suspect = [];
  let index = 0;
  const workers = Array.from({ length: Math.min(CONCURRENCY, links.length) }, async () => {
    while (index < links.length) {
      const [url, pages] = links[index++];
      const result = await probe(url);
      if (!result.ok) suspect.push({ url, status: result.status, pages: [...pages] });
    }
  });
  await Promise.all(workers);

  if (suspect.length === 0) {
    console.log('external-links: every outbound link answered');
    return;
  }

  suspect.sort((a, b) => a.url.localeCompare(b.url));
  console.log(`\nexternal-links: ${suspect.length} did not answer (REPORT ONLY — nothing fails):`);
  for (const entry of suspect) {
    console.log(`  [${entry.status}] ${entry.url}`);
    for (const page of entry.pages.slice(0, 3)) console.log(`      on ${page}`);
    if (entry.pages.length > 3) console.log(`      …and ${entry.pages.length - 3} more pages`);
  }

  const summary = process.env.GITHUB_STEP_SUMMARY;
  if (summary) {
    const lines = [
      '### Outbound links that did not answer',
      '',
      'Report only — this does not fail the build. A third party rearranging their',
      'site is not something a merge should wait on.',
      '',
      '| Status | URL | Pages |',
      '|---|---|---|',
      ...suspect.map((e) => `| \`${e.status}\` | ${e.url} | ${e.pages.length} |`),
      '',
    ];
    const { appendFileSync } = await import('node:fs');
    appendFileSync(summary, lines.join('\n'));
  }
}

// Only when run as a script — importing this module (the tests do, for
// collectExternalLinks) must not fire a few dozen network requests.
if (process.argv[1] && join(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await main();
  // Explicit: this script never fails a build.
  process.exitCode = 0;
}
