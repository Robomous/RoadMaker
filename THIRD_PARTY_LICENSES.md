# Third-party licenses

Every dependency of RoadMaker, its exact pinned version, license, and what it
is used for. A dependency lands in `cmake/deps.cmake` and in this table in the
same commit. Allowed licenses: MIT, BSD-2/3, Apache-2.0, MPL-2.0, zlib,
BSL-1.0, Unlicense/CC0. License files verified in each upstream archive.

| Name | Version | License | URL | Usage |
|---|---|---|---|---|
| Eigen | 3.4.1 | MPL-2.0 | https://gitlab.com/libeigen/eigen | Linear algebra (header-only) |
| Clothoids | 2.1.0 | BSD-2-Clause | https://github.com/ebertolazzi/Clothoids | Clothoid/Euler-spiral math: G1/G2 fitting, arc-length evaluation |
| UtilsLite | 29d83dc (Clothoids 2.1.0 submodule pin) | BSD-2-Clause | https://github.com/ebertolazzi/UtilsLite | Clothoids internal utilities (embeds fmt 11 © fmt contributors, MIT, in inline namespace fmt::v11) |
| quarticRootsFlocke | ad9028e (Clothoids 2.1.0 submodule pin) | BSD-2-Clause | https://github.com/ebertolazzi/quarticRootsFlocke | Polynomial root solving used by Clothoids |
| GenericContainer | f98cd3d (Clothoids 2.1.0 submodule pin) | BSD-2-Clause | https://github.com/ebertolazzi/GenericContainer | Container type used by Clothoids API |
| Manifold | 3.5.2 | Apache-2.0 | https://github.com/elalish/manifold | Robust mesh booleans, junction solids |
| libigl | 2.6.0 | MPL-2.0 | https://github.com/libigl/libigl | Mesh processing (header-only; GPL `copyleft/` headers are never used) |
| CDT | 1.4.4 | MPL-2.0 | https://github.com/artem-ogre/CDT | 2D constrained Delaunay triangulation (junction floors) |
| Clipper2 | 2.0.1 | BSL-1.0 | https://github.com/AngusJohnson/Clipper2 | 2D polygon offsets/unions for plan-view lane logic |
| pugixml | 1.16 | MIT | https://github.com/zeux/pugixml | XML parsing/writing for OpenDRIVE |
| tinygltf | 3.0.0 | MIT | https://github.com/syoyo/tinygltf | glTF 2.0 export (bundles nlohmann/json, MIT; stb, MIT/public-domain) |
| stb (stb_truetype.h) | commit 31c1ad3 (no upstream tagged releases) | MIT OR Unlicense (public domain) | https://github.com/nothings/stb | CPU TrueType rasterisation of editable sign faces (`roadmaker::signs::render_face`); header-only, `STB_TRUETYPE_IMPLEMENTATION` in one core TU |
| Roboto | 2.138 (subset compiled into the kernel) | Apache-2.0 | https://github.com/googlefonts/roboto | Font glyphs for rendered sign-face text — a Latin subset is embedded as a byte array in `core/src/assets/sign_font.gen.cpp` (see `scripts/gen_sign_font.py`); also listed in ASSETS_LICENSES.md |
| md4c | 0.5.2 (release-0.5.2) | MIT | https://github.com/mity/md4c | Markdown→HTML for the editor's build-time help compiler (rm_helpc); host tool only, never linked into the shipped editor (editor builds only) |
| tinyusdz | v0.9.1 | Apache-2.0 | https://github.com/lighttransport/tinyusdz | OpenUSD ASCII (.usda) export backend (editor/optional builds only, gated on `RM_BUILD_USD`; `.usdc`/`.usdz` crate output unsupported — documented M2 limitation) |
| mapbox/eternal | vendored in tinyusdz v0.9.1 | ISC | https://github.com/mapbox/eternal | Compile-time hash map used internally by tinyusdz. ISC is not in the enumerated allow-list but is more permissive than MIT — maintainer-approved 2026-07-10 |
| linalg.h | vendored in tinyusdz v0.9.1 | Unlicense | https://github.com/sgorsten/linalg | Small linear-algebra header used internally by tinyusdz — maintainer-approved 2026-07-10 |
| jsteemann/atoi | vendored in tinyusdz v0.9.1 | Apache-2.0 | https://github.com/jsteemann/atoi | Fast integer parsing used internally by tinyusdz (upstream Apache-2.0; no license text shipped in-tree) |
| tl::expected | 1.3.1 | CC0-1.0 | https://github.com/TartanLlama/expected | rm::Expected error returns (std::expected is C++23; kernel is C++20) |
| fast_float | 8.2.10 | Apache-2.0 OR MIT OR BSL-1.0 | https://github.com/fastfloat/fast_float | Locale-independent number parsing in xodr IO |
| GoogleTest | 1.17.0 | BSD-3-Clause | https://github.com/google/googletest | C++ unit tests (test builds only) |
| nanobind | >=2.1 (pip build requirement) | BSD-3-Clause | https://github.com/wjakob/nanobind | Python bindings (build-time; resolved via pyproject.toml) |
| Qt | 6.8.3 | LGPL-3.0-only | https://www.qt.io | Editor UI toolkit (editor builds only). Dynamically linked, NEVER static; never vendored or modified; provisioned by `scripts/setup_qt.py`, not FetchContent. Bundles ship Qt's LGPLv3 text and keep Qt as replaceable shared libraries (satisfies the LGPL relink provision). Sole sanctioned LGPL dependency — any other LGPL candidate needs maintainer approval. |
| {fmt} | 12.2.0 | MIT | https://github.com/fmtlib/fmt | Formatting (kernel-wide, no iostream) |
| spdlog | 1.17.0 | MIT | https://github.com/gabime/spdlog | Logging (built against external fmt) |
