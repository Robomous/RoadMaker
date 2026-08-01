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

#pragma once

// Read-only item model over the road network: Roads > LaneSections > Lanes,
// plus Junctions. Built as a flat node snapshot (indices, never pointers)
// inside a model reset on every Document::loaded() — the M1 document is
// immutable between loads, so incremental updates are M2 work.

#include "roadmaker/road/id.hpp"

#include <QAbstractItemModel>
#include <QString>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "document/document.hpp"

namespace roadmaker::editor {

class SceneTreeModel : public QAbstractItemModel {
  Q_OBJECT

public:
  explicit SceneTreeModel(const Document& document, QObject* parent = nullptr);

  [[nodiscard]] QModelIndex
  index(int row, int column, const QModelIndex& parent = {}) const override;
  [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QVariant
  headerData(int section, Qt::Orientation orientation, int role) const override;

  /// What a tree row refers to; ids invalid where not applicable (group
  /// headers yield an empty target).
  struct Target {
    RoadId road;
    LaneId lane;
    JunctionId junction;

    /// A scenario actor's `<ScenarioObject @name>` (#246); empty = not an actor
    /// row. A STRING and not an id for the reason `SelectionEntry::actor` is
    /// one: an actor is not arena content and has no generational handle.
    std::string actor;
  };

  [[nodiscard]] Target target_for(const QModelIndex& index) const;

  /// Indexes for mirroring SelectionModel changes into a view; invalid
  /// QModelIndex when the entity is not in the snapshot.
  [[nodiscard]] QModelIndex index_for_road(RoadId road) const;
  [[nodiscard]] QModelIndex index_for_lane(LaneId lane) const;
  [[nodiscard]] QModelIndex index_for_junction(JunctionId junction) const;
  [[nodiscard]] QModelIndex index_for_actor(std::string_view actor) const;

private:
  enum class Kind : std::uint8_t {
    RoadsGroup,
    JunctionsGroup,
    /// The scenario branch (#246). A PERMANENT third top-level group beside the
    /// other two, present even when the scene carries no scenario — the two
    /// existing groups are unconditional, and a group that came and went would
    /// make every top-level row index depend on document state.
    ScenarioGroup,
    Road,
    LaneSection,
    Lane,
    Junction,
    Actor,
  };

  struct Node {
    Kind kind = Kind::RoadsGroup;
    int parent = -1; // index into nodes_; -1 for the two top-level groups
    int row = 0;     // row within parent
    std::vector<int> children;
    RoadId road;
    LaneId lane;
    JunctionId junction;
    std::string actor;
    QString label;
  };

  void rebuild();
  [[nodiscard]] const Node* node_for(const QModelIndex& index) const;
  [[nodiscard]] QModelIndex index_for_node(int node) const;

  const Document& document_;
  /// nodes_[0] = Roads, nodes_[1] = Junctions, nodes_[2] = Scenario. The three
  /// group nodes are at indices 0..2 AND at rows 0..2, which `index()` relies on
  /// for a top-level row (it uses the row itself as the node id).
  std::vector<Node> nodes_;
  std::unordered_map<RoadId, int> road_nodes_;
  std::unordered_map<LaneId, int> lane_nodes_;
  std::unordered_map<JunctionId, int> junction_nodes_;
  std::unordered_map<std::string, int> actor_nodes_;
};

} // namespace roadmaker::editor
