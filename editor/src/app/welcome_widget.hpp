#pragma once

// First-launch surface (UI revamp Phase 0, docs/standards/ui-design.md):
// recent scenes with thumbnails, sample scenes, New/Open entry points, and
// a docs link — shown as the central widget until a document exists.
// Logic-free: every choice is emitted as a signal; MainWindow decides.

#include <QLabel>
#include <QListWidget>
#include <QString>
#include <QWidget>
#include <filesystem>

namespace roadmaker::editor {

class Settings;

class WelcomeWidget : public QWidget {
  Q_OBJECT

public:
  explicit WelcomeWidget(Settings& settings, QWidget* parent = nullptr);

  /// Re-reads the recent-projects and recent-files lists and thumbnails
  /// (called on every show — saves during the session change all three).
  void refresh();

  /// Shows the active-project section for the project at `dir`: its name, its
  /// scenes as tiles, and a "New Scene in Project" affordance. An empty or
  /// unreadable `dir` hides the section. MainWindow calls this after opening,
  /// creating, or closing a project — the widget itself stays logic-free and
  /// only emits what the user picked.
  void set_active_project(const QString& dir);

  /// Where a scene's welcome thumbnail lives (AppDataLocation/thumbnails,
  /// keyed by a hash of the absolute path). Save-time capture writes it;
  /// refresh() reads it. Empty when no writable location exists.
  [[nodiscard]] static QString thumbnail_path_for(const QString& file_path);

signals:
  void new_scene_requested();
  void open_requested();
  void file_requested(const QString& path);
  /// A recent-project tile was clicked; `dir` is the project directory.
  void project_requested(const QString& dir);

protected:
  void showEvent(QShowEvent* event) override;

private:
  void populate_samples();
  void populate_projects();
  void populate_active_project();
  [[nodiscard]] QListWidgetItem* make_scene_item(const QString& display_name,
                                                 const QString& file_path,
                                                 const QString& thumbnail_path) const;

  Settings& settings_;
  QListWidget* recent_list_ = nullptr;
  QListWidget* samples_list_ = nullptr;
  QListWidget* projects_list_ = nullptr;
  QListWidget* project_scenes_list_ = nullptr;
  QWidget* recent_section_ = nullptr;
  QWidget* samples_section_ = nullptr;
  QWidget* projects_section_ = nullptr;
  QWidget* project_section_ = nullptr;
  QLabel* project_label_ = nullptr;
  QString active_project_dir_; ///< empty = no project open
};

/// The committed sample scenes (assets/samples) resolved from the running
/// binary's location — dev builds and bundles differ. Empty when not found.
[[nodiscard]] std::filesystem::path find_samples_dir();

} // namespace roadmaker::editor
