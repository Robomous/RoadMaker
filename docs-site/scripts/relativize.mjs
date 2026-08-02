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

// Turns the root-absolute references Astro emits into page-relative ones, so the
// local reader opens straight off disk (ADR-0009). `file:///…/manual/index.html`
// has no site root, so `/reference/junction.html` resolves against the
// filesystem root and 404s; `../reference/junction.html` just works.
//
// Deliberately NOT a dependency: the transform is string work over a directory of
// HTML, and every npm package added here has to pass the licence gate forever
// after. See docs-site/README.md.
//
// IDEMPOTENT BY CONSTRUCTION: it only rewrites values that begin with `/`, and
// produces none, so a second run is a no-op. test/relativize.test.mjs proves it
// on the real build output rather than taking the argument's word for it.
//
// A reference that resolves to no file in the build is an ERROR, not a silent
// rewrite — that is what catches a link to a page that was renamed or never
// existed, which is otherwise invisible until a human clicks it.
import { readFileSync, writeFileSync, readdirSync, statSync, existsSync, realpathSync } from 'node:fs';
import { join, relative, dirname, posix } from 'node:path';
import { fileURLToPath } from 'node:url';

/** Every .html file under `dir`, as paths relative to it. */
export function htmlFiles(dir, base = dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) {
      out.push(...htmlFiles(path, base));
    } else if (name.endsWith('.html')) {
      out.push(relative(base, path).split('\\').join('/'));
    }
  }
  return out;
}

/**
 * The file in `distDir` a root-absolute reference points at, as a dist-relative
 * path — or '' when nothing matches.
 *
 * Two shapes have to resolve. Starlight's own navigation already emits the built
 * filename (`/reference/junction.html`), but links written in the guide's
 * Markdown arrive extensionless (`/reference/junction`), because the adapter
 * emits Starlight slugs and Astro passes content links through untouched.
 */
export function resolveTarget(distDir, pathname) {
  const clean = pathname.replace(/\/+$/, '');
  const candidates = clean === '' ? ['index.html'] : [clean, `${clean}.html`, `${clean}/index.html`];
  for (const candidate of candidates) {
    const rel = candidate.replace(/^\//, '');
    const full = join(distDir, rel);
    if (existsSync(full) && statSync(full).isFile()) return rel;
  }
  return '';
}

/**
 * Rewrite one page's root-absolute href/src/srcset values to paths relative to
 * `pageRel`. Returns the new text plus any references that resolved to nothing.
 */
export function relativizePage(distDir, pageRel, html) {
  const fromDir = posix.dirname(pageRel);
  const unresolved = [];

  const rewrite = (value) => {
    const hash = value.indexOf('#');
    const pathname = hash >= 0 ? value.slice(0, hash) : value;
    const suffix = hash >= 0 ? value.slice(hash) : '';
    let decoded;
    try {
      decoded = decodeURI(pathname);
    } catch {
      decoded = pathname;
    }
    const targetRel = resolveTarget(distDir, decoded);
    if (!targetRel) {
      unresolved.push(value);
      return value;
    }
    const rel = fromDir === '.' ? targetRel : posix.relative(fromDir, targetRel);
    return `${rel.startsWith('.') ? rel : `./${rel}`}${suffix}`;
  };

  const text = html.replace(
    /\b(href|src|srcset)="([^"]*)"/g,
    (whole, attr, value) => {
      if (attr === 'srcset') {
        // "a.webp 1x, b.webp 2x" — rewrite each URL, keep each descriptor.
        if (!/(^|,)\s*\//.test(value)) return whole;
        const parts = value.split(',').map((part) => {
          const [url, ...rest] = part.trim().split(/\s+/);
          if (!url.startsWith('/')) return part.trim();
          return [rewrite(url), ...rest].join(' ');
        });
        return `${attr}="${parts.join(', ')}"`;
      }
      if (!value.startsWith('/')) return whole;
      return `${attr}="${rewrite(value)}"`;
    },
  );

  return { text, unresolved };
}

/** Rewrite every page under `distDir` in place. Returns the pages touched. */
export function relativize(distDir) {
  const problems = [];
  let touched = 0;
  for (const pageRel of htmlFiles(distDir)) {
    const full = join(distDir, pageRel);
    const before = readFileSync(full, 'utf8');
    const { text, unresolved } = relativizePage(distDir, pageRel, before);
    for (const value of unresolved) problems.push(`${pageRel}: '${value}' matches no file in the build`);
    if (text !== before) {
      writeFileSync(full, text);
      touched += 1;
    }
  }
  return { touched, problems };
}

// Run as a script; importable for the tests.
const here = fileURLToPath(import.meta.url);
const invoked = process.argv[1] ? realpathSync(process.argv[1]) : '';
if (invoked === realpathSync(here)) {
  const distDir = process.argv[2] ?? join(dirname(here), '..', 'dist');
  if (!existsSync(distDir)) {
    console.error(`relativize: no build at ${distDir} — run the local build first`);
    process.exit(1);
  }
  const { touched, problems } = relativize(distDir);
  if (problems.length > 0) {
    console.error(`relativize: ${problems.length} unresolved reference(s)`);
    for (const problem of problems) console.error(`  ${problem}`);
    process.exit(1);
  }
  console.log(`relativize: rewrote ${touched} page(s) in ${distDir}`);
}
