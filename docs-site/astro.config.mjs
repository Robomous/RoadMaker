// @ts-check
import { defineConfig, passthroughImageService } from 'astro/config';
import starlight from '@astrojs/starlight';

// The site is built from adapted content (see scripts/adapt.mjs); nothing under
// src/content/docs/ is hand-written.
export default defineConfig({
  // Astro's default image service is `sharp`, whose prebuilt libvips binaries
  // are LGPL-3.0-or-later. Qt is this project's ONLY sanctioned LGPL dependency
  // (docs/standards/dependencies.md), so the passthrough service is used and
  // `npm ci --omit=optional` keeps sharp out of the tree entirely. Guide images
  // are editor screenshots that need no build-time processing.
  image: { service: passthroughImageService() },
  integrations: [
    starlight({
      title: 'RoadMaker',
      description:
        'Open-source ASAM OpenDRIVE road authoring — user guide, tutorials and tool reference.',
      customCss: ['./src/styles/theme.css'],
      sidebar: [
        { label: 'Guide', link: '/' },
        { label: 'Reference', autogenerate: { directory: 'reference' } },
        { label: 'Tutorials', autogenerate: { directory: 'tutorials' } },
      ],
    }),
  ],
});
