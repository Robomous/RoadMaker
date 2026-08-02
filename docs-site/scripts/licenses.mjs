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

// Licence gate for the site's npm tree (ADR-0009 / docs/standards/dependencies.md).
//
// Reads every installed package's own package.json — the authority, not the
// lockfile, which does not record licences — and fails non-zero on anything
// outside the permitted set. Run in CI so a transitive dependency cannot
// introduce a copyleft licence between audits.
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const ALLOWED = new Set([
  'MIT', 'ISC', 'Apache-2.0', 'BSD-2-Clause', 'BSD-3-Clause', '0BSD',
  'BlueOak-1.0.0', 'CC0-1.0', 'Unlicense', 'MIT-0', 'Python-2.0', 'MPL-2.0',
]);

/** SPDX expressions we accept when every alternative in an OR is allowed. */
function allowed(expr) {
  if (!expr) return false;
  const cleaned = expr.replace(/[()]/g, ' ');
  if (ALLOWED.has(cleaned.trim())) return true;
  if (/\sOR\s/i.test(cleaned)) {
    return cleaned.split(/\sOR\s/i).some((part) => ALLOWED.has(part.trim()));
  }
  if (/\sAND\s/i.test(cleaned)) {
    return cleaned.split(/\sAND\s/i).every((part) => ALLOWED.has(part.trim()));
  }
  return false;
}

/** SPDX id inferred from a package's LICENSE file when package.json omits one. */
function licenseFromFile(dir) {
  for (const name of ['LICENSE', 'LICENSE.md', 'LICENSE.txt', 'license', 'LICENCE']) {
    const path = join(dir, name);
    if (!existsSync(path)) continue;
    const head = readFileSync(path, 'utf8').slice(0, 400);
    if (/MIT License/i.test(head)) return 'MIT';
    if (/Apache License/i.test(head)) return 'Apache-2.0';
    if (/ISC License/i.test(head)) return 'ISC';
    if (/BSD 3-Clause/i.test(head)) return 'BSD-3-Clause';
    if (/BSD 2-Clause/i.test(head)) return 'BSD-2-Clause';
  }
  return '';
}

function* walk(dir) {
  if (!existsSync(dir)) return;
  for (const name of readdirSync(dir)) {
    if (name === '.bin') continue;
    const path = join(dir, name);
    if (name.startsWith('@')) {
      yield* walk(path);
      continue;
    }
    if (existsSync(join(path, 'package.json'))) yield path;
    yield* walk(join(path, 'node_modules'));
  }
}

const rows = [];
const offenders = [];
for (const dir of walk('node_modules')) {
  let pkg;
  try {
    pkg = JSON.parse(readFileSync(join(dir, 'package.json'), 'utf8'));
  } catch {
    continue;
  }
  if (!pkg.name || !pkg.version) continue;
  let license =
    typeof pkg.license === 'string'
      ? pkg.license
      : pkg.license?.type ?? (Array.isArray(pkg.licenses) ? pkg.licenses.map((l) => l.type).join(' OR ') : '');
  // Some packages ship a LICENSE file and declare nothing in package.json
  // (zod-to-ts, at the time of writing). The file is still the licence grant,
  // so read it rather than failing a package that IS permissive.
  if (!license) license = licenseFromFile(dir);
  rows.push({ name: pkg.name, version: pkg.version, license });
  if (!allowed(license)) offenders.push({ name: pkg.name, version: pkg.version, license });
}

rows.sort((a, b) => a.name.localeCompare(b.name) || a.version.localeCompare(b.version));
const counts = new Map();
for (const r of rows) counts.set(r.license, (counts.get(r.license) ?? 0) + 1);

if (process.argv.includes('--markdown')) {
  console.log('| Package | Version | Licence |');
  console.log('|---|---|---|');
  for (const r of rows) console.log(`| \`${r.name}\` | ${r.version} | ${r.license} |`);
} else {
  console.log(`${rows.length} packages`);
  for (const [license, n] of [...counts].sort((a, b) => b[1] - a[1])) {
    console.log(`  ${String(n).padStart(4)}  ${license}`);
  }
}

if (offenders.length > 0) {
  console.error(`\n${offenders.length} package(s) outside the permitted licence set:`);
  for (const o of offenders) console.error(`  ${o.name}@${o.version}: ${o.license || '(none declared)'}`);
  process.exit(1);
}
