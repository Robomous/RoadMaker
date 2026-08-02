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

// Adapts docs/user-guide/** into Starlight's content directory (ADR-0009).
//
// The authored source stays plain Markdown that BOTH renderers accept; this
// script does the Starlight-specific part:
//   - synthesizes the `title` frontmatter Starlight requires, from each page's
//     first H1 (the same rule helpc::build_toc uses, so the two pipelines
//     cannot disagree about a page's title);
//   - derives reference-tier sidebar order from index.md — THE SAME MANIFEST
//     the .qhp reads, which is what stops the two outputs drifting;
//   - rewrites links and image references for Starlight's routing;
//   - FAILS LOUDLY on a broken link, naming source page and target.
//
// Output is gitignored and must never be hand-edited.
import { readFileSync, writeFileSync, mkdirSync, cpSync, rmSync, existsSync, readdirSync, statSync } from 'node:fs';
import { basename, dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(join(here, '..', '..'));
const guide = join(repo, 'docs', 'user-guide');
const outDir = join(here, '..', 'src', 'content', 'docs');

const REPO_BLOB = 'https://github.com/Robomous/RoadMaker/blob/main';
const WEB_DOCS = 'https://github.com/Robomous/RoadMaker/tree/main/docs/user-guide';

/// `local` builds the offline reader that ships in a release (ADR-0009): it opens
/// from file://, so Pagefind cannot index it and the search UI is switched off.
/// Everything else about the two builds is identical.
const target = process.env.RM_DOCS_TARGET === 'local' ? 'local' : 'web';

/// The version segment the site is published under (docs-s3), e.g. `/dev/`.
///
/// Astro applies `base` to the links IT generates — the sidebar, the nav, asset
/// URLs — but a link written in the guide's Markdown is content, and content is
/// passed through untouched. So the prefix has to be applied HERE, or every
/// in-content cross-page link 404s on the published site while the sidebar works
/// perfectly, which is the sort of breakage nobody notices from the front page.
const base = target === 'local' ? '/' : (process.env.RM_DOCS_BASE ?? '/');

/// Said once, on the landing page, so a reader who reaches for search learns
/// where it lives instead of finding a box that does nothing.
const LOCAL_SEARCH_NOTE = [
  ':::note[Offline copy]',
  'This is the manual bundled with your copy of RoadMaker, opened straight from',
  `disk. Full-text search needs a web server, so it is available on the [online`,
  `documentation](${WEB_DOCS}) instead. Every page is here; only the search box`,
  'is missing.',
  ':::',
  '',
].join('\n');

const errors = [];

/** The page's first `# ` heading — helpc::first_h1's rule. */
function firstH1(markdown, rel) {
  for (const line of markdown.split('\n')) {
    if (line.startsWith('# ')) return line.slice(2).trim();
  }
  errors.push(`${rel}: no '# ' H1, so Starlight has no title to use`);
  return rel;
}

function walkMarkdown(dir, base = dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) {
      if (name === 'img') continue;
      out.push(...walkMarkdown(path, base));
    } else if (name.endsWith('.md')) {
      out.push(relative(base, path).split('\\').join('/'));
    }
  }
  return out;
}

/** Reference-tier order, read from index.md — the .qhp's manifest. */
function referenceOrder() {
  const index = readFileSync(join(guide, 'index.md'), 'utf8');
  const order = [];
  for (const m of index.matchAll(/\]\((reference\/[a-z0-9-]+\.md)(?:#[^)]*)?\)/g)) {
    if (!order.includes(m[1])) order.push(m[1]);
  }
  return order;
}

const pages = walkMarkdown(guide);
const pageSet = new Set(pages);
const order = referenceOrder();

rmSync(outDir, { recursive: true, force: true });
mkdirSync(outDir, { recursive: true });

for (const rel of pages) {
  const srcPath = join(guide, rel);
  let body = readFileSync(srcPath, 'utf8');
  const title = firstH1(body, rel);

  // Drop the H1: Starlight renders the frontmatter title as the page heading,
  // so keeping it would show the title twice.
  body = body.replace(/^#\s+.*\n/m, '');

  body = body.replace(/\]\(([^)]+)\)/g, (whole, target) => {
    if (/^(https?:|mailto:|#)/.test(target)) return whole;
    const [path, anchor = ''] = target.split(/(#.*)/);
    if (!path) return whole;

    // `../` means "up one directory" — NOT "out of the guide". Since docs-s1 the
    // guide has subdirectories, so `reference/x.md` -> `../tutorials/y.md` lands
    // back INSIDE it and is an ordinary in-guide link. Resolve first, then decide.
    const absolute = resolve(dirname(srcPath), path);
    const insideGuide = !relative(guide, absolute).startsWith('..');

    // Genuinely leaves the guide -> the repo on GitHub, as the Qt renderer does.
    if (path.startsWith('../') && !insideGuide) {
      const resolved = relative(repo, absolute).split('\\').join('/');
      if (!existsSync(absolute)) {
        errors.push(`${rel}: broken link to '${target}' (resolved to ${resolved})`);
        return whole;
      }
      // An out-of-guide IMAGE is copied in and referenced locally — exactly what
      // helpc::rewrite_target does for the Qt pipeline, so both renderers show
      // the same picture rather than one 404ing.
      if (/\.(png|gif|jpe?g|svg)$/i.test(path)) {
        const name = basename(absolute);
        const destDir = join(outDir, dirname(rel), 'img');
        mkdirSync(destDir, { recursive: true });
        cpSync(absolute, join(destDir, name));
        return `](img/${name}${anchor})`;
      }
      return `](${REPO_BLOB}/${resolved}${anchor})`;
    }

    if (path.endsWith('.md')) {
      const target_rel = relative(guide, absolute).split('\\').join('/');
      if (!pageSet.has(target_rel)) {
        errors.push(`${rel}: broken link to '${target}' (no page ${target_rel})`);
      }
      // Starlight routes on the slug: drop the extension, index -> the root.
      const slug = target_rel.replace(/\.md$/, '').replace(/(^|\/)index$/, '');
      return `](${base}${slug}${anchor})`;
    }
    return whole;
  });

  // In-guide images keep their relative reference; the img/ folders are copied
  // alongside the pages below. Out-of-guide ones were copied in above.
  for (const m of body.matchAll(/!\[[^\]]*\]\((?!https?:|img\/)([^)]+)\)/g)) {
    const resolved = resolve(dirname(srcPath), m[1].split('#')[0]);
    if (!existsSync(resolved)) errors.push(`${rel}: missing image '${m[1]}'`);
  }

  const position = order.indexOf(rel);
  const frontmatter = [
    '---',
    `title: ${JSON.stringify(title)}`,
    ...(position >= 0 ? ['sidebar:', `  order: ${position + 1}`] : []),
    '---',
    '',
  ].join('\n');

  const note = target === 'local' && rel === 'index.md' ? LOCAL_SEARCH_NOTE : '';

  const dest = join(outDir, rel);
  mkdirSync(dirname(dest), { recursive: true });
  writeFileSync(dest, frontmatter + note + body.trimStart());
}

// Image folders ride along so the pages' relative srcs resolve.
for (const rel of ['reference/img', 'tutorials/img']) {
  const src = join(guide, rel);
  if (existsSync(src)) cpSync(src, join(outDir, rel), { recursive: true });
}

// The tab icon, taken from the app's own icon set rather than drawn again, so
// the site and the editor cannot show different marks. Starlight links a favicon
// unconditionally; without the file the reference dangles, which is invisible on
// a server (a 404 in the console) and a real broken reference under file://.
const publicDir = join(here, '..', 'public');
mkdirSync(publicDir, { recursive: true });
cpSync(join(repo, 'editor', 'resources', 'branding', 'icon_64.png'), join(publicDir, 'favicon.png'));

if (errors.length > 0) {
  console.error(`adapt: ${errors.length} problem(s)`);
  for (const e of errors) console.error(`  ${e}`);
  process.exit(1);
}
console.log(
  `adapt: ${pages.length} pages, ${order.length} ordered from index.md (${target} build)`,
);
