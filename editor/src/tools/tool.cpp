#include "tools/tool.hpp"

namespace roadmaker::editor {

Tool::Tool(QObject* parent) : QObject(parent) {}

Tool::~Tool() = default;

} // namespace roadmaker::editor
