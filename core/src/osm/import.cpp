/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "roadmaker/osm/import.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/gis/crs.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace roadmaker::osm {
namespace {

/// Resolves planned roads to the ids they were authored under.
///
/// **Deliberately NOT arena iteration order.** The first design recovered ids
/// by diffing the arena against a pre-import snapshot and pairing the result
/// with the plan positionally, on the assumption that `for_each_road` yields
/// CREATION order. It does not — it walks slots ascending, and a freed slot is
/// reused, so after any erase the newly created roads come back in SLOT order
/// rather than the order they were made. A test that churned the arena first
/// caught two roads carrying each other's geometry: the "welds the wrong ends
/// together, silently" failure this whole mechanism exists to prevent.
///
/// So each road is authored under the `odr_id` its plan entry already names,
/// and this is one O(n) pass building the lookup. Order-independent by
/// construction rather than by assumption.
struct ImportState {
  std::unordered_map<std::string, RoadId> by_odr_id;
  bool resolved = false;
};

/// Wraps a stage so its failure is recorded and skipped rather than unwinding
/// the whole import.
///
/// `edit::composite` is atomic by design, which is right for a four-arm
/// intersection and exactly wrong for a district: one un-fittable way must not
/// discard sixteen hundred good roads. The deviation is confined here so the
/// composite's own contract stays intact for every other caller.
class TolerantStage final : public edit::Command {
public:
  TolerantStage(std::unique_ptr<edit::Command> inner,
                ApplyDiagnostics sink,
                std::string location,
                std::string_view rule)
      : inner_(std::move(inner)), sink_(std::move(sink)), location_(std::move(location)),
        rule_(rule) {}

  Expected<void> apply(RoadNetwork& network) override {
    if (inner_ == nullptr) {
      return {};
    }
    Expected<void> applied = inner_->apply(network);
    if (!applied.has_value()) {
      if (sink_ != nullptr) {
        sink_->push_back(Diagnostic{.severity = Severity::Error,
                                    .location = location_,
                                    .message = applied.error().message,
                                    .rule_id = std::string(rule_)});
      }
      // Drop it: a stage that failed its own apply captured state from a
      // network it did not change, and keeping it would revert work it never
      // did.
      inner_.reset();
    }
    return {};
  }

  Expected<void> revert(RoadNetwork& network) override {
    return inner_ == nullptr ? Expected<void>{} : inner_->revert(network);
  }

  [[nodiscard]] std::string_view name() const override { return "osm stage"; }

  [[nodiscard]] edit::DirtySet dirty() const override {
    return inner_ == nullptr ? edit::DirtySet{} : inner_->dirty();
  }

private:
  std::unique_ptr<edit::Command> inner_;
  ApplyDiagnostics sink_;
  std::string location_;
  std::string_view rule_;
};

std::unique_ptr<edit::Command> tolerant(std::unique_ptr<edit::Command> inner,
                                        const ApplyDiagnostics& sink,
                                        std::string location,
                                        std::string_view rule) {
  return std::make_unique<TolerantStage>(std::move(inner), sink, std::move(location), rule);
}

} // namespace

std::unique_ptr<edit::Command>
import_plan(const RoadNetwork& network, NetworkPlan plan, ApplyDiagnostics sink) {
  (void)network;
  if (sink == nullptr) {
    sink = std::make_shared<std::vector<Diagnostic>>();
  }
  auto state = std::make_shared<ImportState>();
  auto shared_plan = std::make_shared<NetworkPlan>(std::move(plan));

  std::vector<edit::CommandBuilder> builders;
  builders.reserve(shared_plan->roads.size() + 1);

  // --- road stages, in plan order -------------------------------------------
  for (std::size_t index = 0; index < shared_plan->roads.size(); ++index) {
    builders.push_back([shared_plan, sink, index](RoadNetwork&) {
      const PlannedRoad& road = shared_plan->roads[index];
      // The odr_id goes IN rather than being recovered afterwards — see
      // ImportState for what that assumption cost the first time.
      return tolerant(edit::create_road(road.waypoints, road.profile, road.name, {}, road.odr_id),
                      sink,
                      fmt::format("way/{}/segment/{}", road.way, road.segment),
                      rules::kOsmFitApproximated);
    });
  }

  // --- ONE joint stage for every joint --------------------------------------
  // Not one builder per joint: the lookup below is a single O(n) arena pass,
  // and repeating it per joint would be O(n²) at district scale — the same
  // trap RoadNetwork::find_road's documented linear search is.
  builders.push_back([shared_plan, sink, state](RoadNetwork& net) {
    if (!state->resolved) {
      net.for_each_road(
          [&state](RoadId id, const Road& road) { state->by_odr_id.emplace(road.odr_id, id); });
      state->resolved = true;
    }

    // Stamp <type>/<speed>, which create_road cannot carry: it takes a
    // LaneProfile, and a LaneProfile has no road class (see #454).
    for (const PlannedRoad& planned : shared_plan->roads) {
      const auto found = state->by_odr_id.find(planned.odr_id);
      if (found == state->by_odr_id.end() || !planned.type) {
        continue;
      }
      if (Road* road = net.road(found->second); road != nullptr) {
        road->types = {*planned.type};
      }
    }

    std::vector<edit::CommandBuilder> joint_builders;
    joint_builders.reserve(shared_plan->joints.size());

    for (const PlannedJoint& joint : shared_plan->joints) {
      std::vector<RoadEnd> ends;
      ends.reserve(joint.ends.size());
      for (const auto& [plan_index, contact] : joint.ends) {
        if (plan_index >= shared_plan->roads.size()) {
          continue;
        }
        const auto found = state->by_odr_id.find(shared_plan->roads[plan_index].odr_id);
        if (found != state->by_odr_id.end()) {
          ends.push_back(RoadEnd{.road = found->second, .contact = contact});
        }
      }
      if (ends.size() != joint.ends.size()) {
        continue; // a road this joint needed failed its own stage
      }

      const std::string location = fmt::format("node/{}", joint.node);
      if (joint.kind == JointKind::Link) {
        joint_builders.push_back([ends, sink, location](RoadNetwork& inner) {
          // Both ends were authored from the SAME reprojected point, so the
          // gap is ~0 and close_gap takes its pure-link branch.
          if (const auto linkable = edit::check_linkable(inner, ends[0], ends[1]); !linkable) {
            sink->push_back(Diagnostic{.severity = Severity::Warning,
                                       .location = location,
                                       .message = linkable.error().message,
                                       .rule_id = std::string(rules::kOsmTopologyUnlinked)});
            return std::unique_ptr<edit::Command>{};
          }
          return edit::close_gap(inner, ends[0], ends[1]);
        });
        continue;
      }

      joint_builders.push_back([ends, sink, location](RoadNetwork& inner) {
        // preview_junction runs the SAME planner create_junction will, so the
        // dropped-turn list is exact rather than an estimate. A district that
        // dropped forty turns in silence would look identical to one that
        // dropped none.
        const auto preview = edit::preview_junction(inner, ends);
        if (!preview) {
          sink->push_back(Diagnostic{.severity = Severity::Warning,
                                     .location = location,
                                     .message = preview.error().message,
                                     .rule_id = std::string(rules::kOsmTopologyUnlinked)});
          return std::unique_ptr<edit::Command>{};
        }
        for (const std::string& turn : preview->dropped_turns) {
          sink->push_back(Diagnostic{.severity = Severity::Warning,
                                     .location = location,
                                     .message = fmt::format("junction turn not fitted: {}", turn),
                                     .rule_id = std::string(rules::kOsmTurnDropped)});
        }
        if (preview->connection_count == 0) {
          // An empty junction is worse than none: it claims a topology it does
          // not provide.
          sink->push_back(
              Diagnostic{.severity = Severity::Warning,
                         .location = location,
                         .message = "junction would generate no connections; not created",
                         .rule_id = std::string(rules::kOsmTopologyUnlinked)});
          return std::unique_ptr<edit::Command>{};
        }
        // Re-running an import must regenerate rather than overlay a second
        // junction on the same arms.
        if (edit::matching_junction(inner, ends).has_value()) {
          return std::unique_ptr<edit::Command>{};
        }
        return edit::create_junction(inner, ends);
      });
    }

    edit::DirtySet joints_dirty;
    joints_dirty.topology = true;
    joints_dirty.junctions_are_current = true;
    return tolerant(edit::composite("osm joints", joints_dirty, std::move(joint_builders)),
                    sink,
                    "osm",
                    rules::kOsmTopologyUnlinked);
  });

  edit::DirtySet dirty;
  dirty.topology = true;
  // The joint stages generated the junctions themselves; a second editor-side
  // regeneration would double the work on the largest network it will hold.
  dirty.junctions_are_current = true;

  return edit::composite("Import OSM network", dirty, std::move(builders));
}

Expected<OsmImport> prepare_osm_import(const RoadNetwork& network,
                                       const std::filesystem::path& path,
                                       const OsmBuildOptions& options) {
  auto parsed = load_osm(path);
  if (!parsed) {
    return tl::unexpected(parsed.error());
  }

  // The same three calls the GIS and lidar importers make, which is why a
  // tile, an orthophoto and a district land on each other.
  const gis::Crs from = gis::parse_crs(parsed->graph.crs);
  const gis::Crs scene = gis::scene_crs(network.georeference());
  auto transform = gis::crs_transform(from, scene);
  if (!transform) {
    // VERBATIM, and shared: this importer never sets an origin itself.
    return tl::unexpected(transform.error());
  }

  std::vector<std::string> existing;
  network.for_each_road([&existing](RoadId, const Road& road) { existing.push_back(road.odr_id); });

  auto planned = plan_network(parsed->graph, *transform, options, existing);
  if (!planned) {
    return tl::unexpected(planned.error());
  }

  OsmImport import;
  import.diagnostics = std::move(parsed->diagnostics);
  import.diagnostics.insert(
      import.diagnostics.end(), planned->diagnostics.begin(), planned->diagnostics.end());
  import.apply_diagnostics = std::make_shared<std::vector<Diagnostic>>();
  import.plan = planned->plan;
  import.command = import_plan(network, planned->plan, import.apply_diagnostics);
  return import;
}

} // namespace roadmaker::osm
