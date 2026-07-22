// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "app/welcome_widget.hpp"

#include "roadmaker/version.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <array>

#include "app/icons.hpp"
#include "app/settings.hpp"
#include "document/project.hpp"

namespace roadmaker::editor {

namespace {

constexpr QSize kThumbSize{220, 124};

/// Rounded placeholder tile for scenes without a captured thumbnail yet —
/// palette-driven so it follows the theme like everything else.
QPixmap placeholder_thumbnail(const QPalette& palette) {
  QPixmap pixmap(kThumbSize);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(palette.color(QPalette::Base));
  painter.drawRoundedRect(QRect(QPoint(0, 0), kThumbSize), 6, 6);
  const QPixmap glyph = Icons::get(QStringLiteral("clothoid-road")).pixmap(48, 48);
  painter.drawPixmap(QPoint((kThumbSize.width() - 48) / 2, (kThumbSize.height() - 48) / 2), glyph);
  return pixmap;
}

/// Rounded tile for a project — same construction as the scene placeholder,
/// with the folder glyph marking it as a directory of scenes.
QPixmap project_thumbnail(const QPalette& palette) {
  QPixmap pixmap(kThumbSize);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(palette.color(QPalette::Base));
  painter.drawRoundedRect(QRect(QPoint(0, 0), kThumbSize), 6, 6);
  const QPixmap glyph = Icons::get(QStringLiteral("folder-open")).pixmap(48, 48);
  painter.drawPixmap(QPoint((kThumbSize.width() - 48) / 2, (kThumbSize.height() - 48) / 2), glyph);
  return pixmap;
}

QPixmap scaled_thumbnail(const QString& path, const QPalette& palette) {
  QPixmap source(path);
  if (source.isNull()) {
    return placeholder_thumbnail(palette);
  }
  // Cover-crop into the tile so mixed aspect ratios still align in the grid.
  QPixmap scaled =
      source.scaled(kThumbSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
  const QRect crop((scaled.width() - kThumbSize.width()) / 2,
                   (scaled.height() - kThumbSize.height()) / 2,
                   kThumbSize.width(),
                   kThumbSize.height());
  return scaled.copy(crop);
}

QListWidget* make_scene_list(QWidget* parent) {
  auto* list = new QListWidget(parent);
  list->setViewMode(QListView::IconMode);
  list->setResizeMode(QListView::Adjust);
  list->setMovement(QListView::Static);
  list->setIconSize(kThumbSize);
  list->setSpacing(8);
  list->setWordWrap(true);
  list->setSelectionMode(QAbstractItemView::SingleSelection);
  list->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list->setFrameShape(QFrame::NoFrame);
  return list;
}

QLabel* make_section_label(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text.toUpper(), parent);
  label->setObjectName(QStringLiteral("welcomeSection"));
  return label;
}

} // namespace

WelcomeWidget::WelcomeWidget(Settings& settings, QWidget* parent)
    : QWidget(parent), settings_(settings) {
  setObjectName(QStringLiteral("welcomeRoot"));
  setAttribute(Qt::WA_StyledBackground, true); // plain QWidget honors QSS bg

  auto* outer = new QHBoxLayout(this);
  outer->setContentsMargins(48, 48, 48, 48);
  outer->setSpacing(48);

  // Left column: identity + primary actions + links.
  auto* intro = new QVBoxLayout;
  intro->setSpacing(8);
  auto* hero = new QLabel(QStringLiteral("RoadMaker"), this);
  hero->setObjectName(QStringLiteral("welcomeHero"));
  intro->addWidget(hero);
  auto* tagline = new QLabel(tr("Open-source road-network authoring —\n"
                                "ASAM OpenDRIVE, first and always."),
                             this);
  tagline->setObjectName(QStringLiteral("welcomeTagline"));
  intro->addWidget(tagline);
  auto* version =
      new QLabel(tr("kernel %1")
                     .arg(QString::fromUtf8(roadmaker::version().data(),
                                            static_cast<qsizetype>(roadmaker::version().size()))),
                 this);
  version->setObjectName(QStringLiteral("welcomeVersion"));
  intro->addWidget(version);
  intro->addSpacing(16);

  auto* new_button = new QPushButton(tr("New Scene"), this);
  new_button->setObjectName(QStringLiteral("welcomePrimary"));
  new_button->setMinimumHeight(36);
  connect(new_button, &QPushButton::clicked, this, &WelcomeWidget::new_scene_requested);
  intro->addWidget(new_button);
  auto* open_button = new QPushButton(tr("Open…"), this);
  open_button->setObjectName(QStringLiteral("welcomeOpen"));
  open_button->setMinimumHeight(36);
  connect(open_button, &QPushButton::clicked, this, &WelcomeWidget::open_requested);
  intro->addWidget(open_button);
  intro->addSpacing(16);

  auto* links = new QLabel(
      tr("<a href=\"https://github.com/Robomous/RoadMaker/tree/main/docs\">Documentation</a><br>"
         "<a href=\"https://github.com/Robomous/RoadMaker/issues\">Report an issue</a>"),
      this);
  links->setOpenExternalLinks(true);
  intro->addWidget(links);
  intro->addStretch(1);
  outer->addLayout(intro, 0);

  // Right column: the active project (when one is open), recent projects,
  // recent scenes, then samples.
  auto* content = new QVBoxLayout;
  content->setSpacing(8);

  project_section_ = new QWidget(this);
  auto* project_layout = new QVBoxLayout(project_section_);
  project_layout->setContentsMargins(0, 0, 0, 0);
  project_layout->setSpacing(8);
  project_label_ = make_section_label(QString(), project_section_);
  project_layout->addWidget(project_label_);
  project_scenes_list_ = make_scene_list(project_section_);
  project_scenes_list_->setObjectName(QStringLiteral("welcomeProjectScenesList"));
  project_layout->addWidget(project_scenes_list_);
  connect(
      project_scenes_list_, &QListWidget::itemClicked, this, [this](const QListWidgetItem* item) {
        emit file_requested(item->data(Qt::UserRole).toString());
      });
  auto* project_new_scene = new QPushButton(tr("New Scene in Project"), project_section_);
  project_new_scene->setObjectName(QStringLiteral("welcomeProjectNewScene"));
  connect(project_new_scene, &QPushButton::clicked, this, &WelcomeWidget::new_scene_requested);
  project_layout->addWidget(project_new_scene, 0, Qt::AlignLeft);
  project_section_->setVisible(false); // until a project is opened
  content->addWidget(project_section_, 3);

  projects_section_ = new QWidget(this);
  auto* projects_layout = new QVBoxLayout(projects_section_);
  projects_layout->setContentsMargins(0, 0, 0, 0);
  projects_layout->setSpacing(8);
  projects_layout->addWidget(make_section_label(tr("Recent projects"), projects_section_));
  projects_list_ = make_scene_list(projects_section_);
  projects_list_->setObjectName(QStringLiteral("welcomeProjectsList"));
  projects_layout->addWidget(projects_list_);
  connect(projects_list_, &QListWidget::itemClicked, this, [this](const QListWidgetItem* item) {
    emit project_requested(item->data(Qt::UserRole).toString());
  });
  content->addWidget(projects_section_, 2);

  recent_section_ = new QWidget(this);
  auto* recent_layout = new QVBoxLayout(recent_section_);
  recent_layout->setContentsMargins(0, 0, 0, 0);
  recent_layout->setSpacing(8);
  recent_layout->addWidget(make_section_label(tr("Recent"), recent_section_));
  recent_list_ = make_scene_list(recent_section_);
  recent_list_->setObjectName(QStringLiteral("welcomeRecentList"));
  recent_layout->addWidget(recent_list_);
  connect(recent_list_, &QListWidget::itemClicked, this, [this](const QListWidgetItem* item) {
    emit file_requested(item->data(Qt::UserRole).toString());
  });
  content->addWidget(recent_section_, 3);

  samples_section_ = new QWidget(this);
  auto* samples_layout = new QVBoxLayout(samples_section_);
  samples_layout->setContentsMargins(0, 0, 0, 0);
  samples_layout->setSpacing(8);
  samples_layout->addWidget(make_section_label(tr("Sample scenes"), samples_section_));
  samples_list_ = make_scene_list(samples_section_);
  samples_list_->setObjectName(QStringLiteral("welcomeSamplesList"));
  samples_layout->addWidget(samples_list_);
  connect(samples_list_, &QListWidget::itemClicked, this, [this](const QListWidgetItem* item) {
    emit file_requested(item->data(Qt::UserRole).toString());
  });
  content->addWidget(samples_section_, 2);

  outer->addLayout(content, 1);

  populate_samples();
  refresh();
}

QListWidgetItem* WelcomeWidget::make_scene_item(const QString& display_name,
                                                const QString& file_path,
                                                const QString& thumbnail_path) const {
  auto* item =
      new QListWidgetItem(QIcon(scaled_thumbnail(thumbnail_path, palette())), display_name);
  item->setData(Qt::UserRole, file_path);
  item->setToolTip(file_path);
  return item;
}

void WelcomeWidget::refresh() {
  recent_list_->clear();
  const QStringList recent = settings_.recent_files();
  for (const QString& path : recent) {
    if (!QFileInfo::exists(path)) {
      continue; // moved/deleted files would open into an error box
    }
    recent_list_->addItem(
        make_scene_item(QFileInfo(path).fileName(), path, thumbnail_path_for(path)));
  }
  recent_section_->setVisible(recent_list_->count() > 0);
  populate_projects();
  populate_active_project();
}

void WelcomeWidget::set_active_project(const QString& dir) {
  active_project_dir_ = dir;
  populate_active_project();
}

void WelcomeWidget::populate_projects() {
  projects_list_->clear();
  const QStringList recent = settings_.recent_projects();
  for (const QString& dir : recent) {
    // Re-open the manifest for each tile: a moved/deleted project would only
    // click into an error box, and the name + scene count must reflect the
    // directory as it is now.
    const auto project = Project::open(std::filesystem::path(dir.toStdString()));
    if (!project.has_value()) {
      continue;
    }
    const qsizetype scene_count = project->scenes().size();
    auto* item = new QListWidgetItem(
        QIcon(project_thumbnail(palette())),
        tr("%1 — %n scene(s)", "", static_cast<int>(scene_count)).arg(project->name()));
    item->setData(Qt::UserRole, dir);
    item->setToolTip(dir);
    projects_list_->addItem(item);
  }
  projects_section_->setVisible(projects_list_->count() > 0);
}

void WelcomeWidget::populate_active_project() {
  project_scenes_list_->clear();
  if (active_project_dir_.isEmpty()) {
    project_section_->setVisible(false);
    return;
  }
  const auto project = Project::open(std::filesystem::path(active_project_dir_.toStdString()));
  if (!project.has_value()) {
    project_section_->setVisible(false);
    return;
  }
  project_label_->setText(tr("Project — %1").arg(project->name()).toUpper());
  const QStringList scenes = project->scenes();
  for (const QString& path : scenes) {
    project_scenes_list_->addItem(
        make_scene_item(QFileInfo(path).fileName(), path, thumbnail_path_for(path)));
  }
  project_section_->setVisible(true);
}

void WelcomeWidget::populate_samples() {
  samples_list_->clear();
  const std::filesystem::path dir = find_samples_dir();
  if (dir.empty()) {
    samples_section_->setVisible(false);
    return;
  }
  // Curated subset with human names — the folder also holds test-oriented
  // fixtures that would read as noise here.
  const std::array<std::pair<const char*, const char*>, 4> curated{{
      {"crossing", "4-arm crossing"},
      {"t_junction", "T-junction"},
      {"curved_road", "Curved road"},
      {"overpass", "Overpass"},
  }};
  const QString dir_path = QString::fromStdString(dir.string());
  for (const auto& [stem, name] : curated) {
    const QString file = dir_path + QStringLiteral("/%1.xodr").arg(QLatin1String(stem));
    if (!QFileInfo::exists(file)) {
      continue;
    }
    const QString thumb = dir_path + QStringLiteral("/thumbnails/%1.png").arg(QLatin1String(stem));
    samples_list_->addItem(make_scene_item(tr(name), file, thumb));
  }
  samples_section_->setVisible(samples_list_->count() > 0);
}

void WelcomeWidget::showEvent(QShowEvent* event) {
  refresh();
  QWidget::showEvent(event);
}

QString WelcomeWidget::thumbnail_path_for(const QString& file_path) {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (base.isEmpty()) {
    return {};
  }
  const QString key =
      QString::fromLatin1(QCryptographicHash::hash(QFileInfo(file_path).absoluteFilePath().toUtf8(),
                                                   QCryptographicHash::Sha1)
                              .toHex());
  QDir dir(base + QStringLiteral("/thumbnails"));
  if (!dir.exists()) {
    dir.mkpath(QStringLiteral("."));
  }
  return dir.filePath(key + QStringLiteral(".png"));
}

std::filesystem::path find_samples_dir() {
  // Dev builds: the binary sits inside build/<preset>/editor(/*.app/…);
  // walk up until the repo root's assets/samples appears. Bundles later
  // relocate the samples next to the binary — the first hit wins either way.
  QDir probe(QCoreApplication::applicationDirPath());
  for (int depth = 0; depth < 8; ++depth) {
    const QString candidate = probe.filePath(QStringLiteral("assets/samples"));
    if (QFileInfo::exists(candidate)) {
      return std::filesystem::path(candidate.toStdString());
    }
    if (!probe.cdUp()) {
      break;
    }
  }
  return {};
}

} // namespace roadmaker::editor
