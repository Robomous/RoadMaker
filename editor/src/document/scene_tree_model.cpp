#include "document/scene_tree_model.hpp"

namespace roadmaker::editor {

namespace {

constexpr quintptr kNoNode = static_cast<quintptr>(-1);

} // namespace

SceneTreeModel::SceneTreeModel(const Document& document, QObject* parent)
    : QAbstractItemModel(parent), document_(document) {
  rebuild();
  connect(&document_, &Document::loaded, this, [this] {
    beginResetModel();
    rebuild();
    endResetModel();
  });
}

void SceneTreeModel::rebuild() {
  nodes_.clear();
  road_nodes_.clear();
  lane_nodes_.clear();

  nodes_.push_back(Node{.kind = Kind::RoadsGroup, .row = 0, .label = tr("Roads")});
  nodes_.push_back(Node{.kind = Kind::JunctionsGroup, .row = 1, .label = tr("Junctions")});

  const RoadNetwork& network = document_.network();

  network.for_each_road([&](RoadId road_id, const Road& road) {
    const int road_node = static_cast<int>(nodes_.size());
    const QString road_name = road.name.empty()
                                  ? tr("road %1").arg(QString::fromStdString(road.odr_id))
                                  : QString::fromStdString(road.name);
    nodes_.push_back(Node{.kind = Kind::Road,
                          .parent = 0,
                          .row = static_cast<int>(nodes_[0].children.size()),
                          .road = road_id,
                          .label = road_name});
    nodes_[0].children.push_back(road_node);
    road_nodes_.emplace(road_id, road_node);

    for (const LaneSectionId section_id : road.sections) {
      const LaneSection* section = network.lane_section(section_id);
      if (section == nullptr) {
        continue;
      }
      const int section_node = static_cast<int>(nodes_.size());
      nodes_.push_back(
          Node{.kind = Kind::LaneSection,
               .parent = road_node,
               .row = static_cast<int>(nodes_[static_cast<std::size_t>(road_node)].children.size()),
               .road = road_id,
               .label = tr("section s0=%1 m").arg(section->s0)});
      nodes_[static_cast<std::size_t>(road_node)].children.push_back(section_node);

      for (const LaneId lane_id : section->lanes) {
        const Lane* lane = network.lane(lane_id);
        if (lane == nullptr) {
          continue;
        }
        const int lane_node = static_cast<int>(nodes_.size());
        nodes_.push_back(Node{
            .kind = Kind::Lane,
            .parent = section_node,
            .row = static_cast<int>(nodes_[static_cast<std::size_t>(section_node)].children.size()),
            .road = road_id,
            .lane = lane_id,
            .label = tr("lane %1").arg(lane->odr_id)});
        nodes_[static_cast<std::size_t>(section_node)].children.push_back(lane_node);
        lane_nodes_.emplace(lane_id, lane_node);
      }
    }
  });

  network.for_each_junction([&](JunctionId, const Junction& junction) {
    const int junction_node = static_cast<int>(nodes_.size());
    const QString name = junction.name.empty()
                             ? tr("junction %1").arg(QString::fromStdString(junction.odr_id))
                             : QString::fromStdString(junction.name);
    nodes_.push_back(Node{.kind = Kind::Junction,
                          .parent = 1,
                          .row = static_cast<int>(nodes_[1].children.size()),
                          .label = name});
    nodes_[1].children.push_back(junction_node);
  });
}

const SceneTreeModel::Node* SceneTreeModel::node_for(const QModelIndex& index) const {
  if (!index.isValid() || index.internalId() == kNoNode) {
    return nullptr;
  }
  const auto node = static_cast<std::size_t>(index.internalId());
  return node < nodes_.size() ? &nodes_[node] : nullptr;
}

QModelIndex SceneTreeModel::index_for_node(int node) const {
  if (node < 0 || static_cast<std::size_t>(node) >= nodes_.size()) {
    return {};
  }
  return createIndex(nodes_[static_cast<std::size_t>(node)].row, 0, static_cast<quintptr>(node));
}

QModelIndex SceneTreeModel::index(int row, int column, const QModelIndex& parent) const {
  if (!hasIndex(row, column, parent)) {
    return {};
  }
  if (!parent.isValid()) {
    return createIndex(row, column, static_cast<quintptr>(row)); // the two groups
  }
  const Node* parent_node = node_for(parent);
  if (parent_node == nullptr || row >= static_cast<int>(parent_node->children.size())) {
    return {};
  }
  return createIndex(
      row, column, static_cast<quintptr>(parent_node->children[static_cast<std::size_t>(row)]));
}

QModelIndex SceneTreeModel::parent(const QModelIndex& child) const {
  const Node* node = node_for(child);
  if (node == nullptr || node->parent < 0) {
    return {};
  }
  return index_for_node(node->parent);
}

int SceneTreeModel::rowCount(const QModelIndex& parent) const {
  if (!parent.isValid()) {
    return 2; // Roads, Junctions
  }
  if (parent.column() != 0) {
    return 0;
  }
  const Node* node = node_for(parent);
  return node == nullptr ? 0 : static_cast<int>(node->children.size());
}

int SceneTreeModel::columnCount(const QModelIndex& /*parent*/) const {
  return 1;
}

QVariant SceneTreeModel::data(const QModelIndex& index, int role) const {
  const Node* node = node_for(index);
  if (node == nullptr || role != Qt::DisplayRole) {
    return {};
  }
  return node->label;
}

QVariant SceneTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section == 0) {
    return tr("Scene");
  }
  return {};
}

SceneTreeModel::Target SceneTreeModel::target_for(const QModelIndex& index) const {
  const Node* node = node_for(index);
  if (node == nullptr) {
    return {};
  }
  return Target{.road = node->road, .lane = node->lane};
}

QModelIndex SceneTreeModel::index_for_road(RoadId road) const {
  const auto it = road_nodes_.find(road);
  return it == road_nodes_.end() ? QModelIndex{} : index_for_node(it->second);
}

QModelIndex SceneTreeModel::index_for_lane(LaneId lane) const {
  const auto it = lane_nodes_.find(lane);
  return it == lane_nodes_.end() ? QModelIndex{} : index_for_node(it->second);
}

} // namespace roadmaker::editor
