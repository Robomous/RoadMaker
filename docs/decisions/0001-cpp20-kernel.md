# ADR-0001: C++20 kernel with a permissive-license geometry stack

*Why the kernel is C++20 and why the geometry stack is Clothoids + Clipper2 +
CDT + Manifold rather than CGAL.*

- **Status:** accepted
- **Date:** 2026-05 (recorded retroactively 2026-07-10)
- **Deciders:** Armando Anaya

## Context

RoadMaker's value is geometric and standards correctness: clothoid-based
reference lines, watertight meshes, and OpenDRIVE round-trips within tight
tolerances. That demands a systems language with mature numeric libraries.
The project is Apache-2.0-licensed end to end, which rules out the most famous
computational-geometry library, CGAL (GPL), and any dependency that could
contaminate the license story.

Simulation-industry interoperability (CARLA, esmini, Omniverse) is all
C/C++, and the Python surface had to be a thin binding over the same kernel,
not a reimplementation.

## Decision

The kernel is **C++20, no compiler extensions**, and the geometry stack is
composed of permissive libraries only:

- **ebertolazzi/Clothoids** — clothoid evaluation and G1 Hermite fitting.
  Fresnel integrals are never hand-rolled.
- **Clipper2** — robust 2D polygon booleans (junction footprints).
- **CDT** — constrained Delaunay triangulation.
- **Manifold** — robust 3D booleans/solids where needed.

CGAL is forbidden (GPL); Triangle is forbidden (non-commercial license).
Error handling across the public kernel API uses `rm::Expected`, not
exceptions (see the [C++ style standard](../standards/cpp-style.md)).

## Consequences

- The Apache-2.0 story holds for the kernel and the Python wheels with no
  carve-outs; only the editor carries the sanctioned Qt/LGPL exception
  ([ADR-0003](0003-qt-widgets-editor.md)).
- Some algorithms that CGAL provides out of the box (exact predicates,
  arrangement machinery) must be composed from the smaller libraries or
  implemented against our own tolerance model (`rm::tol`).
- C++20 (concepts, ranges, designated initializers, `std::span`) is
  available everywhere; MSVC compatibility is a standing constraint (see
  [cross-platform standard](../standards/cross-platform.md)).

## References

- [Dependency & licensing policy](../standards/dependencies.md)
- [Geometry & meshing conventions](../domain/geometry.md)
