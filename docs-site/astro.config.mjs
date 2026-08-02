// @ts-check
import { defineConfig, passthroughImageService } from 'astro/config';
import starlight from '@astrojs/starlight';

// The site is built from adapted content (see scripts/adapt.mjs); nothing under
// src/content/docs/ is hand-written.
//
// TWO BUILDS, ONE SOURCE (ADR-0009):
//   web   — the published site. Directory URLs, Pagefind search on.
//   local — the offline reader bundled in every release. It opens from file://,
//           which changes two things. `format: 'file'` because a browser will
//           not serve index.html for a bare directory over file://, and search
//           OFF because Pagefind fetches its index over XHR, which file://
//           blocks in every mainstream browser. scripts/relativize.mjs then
//           turns the root-absolute refs Astro emits into relative ones.
const local = process.env.RM_DOCS_TARGET === 'local';

// Published under a version segment (`/dev/`, `/v0.1.0/`, or a dry-run scratch
// prefix) — docs-s3. Always a leading and trailing slash so it composes by
// concatenation; `/` for a site served from the domain root, and for the local
// reader, which has no server and no site root at all.
const base = local ? '/' : (process.env.RM_DOCS_BASE ?? '/');

export default defineConfig({
  base,
  // Astro's default image service is `sharp`, whose prebuilt libvips binaries
  // are LGPL-3.0-or-later. Qt is this project's ONLY sanctioned LGPL dependency
  // (docs/standards/dependencies.md), so the passthrough service is used and
  // `npm ci --omit=optional` keeps sharp out of the tree entirely. Guide images
  // are editor screenshots that need no build-time processing.
  image: { service: passthroughImageService() },
  ...(local ? { build: { format: 'file' } } : {}),
  integrations: [
    starlight({
      title: 'RoadMaker',
      description:
        'Open-source ASAM OpenDRIVE road authoring — user guide, tutorials and tool reference.',
      customCss: ['./src/styles/theme.css'],
      // Copied from editor/resources/branding by scripts/adapt.mjs. Starlight
      // links a favicon whether or not one exists, so naming a real file is what
      // stops the reference dangling.
      favicon: '/favicon.png',
      // Never ship a search box that does nothing: switching Pagefind off also
      // removes the header UI that would query it.
      pagefind: !local,
      // The version dropdown takes LanguageSelect's slot: the header renders it
      // unconditionally and a single-language site leaves it empty, so it is a
      // header position already shaped for choosing a variant of the site.
      //
      // Not registered at all for the local reader. Guarding inside the
      // component would still ship its hoisted <script>, and the offline copy
      // has exactly one version by definition — there is nothing to switch to.
      components: local ? {} : { LanguageSelect: './src/components/VersionSelect.astro' },
      sidebar: [
        { label: 'Guide', link: '/' },
        { label: 'Reference', autogenerate: { directory: 'reference' } },
        { label: 'Tutorials', autogenerate: { directory: 'tutorials' } },
      ],
    }),
  ],
});
