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

// Assembles the published documentation tree (ADR-0009 / docs-s3):
//
//     /                 redirect to latest/, or dev/ while no release exists
//     /versions.json    what the header dropdown reads
//     /dev/             built from main
//     /vX.Y.Z/          built from each release tag
//     /latest/          a copy of the HIGHEST SEMVER version
//     /amplify.yml      serve-prebuilt config, so it rides on the published branch
//
// Two properties this has to have, and both are tested rather than asserted:
//
//   IDEMPOTENT — running it twice with the same inputs leaves the same bytes.
//   NON-DESTRUCTIVE — writing one version must not disturb any other. Only the
//   one segment is replaced; the derived files are recomputed from whatever is
//   on disk afterwards, so a version this run knows nothing about survives.
//
// NOTHING HERE CREATES A TAG OR A RELEASE. It only ever reacts to one that the
// maintainer already made (docs/roadmap/README.md, release philosophy §4).
import {
  cpSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { join, relative, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

/** `dev`, or a `vMAJOR.MINOR.PATCH` release segment. Nothing else may be written. */
const SEGMENT_RE = /^(dev|v\d+\.\d+\.\d+)$/;
const RELEASE_RE = /^v(\d+)\.(\d+)\.(\d+)$/;

/** Reserved names the assembler owns; a version may never be called one. */
const DERIVED = new Set(['latest', 'versions.json', 'index.html', 'amplify.yml']);

/** Sort key for a release segment — numeric per component, so v0.10.0 > v0.9.0. */
export function semverKey(segment) {
  const m = RELEASE_RE.exec(segment);
  if (!m) return null;
  return [Number(m[1]), Number(m[2]), Number(m[3])];
}

/** Descending semver; the first element is the highest. */
export function sortReleases(segments) {
  return segments
    .filter((s) => semverKey(s) !== null)
    .sort((a, b) => {
      const ka = semverKey(a);
      const kb = semverKey(b);
      for (let i = 0; i < 3; i += 1) {
        if (ka[i] !== kb[i]) return kb[i] - ka[i];
      }
      return 0;
    });
}

/** Every .html file under `dir`, dist-relative, sorted — the page list for a version. */
export function pagesIn(dir, base = dir) {
  if (!existsSync(dir)) return [];
  const out = [];
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) {
      out.push(...pagesIn(path, base));
    } else if (name.endsWith('.html')) {
      out.push(relative(base, path).split('\\').join('/'));
    }
  }
  return out.sort();
}

/** The version segments currently present in a publish root. */
export function existingSegments(root) {
  if (!existsSync(root)) return [];
  return readdirSync(root)
    .filter((name) => !DERIVED.has(name) && !name.startsWith('.') && !name.startsWith('_'))
    .filter((name) => SEGMENT_RE.test(name))
    .filter((name) => statSync(join(root, name)).isDirectory())
    .sort();
}

/**
 * The dropdown's manifest. `latest` names the highest release, or null while no
 * release exists — which is today's state and has to work.
 *
 * Each entry carries its page list so the dropdown can preserve the reader's
 * current page WITHOUT probing the server: a HEAD request would depend on how
 * the host answers for a missing file, and a host that serves a 200 fallback
 * would silently send every switch to a not-found page that looks fine.
 */
export function buildVersionsManifest(root, rootPath) {
  const segments = existingSegments(root);
  const releases = sortReleases(segments);
  const latest = releases.length > 0 ? releases[0] : null;

  const ordered = [...releases];
  if (segments.includes('dev')) ordered.push('dev');

  return {
    root: rootPath,
    latest,
    // Where `/` sends a reader. Until the maintainer tags v0.1.0 there is no
    // release, so the tree must stand on `dev/` alone.
    default: latest ?? (segments.includes('dev') ? 'dev' : null),
    versions: ordered.map((segment) => ({
      id: segment,
      path: segment,
      label: segment === 'dev' ? 'dev (main)' : segment,
      release: segment !== 'dev',
      pages: pagesIn(join(root, segment)),
    })),
  };
}

/** The root redirect. No-JS readers get the meta refresh and a visible link. */
export function buildRootRedirect(target) {
  const href = `./${target}/`;
  return `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>RoadMaker documentation</title>
    <meta http-equiv="refresh" content="0; url=${href}">
    <link rel="canonical" href="${href}">
    <meta name="robots" content="noindex">
  </head>
  <body>
    <p>Redirecting to <a href="${href}">the RoadMaker documentation</a>.</p>
    <script>location.replace(${JSON.stringify(href)});</script>
  </body>
</html>
`;
}

/** Copy a directory over the top of a destination, replacing it wholesale. */
function replaceDir(src, dest) {
  rmSync(dest, { recursive: true, force: true });
  mkdirSync(dirname(dest), { recursive: true });
  cpSync(src, dest, { recursive: true });
}

/** Write only when the bytes differ, so an unchanged run reports nothing changed. */
function writeIfChanged(path, content) {
  if (existsSync(path) && readFileSync(path, 'utf8') === content) return false;
  writeFileSync(path, content);
  return true;
}

/**
 * Place `buildDir` at `segment` inside `publishRoot` and recompute the derived
 * files.
 *
 * `scratch` is the dry run: everything, including that run's own versions.json,
 * latest/ and root redirect, is written inside `publishRoot/<scratch>/`. That is
 * a containment property rather than a promise — the real dev/, latest/,
 * versions.json and root redirect are not addressable from inside the scratch
 * root at all, so a dry run cannot reach them even if this code is wrong.
 */
export function assemble({ publishRoot, buildDir, segment, scratch = '', rootPath = '/' }) {
  if (!SEGMENT_RE.test(segment)) {
    throw new Error(
      `refusing to publish segment '${segment}': expected 'dev' or 'vMAJOR.MINOR.PATCH'`,
    );
  }
  if (buildDir && !existsSync(join(buildDir, 'index.html'))) {
    throw new Error(`'${buildDir}' has no index.html — that is not a built site`);
  }

  const root = scratch ? join(publishRoot, scratch) : publishRoot;
  const effectiveRootPath = scratch ? `${rootPath}${scratch}/`.replace(/\/+/g, '/') : rootPath;
  mkdirSync(root, { recursive: true });

  const changed = [];

  if (buildDir) {
    replaceDir(buildDir, join(root, segment));
    changed.push(`${segment}/`);
  }

  // latest/ is a COPY, not a link: the host serves files, and a symlink in a git
  // tree is a file containing a path.
  const manifest = buildVersionsManifest(root, effectiveRootPath);
  if (manifest.latest) {
    replaceDir(join(root, manifest.latest), join(root, 'latest'));
    changed.push(`latest/ (= ${manifest.latest})`);
  }

  if (manifest.default) {
    if (writeIfChanged(join(root, 'index.html'), buildRootRedirect(manifest.default))) {
      changed.push(`index.html -> ${manifest.default}/`);
    }
  }

  if (writeIfChanged(join(root, 'versions.json'), `${JSON.stringify(manifest, null, 2)}\n`)) {
    changed.push('versions.json');
  }

  const amplify = join(dirname(fileURLToPath(import.meta.url)), '..', 'publish', 'amplify.yml');
  if (existsSync(amplify)) {
    if (writeIfChanged(join(root, 'amplify.yml'), readFileSync(amplify, 'utf8'))) {
      changed.push('amplify.yml');
    }
  }

  return { root, manifest, changed };
}

// ------------------------------------------------------------------ as a script

function arg(name, fallback = '') {
  const prefix = `--${name}=`;
  const found = process.argv.find((a) => a.startsWith(prefix));
  return found ? found.slice(prefix.length) : fallback;
}

const here = fileURLToPath(import.meta.url);
if (process.argv[1] && join(process.argv[1]) === here) {
  const publishRoot = arg('root');
  const segment = arg('segment');
  if (!publishRoot || !segment) {
    console.error(
      'usage: assemble.mjs --root=<publish tree> --segment=dev|vX.Y.Z [--build=<dist>] [--scratch=<dir>]',
    );
    process.exit(1);
  }
  try {
    const { manifest, changed } = assemble({
      publishRoot,
      buildDir: arg('build'),
      segment,
      scratch: arg('scratch'),
    });
    console.log(`assemble: ${changed.length > 0 ? changed.join(', ') : 'nothing changed'}`);
    console.log(
      `assemble: ${manifest.versions.length} version(s), latest=${manifest.latest ?? '(none yet)'}, root -> ${manifest.default}/`,
    );
  } catch (error) {
    console.error(`assemble: ${error.message}`);
    process.exit(1);
  }
}
