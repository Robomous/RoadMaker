// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Read-only item model over the road network: Roads > LaneSections > Lanes,
// plus Junctions. Built as a flat node snapshot (indices, never pointers)
// inside a model reset on every Document::loaded() — the M1 document is
// immutable between loads, so incremental updates are M2 work.

#include "roadmaker/road/id.hpp"

#include <QAbstractItemModel>
#include <QString>
#include <cstdint>
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
  };

  [[nodiscard]] Target target_for(const QModelIndex& index) const;

  /// Indexes for mirroring SelectionModel changes into a view; invalid
  /// QModelIndex when the entity is not in the snapshot.
  [[nodiscard]] QModelIndex index_for_road(RoadId road) const;
  [[nodiscard]] QModelIndex index_for_lane(LaneId lane) const;
  [[nodiscard]] QModelIndex index_for_junction(JunctionId junction) const;

private:
  enum class Kind : std::uint8_t { RoadsGroup, JunctionsGroup, Road, LaneSection, Lane, Junction };

  struct Node {
    Kind kind = Kind::RoadsGroup;
    int parent = -1; // index into nodes_; -1 for the two top-level groups
    int row = 0;     // row within parent
    std::vector<int> children;
    RoadId road;
    LaneId lane;
    JunctionId junction;
    QString label;
  };

  void rebuild();
  [[nodiscard]] const Node* node_for(const QModelIndex& index) const;
  [[nodiscard]] QModelIndex index_for_node(int node) const;

  const Document& document_;
  std::vector<Node> nodes_; // nodes_[0] = Roads group, nodes_[1] = Junctions group
  std::unordered_map<RoadId, int> road_nodes_;
  std::unordered_map<LaneId, int> lane_nodes_;
  std::unordered_map<JunctionId, int> junction_nodes_;
};

} // namespace roadmaker::editor
