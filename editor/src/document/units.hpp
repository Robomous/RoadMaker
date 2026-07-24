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

// Display-unit layer (#412, realism batch #411). The kernel, the persistence
// layer and every command value are SI meters unconditionally
// (docs/domain/realism_defaults.md, Unit policy) — this helper converts at the
// widget boundary only: readouts format in the active system, typed input is
// parsed back to meters. Imperial lengths display as decimal feet (US
// civil-engineering practice); parsing additionally accepts inches, meters and
// the ' / " abbreviations regardless of the active system.

#include <QObject>
#include <QString>
#include <optional>

namespace roadmaker::editor::units {

enum class UnitSystem { Metric, Imperial };

constexpr double kFeetPerMeter = 1.0 / 0.3048; // international foot, exact
constexpr double kInchesPerMeter = kFeetPerMeter * 12.0;

/// The process-wide display system. Metric until set_active() says otherwise;
/// MainWindow seeds it from Settings before any panel formats a value.
[[nodiscard]] UnitSystem active();

/// Switches the display system and notifies live widgets. Never touches the
/// document — display state is Settings territory, not undo territory.
void set_active(UnitSystem system);

/// Emits changed() whenever set_active() flips the system, so widgets that
/// hold formatted text (spin boxes, painted readouts, the status bar)
/// re-render in place instead of waiting for their next natural refresh.
class Notifier : public QObject {
  Q_OBJECT

public:
  [[nodiscard]] static Notifier& instance();

signals:
  void changed();
};

/// " m" / " ft" — the suffix a spin box shows after its number.
[[nodiscard]] QString suffix();

/// Meters → "3.50 m" / "11.48 ft". `decimals` applies to the displayed number
/// in either system.
[[nodiscard]] QString format_length(double meters, int decimals = 2);

/// Square meters → "120.0 m²" / "1291.7 ft²".
[[nodiscard]] QString format_area(double square_meters, int decimals = 1);

/// The magnitude `meters` displays as in the active system (no rounding).
[[nodiscard]] double display_from_meters(double meters);
[[nodiscard]] double meters_from_display(double value);

/// Typed input → meters. Accepts an optional unit ("m", "ft", "in", ', "),
/// including the composite 12 ft 6 in / 12'6" forms; a bare number means the
/// active display unit. Returns nullopt when the text is not a length.
[[nodiscard]] std::optional<double> parse_length(const QString& text);

} // namespace roadmaker::editor::units
