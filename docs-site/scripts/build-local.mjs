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

// `npm run build:local` — the offline reader that ships inside a release
// (ADR-0009 / docs-s2). One script rather than a chain of npm scripts for two
// reasons: setting an environment variable inside an npm script is not portable
// without a dependency, and the post-processing step MUST NOT be skippable —
// a build whose links were never relativized looks perfectly fine until someone
// opens it from a disc.
//
// The release packaging job runs this and hands the resulting dist/ to CMake as
// a path; CMake never invokes npm.
import { spawnSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const env = { ...process.env, RM_DOCS_TARGET: 'local' };

/** Run a step; a non-zero exit stops the build with that step's status. */
function step(label, command, args) {
  console.log(`\nbuild:local — ${label}`);
  const result = spawnSync(command, args, { cwd: root, env, stdio: 'inherit', shell: false });
  if (result.error) {
    console.error(`build:local: ${label} could not start: ${result.error.message}`);
    process.exit(1);
  }
  if (result.status !== 0) {
    console.error(`build:local: ${label} failed (exit ${result.status})`);
    process.exit(result.status ?? 1);
  }
}

const node = process.execPath;
const astro = join(root, 'node_modules', 'astro', 'astro.js');

step('theme tokens', node, [join(here, 'theme-css.mjs')]);
step('adapt docs/user-guide', node, [join(here, 'adapt.mjs')]);
step('F1 coverage', node, [join(here, 'check-f1-coverage.mjs')]);
step('astro build (file format, search off)', node, [astro, 'build']);
step('relativize references', node, [join(here, 'relativize.mjs')]);

// See build-web.mjs: the checker runs in its own shell and must not have to
// guess. The offline reader's references are relative, so its base is `/`.
writeFileSync(
  join(root, 'dist', '.rm-docs-build.json'),
  `${JSON.stringify({ target: 'local', base: '/' }, null, 2)}\n`,
);

step('verify the local build', node, [join(here, 'check-local-build.mjs')]);
step('internal links', node, [join(here, 'check-links.mjs')]);

console.log('\nbuild:local: dist/ is ready to open from file://');
