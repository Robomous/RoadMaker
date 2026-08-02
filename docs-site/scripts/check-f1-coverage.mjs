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

// Every F1-reachable page must also exist in the adapted Starlight set, so a
// page can never be reachable in-app but missing from the site (docs-s1
// acceptance). The C++ side has the mirror gate
// (test_help_registry.cpp EveryPageResolvesToACommittedGuidePage), which checks
// the same slugs against docs/; this one checks them against the site output.
//
// The slugs are read from help_registry.cpp — the same static table the editor
// resolves F1 through — so the two cannot drift.
import { readFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const registry = join(repo, 'editor', 'src', 'help', 'help_registry.cpp');
const adapted = join(here, '..', 'src', 'content', 'docs');

const text = readFileSync(registry, 'utf8');
const slugs = new Set();
for (const m of text.matchAll(/\{ToolId::\w+,\s*"([^"]+)"\}/g)) slugs.add(m[1]);
for (const m of text.matchAll(/\{"[^"]+",\s*"([^"]+)"\}/g)) slugs.add(m[1]);

if (slugs.size === 0) {
  console.error('check-f1-coverage: parsed no slugs from help_registry.cpp — the table shape changed');
  process.exit(1);
}

const missing = [...slugs].filter((slug) => !existsSync(join(adapted, `${slug}.md`)));
if (missing.length > 0) {
  console.error(`check-f1-coverage: ${missing.length} F1 page(s) missing from the site:`);
  for (const slug of missing) console.error(`  ${slug}`);
  console.error('Run the adapter, and check the page is linked from docs/user-guide/index.md.');
  process.exit(1);
}
console.log(`check-f1-coverage: ${slugs.size} F1 slugs all present in the adapted set`);
