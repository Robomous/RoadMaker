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

// Astro lists `sharp` as an optionalDependency for its default image service.
// Its prebuilt libvips binaries are LGPL-3.0-or-later, and Qt is this project's
// ONLY sanctioned LGPL dependency (docs/standards/dependencies.md) — so
// package.json's `overrides` points sharp at this stub, and astro.config.mjs
// uses the passthrough image service instead.
//
// npm offers no way to omit a single optional dependency: `--omit=optional`
// would also drop rollup's required native binary. Overriding the one package
// is the narrowest mechanism that keeps the licence tree clean.
//
// Nothing should ever import this. If something does, say so loudly rather than
// failing somewhere confusing later.
throw new Error(
  'sharp is deliberately stubbed in this project (LGPL libvips). ' +
    'astro.config.mjs uses passthroughImageService(); if image processing is ' +
    'now genuinely required, that is a dependency-policy decision for the ' +
    'maintainer — see docs-site/README.md.',
);
