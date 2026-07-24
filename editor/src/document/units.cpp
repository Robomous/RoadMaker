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

#include "document/units.hpp"

#include <QRegularExpression>
#include <cmath>

namespace roadmaker::editor::units {

namespace {

UnitSystem g_active = UnitSystem::Metric;

// One number: 12, 12.5, .5, signed. Locale decimal commas are out of scope
// for this batch (#412), so the separator is '.' only.
constexpr const char* kNumber = R"([+-]?(?:\d+(?:\.\d+)?|\.\d+))";

} // namespace

UnitSystem active() {
  return g_active;
}

void set_active(UnitSystem system) {
  if (g_active == system) {
    return;
  }
  g_active = system;
  emit Notifier::instance().changed();
}

Notifier& Notifier::instance() {
  static Notifier notifier;
  return notifier;
}

QString suffix() {
  return active() == UnitSystem::Metric ? QStringLiteral(" m") : QStringLiteral(" ft");
}

double display_from_meters(double meters) {
  return active() == UnitSystem::Metric ? meters : meters * kFeetPerMeter;
}

double meters_from_display(double value) {
  return active() == UnitSystem::Metric ? value : value / kFeetPerMeter;
}

QString format_length(double meters, int decimals) {
  return QString::number(display_from_meters(meters), 'f', decimals) + suffix();
}

QString format_area(double square_meters, int decimals) {
  const bool metric = active() == UnitSystem::Metric;
  const double shown = metric ? square_meters : square_meters * kFeetPerMeter * kFeetPerMeter;
  return QString::number(shown, 'f', decimals) +
         (metric ? QStringLiteral(" m²") : QStringLiteral(" ft²"));
}

std::optional<double> parse_length(const QString& text) {
  const QString trimmed = text.trimmed();

  // Composite feet-and-inches first: 12 ft 6 in, 12'6", 12' 6. The sign on the
  // feet part applies to the whole length; the inches part is unsigned.
  static const QRegularExpression composite(
      QStringLiteral(R"(^(%1)\s*(?:ft|′|')\s*(\d+(?:\.\d+)?|\.\d+)\s*(?:in|″|")?$)")
          .arg(QLatin1String(kNumber)),
      QRegularExpression::CaseInsensitiveOption);
  if (const QRegularExpressionMatch match = composite.match(trimmed); match.hasMatch()) {
    const double feet = match.captured(1).toDouble();
    const double inches = match.captured(2).toDouble();
    const double magnitude = std::abs(feet) + (inches / 12.0);
    return std::copysign(magnitude, feet) / kFeetPerMeter;
  }

  static const QRegularExpression single(
      QStringLiteral(R"(^(%1)\s*(m|ft|in|′|″|'|")?$)").arg(QLatin1String(kNumber)),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = single.match(trimmed);
  if (!match.hasMatch()) {
    return std::nullopt;
  }
  const double value = match.captured(1).toDouble();
  const QString unit = match.captured(2).toLower();
  if (unit == QLatin1String("m")) {
    return value;
  }
  if (unit == QLatin1String("ft") || unit == QLatin1String("'") || unit == QStringLiteral("′")) {
    return value / kFeetPerMeter;
  }
  if (unit == QLatin1String("in") || unit == QLatin1String("\"") || unit == QStringLiteral("″")) {
    return value / kInchesPerMeter;
  }
  return meters_from_display(value); // bare number: the active display unit
}

} // namespace roadmaker::editor::units
