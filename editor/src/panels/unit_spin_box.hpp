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

// A length spin box for the display-unit toggle (#412). value(), setValue(),
// the range, the step and every valueChanged consumer stay SI meters — only
// the rendered text and the meaning of typed input follow units::active(), so
// call sites are unit-blind and an imperial edit commits meters unchanged.

#include <QDoubleSpinBox>
#include <optional>

namespace roadmaker::editor {

class UnitSpinBox : public QDoubleSpinBox {
  Q_OBJECT

public:
  explicit UnitSpinBox(QWidget* parent = nullptr);

protected:
  [[nodiscard]] QString textFromValue(double value) const override;
  [[nodiscard]] double valueFromText(const QString& text) const override;
  QValidator::State validate(QString& input, int& pos) const override;

private:
  /// Edit text → meters, tolerant of the suffix Qt's internal validator
  /// re-appends to input that carries its own unit.
  [[nodiscard]] std::optional<double> parsed(const QString& text) const;
};

} // namespace roadmaker::editor
