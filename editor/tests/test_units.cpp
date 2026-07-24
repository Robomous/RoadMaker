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

// Display-unit toggle (#412, realism batch #411). The contract under test:
// display and input parsing follow units::active(), while every value that
// reaches the document — spin box value(), command captures, files — stays SI
// meters (docs/domain/realism_defaults.md, Unit policy).

#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QTest>
#include <QUndoStack>
#include <filesystem>
#include <optional>

#include "app/settings.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "document/units.hpp"
#include "panels/properties_panel.hpp"
#include "panels/unit_spin_box.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

/// Every test runs metric-first and leaves the process metric, so ordering
/// between suites never depends on which units test ran last.
class Units : public ::testing::Test {
protected:
  void SetUp() override { units::set_active(units::UnitSystem::Metric); }

  void TearDown() override { units::set_active(units::UnitSystem::Metric); }
};

using UnitSpinBoxTest = Units;
using PropertiesPanelUnits = Units;

/// QAbstractSpinBox keeps lineEdit() protected; typing tests need it.
struct ExposedSpin : UnitSpinBox {
  using QAbstractSpinBox::lineEdit;
};

RoadId first_road(const Document& document) {
  RoadId plain;
  document.network().for_each_road([&](RoadId id, const Road& road) {
    if (!plain.is_valid() && !road.junction.is_valid()) {
      plain = id;
    }
  });
  return plain;
}

LaneId lane_on_road(const Document& document, RoadId road_id, int odr_id) {
  const Road* road = document.network().road(road_id);
  if (road == nullptr || road->sections.empty()) {
    return {};
  }
  const LaneSection* section = document.network().lane_section(road->sections.front());
  if (section == nullptr) {
    return {};
  }
  for (const LaneId lid : section->lanes) {
    const Lane* lane = document.network().lane(lid);
    if (lane != nullptr && lane->odr_id == odr_id) {
      return lid;
    }
  }
  return {};
}

TEST_F(Units, MetricFormatsAsShipped) {
  EXPECT_EQ(units::suffix(), QStringLiteral(" m"));
  EXPECT_EQ(units::format_length(3.5), QStringLiteral("3.50 m"));
  EXPECT_EQ(units::format_length(12.3456, 3), QStringLiteral("12.346 m"));
  EXPECT_EQ(units::format_area(120.04, 1), QStringLiteral("120.0 m²"));
}

TEST_F(Units, ImperialFormatsDecimalFeet) {
  units::set_active(units::UnitSystem::Imperial);
  EXPECT_EQ(units::suffix(), QStringLiteral(" ft"));
  EXPECT_EQ(units::format_length(3.5), QStringLiteral("11.48 ft"));
  EXPECT_EQ(units::format_length(0.3048), QStringLiteral("1.00 ft"));
  EXPECT_EQ(units::format_area(100.0, 0), QStringLiteral("1076 ft²"));
}

TEST_F(Units, ExplicitUnitsParseRegardlessOfTheActiveSystem) {
  for (const units::UnitSystem system : {units::UnitSystem::Metric, units::UnitSystem::Imperial}) {
    units::set_active(system);
    EXPECT_NEAR(units::parse_length(QStringLiteral("3.5 m")).value(), 3.5, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("12 ft")).value(), 3.6576, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("12'")).value(), 3.6576, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("6 in")).value(), 0.1524, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("6\"")).value(), 0.1524, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("12 ft 6 in")).value(), 3.81, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("12'6\"")).value(), 3.81, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("-2 ft")).value(), -0.6096, 1e-12);
    EXPECT_NEAR(units::parse_length(QStringLiteral("-12' 6\"")).value(), -3.81, 1e-12);
  }
}

TEST_F(Units, BareNumbersMeanTheActiveDisplayUnit) {
  EXPECT_NEAR(units::parse_length(QStringLiteral("2.5")).value(), 2.5, 1e-12);
  EXPECT_NEAR(units::parse_length(QStringLiteral(".5")).value(), 0.5, 1e-12);
  units::set_active(units::UnitSystem::Imperial);
  EXPECT_NEAR(units::parse_length(QStringLiteral("12")).value(), 3.6576, 1e-12);
  EXPECT_NEAR(units::parse_length(QStringLiteral("-1.5")).value(), -0.4572, 1e-12);
}

TEST_F(Units, NonLengthsAreRejected) {
  EXPECT_FALSE(units::parse_length(QString()).has_value());
  EXPECT_FALSE(units::parse_length(QStringLiteral("abc")).has_value());
  EXPECT_FALSE(units::parse_length(QStringLiteral("12 kg")).has_value());
  EXPECT_FALSE(units::parse_length(QStringLiteral("--3")).has_value());
  EXPECT_FALSE(units::parse_length(QStringLiteral("3.5 m extra")).has_value());
}

TEST_F(Units, FormatThenParseRoundTripsInBothSystems) {
  for (const units::UnitSystem system : {units::UnitSystem::Metric, units::UnitSystem::Imperial}) {
    units::set_active(system);
    for (const double meters : {0.12, 3.5, 12.0, 250.75}) {
      const std::optional<double> back = units::parse_length(units::format_length(meters, 6));
      ASSERT_TRUE(back.has_value());
      EXPECT_NEAR(*back, meters, 1e-5);
    }
  }
}

TEST_F(UnitSpinBoxTest, ValueStaysMetersAcrossTheToggle) {
  UnitSpinBox spin;
  spin.setRange(0.0, 100.0);
  spin.setDecimals(2);
  spin.setValue(3.5);
  EXPECT_EQ(spin.text(), QStringLiteral("3.50 m"));

  units::set_active(units::UnitSystem::Imperial);
  EXPECT_EQ(spin.text(), QStringLiteral("11.48 ft"));
  EXPECT_DOUBLE_EQ(spin.value(), 3.5) << "the toggle must never touch the stored meters";

  units::set_active(units::UnitSystem::Metric);
  EXPECT_EQ(spin.text(), QStringLiteral("3.50 m"));
}

TEST_F(UnitSpinBoxTest, TypedImperialInputCommitsMeters) {
  units::set_active(units::UnitSystem::Imperial);
  ExposedSpin spin;
  spin.setRange(0.0, 100.0);
  spin.setDecimals(4);

  spin.lineEdit()->setText(QStringLiteral("12 ft 6 in"));
  spin.interpretText();
  EXPECT_NEAR(spin.value(), 3.81, 1e-9);

  spin.lineEdit()->setText(QStringLiteral("12"));
  spin.interpretText();
  EXPECT_NEAR(spin.value(), 3.6576, 1e-9) << "a bare number is the displayed unit — feet";

  spin.lineEdit()->setText(QStringLiteral("3.5 m"));
  spin.interpretText();
  EXPECT_NEAR(spin.value(), 3.5, 1e-9) << "an explicit metric entry wins over the display unit";
}

TEST_F(UnitSpinBoxTest, UnparseableTextRevertsToTheLastValue) {
  ExposedSpin spin;
  spin.setRange(0.0, 100.0);
  spin.setValue(2.0);
  spin.lineEdit()->setText(QStringLiteral("garbage"));
  spin.interpretText();
  EXPECT_DOUBLE_EQ(spin.value(), 2.0);
}

TEST_F(UnitSpinBoxTest, SpecialValueTextSurvivesTheToggle) {
  // The junction default-radius box spells its zero position "Derived"
  // (p4-s2); the unit layer must not re-format that sentinel into a number.
  UnitSpinBox spin;
  spin.setRange(0.0, 50.0);
  spin.setSpecialValueText(QStringLiteral("Derived"));
  spin.setValue(0.0);
  EXPECT_EQ(spin.text(), QStringLiteral("Derived"));
  units::set_active(units::UnitSystem::Imperial);
  EXPECT_EQ(spin.text(), QStringLiteral("Derived"));
}

TEST_F(Units, SettingPersistsAcrossInstancesAndDefaultsMetric) {
  // Same isolation as the other Settings suites: a test-only QSettings scope.
  QCoreApplication::setOrganizationName(QStringLiteral("RobomousTests"));
  QSettings().clear();
  {
    Settings fresh;
    EXPECT_EQ(fresh.display_units(), units::UnitSystem::Metric) << "metric is the default";
    fresh.set_display_units(units::UnitSystem::Imperial);
  }
  {
    Settings reloaded;
    EXPECT_EQ(reloaded.display_units(), units::UnitSystem::Imperial)
        << "the choice must survive a restart";
  }
  QSettings().clear();
  QCoreApplication::setOrganizationName(QStringLiteral("Robomous"));
}

TEST_F(Units, TogglingUnitsNeverDirtiesTheDocument) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  ASSERT_FALSE(document.is_dirty());

  units::set_active(units::UnitSystem::Imperial);
  units::set_active(units::UnitSystem::Metric);

  EXPECT_FALSE(document.is_dirty());
  EXPECT_EQ(document.undo_stack()->count(), 0) << "display state must never reach the undo stack";
}

TEST_F(PropertiesPanelUnits, ReadoutsFollowTheToggleLive) {
  Document document;
  SelectionModel selection{document};
  ASSERT_TRUE(document.load(kSample).has_value());
  PropertiesPanel panel(document, selection);
  selection.select({.road = first_road(document)});

  const auto row_text_containing = [&panel](const QString& needle) -> QString {
    const QList<QLabel*> labels = panel.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
      if (label->text().contains(needle)) {
        return label->text();
      }
    }
    return {};
  };

  ASSERT_FALSE(row_text_containing(QStringLiteral(" m")).isEmpty())
      << "a selected road shows metric readouts by default";
  units::set_active(units::UnitSystem::Imperial);
  EXPECT_FALSE(row_text_containing(QStringLiteral(" ft")).isEmpty())
      << "the pane must re-render live on the toggle, without a reselect";
}

TEST_F(PropertiesPanelUnits, ImperialSpinEditCommitsMetersToTheModel) {
  units::set_active(units::UnitSystem::Imperial);
  Document document;
  SelectionModel selection{document};
  ASSERT_TRUE(document.load(kSample).has_value());
  PropertiesPanel panel(document, selection);

  const RoadId road = first_road(document);
  const LaneId lane = lane_on_road(document, road, -1);
  ASSERT_TRUE(lane.is_valid());
  selection.select({.road = road, .lane = lane});

  auto* spin = panel.findChild<UnitSpinBox*>(QStringLiteral("lane_width_spin"));
  ASSERT_NE(spin, nullptr);
  spin->selectAll();
  QTest::keyClicks(spin, QStringLiteral("12"));
  spin->interpretText();
  emit spin->editingFinished();

  const Lane* edited = document.network().lane(lane);
  ASSERT_NE(edited, nullptr);
  ASSERT_FALSE(edited->widths.empty());
  // 12 ft = 3.6576 m, rounded by the box's two stored decimals.
  EXPECT_NEAR(edited->widths.front().a, 3.6576, 0.005)
      << "the model must receive meters, never the typed feet";
  EXPECT_EQ(document.undo_stack()->count(), 1);
}

} // namespace
} // namespace roadmaker::editor
