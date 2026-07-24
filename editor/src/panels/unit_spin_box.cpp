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

#include "panels/unit_spin_box.hpp"

#include <optional>

#include "document/units.hpp"

namespace roadmaker::editor {

UnitSpinBox::UnitSpinBox(QWidget* parent) : QDoubleSpinBox(parent) {
  setSuffix(units::suffix());
  // Re-render in place when the display system flips: setSuffix refreshes the
  // line edit through textFromValue, and the meters value is untouched.
  connect(&units::Notifier::instance(), &units::Notifier::changed, this, [this] {
    setSuffix(units::suffix());
  });
}

QString UnitSpinBox::textFromValue(double value) const {
  // Qt appends suffix() itself; emit only the converted number.
  return QString::number(units::display_from_meters(value), 'f', decimals());
}

double UnitSpinBox::valueFromText(const QString& text) const {
  return parsed(text).value_or(value());
}

QValidator::State UnitSpinBox::validate(QString& input, int& pos) const {
  Q_UNUSED(pos);
  if (!specialValueText().isEmpty() && input == specialValueText()) {
    return QValidator::Acceptable;
  }
  // Never Invalid: rejecting keystrokes would block typing "12 ft 6 in"
  // through its intermediate states. Unparseable text stays Intermediate, so
  // Qt reverts to the last good value on focus-out instead of committing it.
  return parsed(input).has_value() ? QValidator::Acceptable : QValidator::Intermediate;
}

std::optional<double> UnitSpinBox::parsed(const QString& text) const {
  // The edit text may carry the rendered suffix ("11.48 ft"), a unit the user
  // typed themselves ("6 in", "3.5 m"), or a bare number in the display unit.
  // Qt's internal line-edit validator also RE-APPENDS the configured suffix to
  // text that lacks it ("3.5 m" arrives as "3.5 m ft"), so a trailing rendered
  // suffix is chopped before parsing — for genuine "12 ft" input that chop
  // only turns an explicit foot entry into a bare display-unit one, which
  // parses to the same meters.
  const QString rendered = suffix();
  if (!rendered.isEmpty() && text.endsWith(rendered)) {
    if (const std::optional<double> meters = units::parse_length(text.chopped(rendered.size()))) {
      return meters;
    }
  }
  return units::parse_length(text);
}

} // namespace roadmaker::editor
