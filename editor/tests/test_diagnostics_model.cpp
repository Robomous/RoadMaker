#include "roadmaker/xodr/rules.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QIcon>
#include <QTemporaryDir>
#include <fstream>

#include "document/diagnostics_model.hpp"
#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

/// Road 1 declares a wrong length (rule-cited warning with a road id) and a
/// second road 1 is a skipped duplicate (rule-cited error without entity
/// ids); the header itself parses clean.
constexpr const char* kDiagnosticXodr = R"(<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <road id="1" length="7">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0">
      <center><lane id="0" type="none"/></center>
      <right><lane id="-1" type="driving"><width sOffset="0" a="3" b="0" c="0" d="0"/></lane></right>
    </laneSection></lanes>
  </road>
  <road id="1" length="5">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
</OpenDRIVE>)";

std::filesystem::path write_diagnostic_sample(const QTemporaryDir& dir) {
  const auto path = std::filesystem::path(dir.path().toStdString()) / "diagnostics.xodr";
  std::ofstream(path) << kDiagnosticXodr;
  return path;
}

TEST(DiagnosticsModel, PassesQtModelSanityChecks) {
  Document document;
  DiagnosticsModel model(document);
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);

  EXPECT_EQ(model.rowCount(), 0);
  ASSERT_TRUE(document.load(kSample).has_value());
  EXPECT_EQ(model.rowCount(), static_cast<int>(document.diagnostics().size()));
  EXPECT_EQ(model.columnCount(), DiagnosticsModel::kColumnCount);
}

TEST(DiagnosticsModel, RowsMirrorDiagnosticsAfterFailedLoad) {
  Document document;
  DiagnosticsModel model(document);

  const auto result = document.load("missing.xodr");
  EXPECT_FALSE(result.has_value());
  ASSERT_EQ(model.rowCount(), 1);
  EXPECT_EQ(model.index(0, DiagnosticsModel::kSeverity).data().toString(), QStringLiteral("Error"));
  EXPECT_FALSE(model.index(0, DiagnosticsModel::kMessage).data().toString().isEmpty());
  EXPECT_EQ(model.diagnostic_at(0), &document.diagnostics().front());
  EXPECT_EQ(model.diagnostic_at(7), nullptr);
}

TEST(DiagnosticsModel, SeverityColumnCarriesADecorationIcon) {
  Document document;
  DiagnosticsModel model(document);
  EXPECT_FALSE(document.load("missing.xodr").has_value());
  ASSERT_EQ(model.rowCount(), 1);

  const QVariant decoration = model.index(0, DiagnosticsModel::kSeverity).data(Qt::DecorationRole);
  const QIcon icon = decoration.value<QIcon>();
  ASSERT_FALSE(icon.isNull());
  EXPECT_FALSE(icon.pixmap(16).isNull());
  // Only the severity column decorates.
  EXPECT_FALSE(model.index(0, DiagnosticsModel::kMessage).data(Qt::DecorationRole).isValid());
}

TEST(DiagnosticsModel, RuleIdColumnMirrorsKernelRuleIds) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  DiagnosticsModel model(document);
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  ASSERT_TRUE(document.load(write_diagnostic_sample(dir)).has_value());

  std::vector<std::string> rule_cells;
  for (int row = 0; row < model.rowCount(); ++row) {
    rule_cells.push_back(
        model.index(row, DiagnosticsModel::kRuleId).data().toString().toStdString());
  }
  EXPECT_NE(std::ranges::find(rule_cells, std::string(roadmaker::rules::kIdUniqueInClass)),
            rule_cells.end());
  EXPECT_NE(std::ranges::find(rule_cells, std::string(roadmaker::rules::kRoadLengthSumGeometries)),
            rule_cells.end());
}

TEST(DiagnosticsModel, HeadersMatchSpec) {
  Document document;
  DiagnosticsModel model(document);
  EXPECT_EQ(
      model.headerData(DiagnosticsModel::kSeverity, Qt::Horizontal, Qt::DisplayRole).toString(),
      QStringLiteral("Severity"));
  EXPECT_EQ(model.headerData(DiagnosticsModel::kRuleId, Qt::Horizontal, Qt::DisplayRole).toString(),
            QStringLiteral("Rule id"));
  EXPECT_EQ(
      model.headerData(DiagnosticsModel::kLocation, Qt::Horizontal, Qt::DisplayRole).toString(),
      QStringLiteral("Location"));
  EXPECT_EQ(
      model.headerData(DiagnosticsModel::kMessage, Qt::Horizontal, Qt::DisplayRole).toString(),
      QStringLiteral("Message"));
}

} // namespace
} // namespace roadmaker::editor
