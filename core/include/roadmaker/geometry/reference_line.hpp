// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"

#include <cstddef>
#include <memory>
#include <variant>
#include <vector>

namespace roadmaker {

/// Sampled pose on a plan-view curve. Frame: right-handed, Z-up, meters;
/// hdg is CCW from +X in radians, curvature in 1/m (positive = left turn).
struct PathPoint {
  double x = 0.0;
  double y = 0.0;
  double hdg = 0.0;
  double curvature = 0.0;
};

/// planView primitive shapes (OpenDRIVE <geometry> children). The start
/// pose and length live in GeometryRecord; shapes hold only what varies.
struct LineGeom {};

struct ArcGeom {
  /// Constant curvature [1/m]; positive turns left.
  double curvature = 0.0;
};

struct SpiralGeom {
  /// Curvature at the record start / end [1/m]; varies linearly in s.
  double curv_start = 0.0;
  double curv_end = 0.0;
};

struct ParamPoly3Geom {
  /// u(p) = au + bu·p + cu·p² + du·p³ (local, along start heading);
  /// v(p) analogous (local, left of start heading).
  double au = 0.0, bu = 0.0, cu = 0.0, du = 0.0;
  double av = 0.0, bv = 0.0, cv = 0.0, dv = 0.0;

  /// true: p ∈ [0,1] (pRange="normalized"); false: p = local s in meters
  /// (pRange="arcLength").
  bool normalized = false;
};

/// One <geometry> record: start pose on the road + shape + length.
struct GeometryRecord {
  /// Start station [m], global along the reference line. Derived by
  /// ReferenceLine::append — records are always contiguous.
  double s = 0.0;

  /// Start position [m] and heading [rad] in the inertial frame.
  double x = 0.0;
  double y = 0.0;
  double hdg = 0.0;

  /// Arc length of this record [m]; must be > 0.
  double length = 0.0;

  std::variant<LineGeom, ArcGeom, SpiralGeom, ParamPoly3Geom> shape;
};

/// Prebuilt per-record evaluators for spiral records (Clothoids state is
/// expensive to rebuild per evaluate() call — hot during drag re-meshing).
/// Implementation detail of ReferenceLine, defined in the .cpp so the
/// Clothoids dependency never leaks into headers.
struct SpiralEvalCache;

/// A road's plan-view reference line: a contiguous sequence of geometry
/// records. `evaluate(s)` is the single entry point for all downstream
/// consumers (lanes, meshing, editors) — s in [0, length()], clamped.
// Exported per-method (like RoadNetwork): a class-level RM_API would demand a
// DLL interface for the std::vector member on MSVC (C4251).
class ReferenceLine {
public:
  /// Appends a record; its `s` is overwritten with the current length so
  /// the sequence stays contiguous. Records with length <= 0 are ignored.
  RM_API void append(GeometryRecord record);

  [[nodiscard]] const std::vector<GeometryRecord>& records() const { return records_; }

  [[nodiscard]] double length() const { return length_; }

  [[nodiscard]] bool empty() const { return records_.empty(); }

  /// Pose at station s [m], clamped to [0, length()]. Zero pose if empty.
  [[nodiscard]] RM_API PathPoint evaluate(double s) const;

private:
  [[nodiscard]] const SpiralEvalCache& spiral_cache() const;

  std::vector<GeometryRecord> records_;
  double length_ = 0.0;

  /// Lazily built on the first spiral evaluation, shared by copies (it is
  /// a pure function of records_), dropped by append(). Mutation under
  /// const relies on the kernel's single-threaded-per-network contract.
  mutable std::shared_ptr<const SpiralEvalCache> spiral_cache_;
};

/// Options for curvature-adaptive station sampling.
struct SamplingOptions {
  /// Max chord deviation between the sampled polyline and the true curve [m].
  double chord_tolerance = 0.01;

  /// Station step clamp [m].
  double min_step = 0.05;
  double max_step = 5.0;

  /// Stations that must appear in the output (lane-section boundaries,
  /// width-poly knots, ...). Values outside [0, length] are clamped.
  std::vector<double> extra_stations;
};

/// Stations for tessellating the reference line: 0, length(), every record
/// boundary, every extra station, and curvature-adaptive fill in between
/// (step = clamp(sqrt(8·tol/|κ|), min, max)). Sorted ascending, deduplicated
/// within tol::kLength. Empty line yields {}.
[[nodiscard]] RM_API std::vector<double> sample_stations(const ReferenceLine& line,
                                                         const SamplingOptions& options = {});

} // namespace roadmaker
