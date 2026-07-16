#pragma once

// Interactive soak driver (hardening sprint §4.3, issue #86): executes
// thousands of seeded-random valid-ish editing operations against the REAL
// Document/command stack — create/drag/split roads, lane and elevation
// edits, junction create/delete, undo/redo bursts, save/reload cycles,
// selection churn — checking invariants after every operation:
//
//   - no preview session leaks out of an operation
//   - validate_network() reports no Error-severity findings
//   - every id the model still holds resolves (road→sections→lanes,
//     junction connections/arms, the selection)
//   - write_xodr → parse_xodr → write_xodr is byte-identical (on a cadence,
//     and at every save/reload)
//
// Determinism contract: the same seed produces the same operation sequence
// (all randomness flows through one mt19937; nothing depends on wall time,
// which only bounds the loop). The seed is printed on failure so any find
// is a one-line repro: roadmaker_soak --seed <N>.

#include "roadmaker/road/authoring.hpp"

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor::soak {

struct SoakOptions {
  std::uint32_t seed = 1;

  /// Stop after this many operations (0 = unbounded; bound by max_seconds).
  int max_ops = 1000;

  /// Stop after this much wall time (0 = unbounded; bound by max_ops).
  double max_seconds = 0.0;

  /// Directory for save/reload cycles (must exist and be writable).
  std::filesystem::path work_dir;

  /// Full write→parse→write byte comparison every N operations (it also
  /// always runs as part of a save/reload op). 0 disables the cadence.
  int round_trip_every = 8;
};

struct SoakStats {
  int ops = 0;
  int commands = 0; ///< push_command calls that succeeded
  int previews = 0; ///< drag sessions (committed or cancelled)
  int undos = 0;
  int redos = 0;
  int saves = 0;
  int rejected = 0; ///< factory/push refusals (valid-ish inputs may be refused)
};

class SoakDriver {
public:
  SoakDriver(Document& document, SelectionModel& selection, SoakOptions options);

  /// Runs until an invariant fails or a bound is hit. False = failure;
  /// failure() then carries seed, op index, op label, and detail.
  [[nodiscard]] bool run();

  [[nodiscard]] const std::string& failure() const { return failure_; }

  [[nodiscard]] const SoakStats& stats() const { return stats_; }

private:
  void step(int index);
  [[nodiscard]] bool check_invariants(int index, const char* label);
  void fail(int index, const char* label, const std::string& detail);
  void dump_round_trip(const std::string& first, const std::string& second) const;

  // Operations. Each draws all its randomness from rng_ and treats kernel
  // refusals as data (counted, never fatal) — the invariants afterwards are
  // what must hold.
  void op_create_road();
  void op_drag_waypoint();
  void op_insert_waypoint();
  void op_delete_waypoint();
  void op_lane_edit();
  void op_split_lane_section();
  void op_lane_width_profile();
  void op_elevation();
  void op_split_road();
  void op_translate_road();
  void op_rotate_road();
  void op_merge_roads();
  void op_create_junction();
  void op_duplicate_junction_attempt();
  void op_attach_t();
  void op_assembly_drop_on_road();
  void op_remove_lane();
  void op_overpass();
  void op_delete_crossing_road();
  void op_delete_junction();
  void op_delete_road();
  void op_undo_redo();
  void op_save_reload();
  void op_select();

  [[nodiscard]] double rand_range(double lo, double hi);
  [[nodiscard]] int rand_int(int lo, int hi); ///< inclusive bounds
  [[nodiscard]] bool chance(double probability);

  /// Live road ids in creation order; editable_only skips junction
  /// connecting roads (their geometry is owned by the generator).
  [[nodiscard]] std::vector<RoadId> live_roads(bool editable_only) const;
  [[nodiscard]] std::vector<JunctionId> live_junctions() const;
  [[nodiscard]] LaneProfile random_profile();

  void push(std::unique_ptr<edit::Command> command);

  Document& document_;
  SelectionModel& selection_;
  SoakOptions options_;
  std::mt19937 rng_;
  SoakStats stats_;
  std::string failure_;
  const char* current_label_ = "";
};

} // namespace roadmaker::editor::soak
