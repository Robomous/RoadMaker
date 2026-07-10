#include <gtest/gtest.h>

#include <QAbstractItemModelTester>

#include "document/diagnostics_model.hpp"
#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

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
