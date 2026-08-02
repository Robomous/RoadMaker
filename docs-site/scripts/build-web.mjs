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

// `npm run build:web -- --base=/dev/` — the published site, for one version
// segment (ADR-0009 / docs-s3). Search on, directory URLs, and every link
// prefixed with the segment.
//
// The base has to reach BOTH the Astro config (which prefixes the links Astro
// generates) and the adapter (which prefixes the links written in the guide's
// Markdown — content Astro passes through untouched). Passing it through one
// environment variable is what keeps those two from disagreeing.
import { spawnSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

const raw =
  process.argv.find((a) => a.startsWith('--base='))?.slice('--base='.length) ??
  process.env.RM_DOCS_BASE ??
  '/';
// Normalise to leading + trailing slash so the two consumers compose it by
// plain concatenation and cannot differ over a missing separator.
const base = `/${raw.replace(/^\/+|\/+$/g, '')}/`.replace('//', '/');

const env = { ...process.env, RM_DOCS_TARGET: 'web', RM_DOCS_BASE: base };

function step(label, args) {
  console.log(`\nbuild:web (${base}) — ${label}`);
  const result = spawnSync(process.execPath, args, {
    cwd: root,
    env,
    stdio: 'inherit',
    shell: false,
  });
  if (result.error) {
    console.error(`build:web: ${label} could not start: ${result.error.message}`);
    process.exit(1);
  }
  if (result.status !== 0) {
    console.error(`build:web: ${label} failed (exit ${result.status})`);
    process.exit(result.status ?? 1);
  }
}

step('theme tokens', [join(here, 'theme-css.mjs')]);
step('adapt docs/user-guide', [join(here, 'adapt.mjs')]);
step('F1 coverage', [join(here, 'check-f1-coverage.mjs')]);
step('astro build', [join(root, 'node_modules', 'astro', 'astro.js'), 'build']);

// Record the base in the output. check-links.mjs runs in its own shell — in CI
// and on a contributor's machine — where this environment variable no longer
// exists, and a link checker that assumes the wrong base reports every link in
// the build as broken. The stamp rides along into the published tree, so a
// version directory (and the `latest/` copy of one) stays self-describing.
writeFileSync(
  join(root, 'dist', '.rm-docs-build.json'),
  `${JSON.stringify({ target: 'web', base }, null, 2)}\n`,
);

step('verify the base', [join(here, 'check-web-build.mjs')]);
step('internal links', [join(here, 'check-links.mjs')]);

console.log(`\nbuild:web: dist/ is ready to publish under ${base}`);
