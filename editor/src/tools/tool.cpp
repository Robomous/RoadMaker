// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "tools/tool.hpp"

namespace roadmaker::editor {

Tool::Tool(QObject* parent) : QObject(parent) {}

Tool::~Tool() = default;

} // namespace roadmaker::editor
