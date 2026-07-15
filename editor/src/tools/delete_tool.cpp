#include "tools/delete_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include "document/document.hpp"

namespace roadmaker::editor {

DeleteTool::DeleteTool(Document& document, QObject* parent) : Tool(parent), document_(document) {}

void DeleteTool::activate() {}

bool DeleteTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  if (!event.pick.has_value()) {
    return true; // LMB belongs to the tool even on a miss (M2 button map)
  }
  const Road* road = document_.network().road(event.pick->road);
  if (road == nullptr) {
    return true;
  }
  const QString name = road->name.empty() ? QString::fromStdString(road->odr_id)
                                          : QString::fromStdString(road->name);
  if (document_.push_command(edit::delete_road(document_.network(), event.pick->road))) {
    emit status_message(tr("Deleted road \"%1\" — Ctrl+Z restores").arg(name));
  }
  return true;
}

QString DeleteTool::instruction() const {
  return tr("Click a road to delete it · Undo restores it");
}

} // namespace roadmaker::editor
