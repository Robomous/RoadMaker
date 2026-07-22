// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A width-stable label for variable-length text (issue #332): it keeps the full
// string and paints an ElideRight-elided copy sized to the current width.
//
// Plain QLabel reports the whole sentence as its size hint, so a long,
// tool-dependent string either widens its container — the toolbar reflow this
// was written for — or clips mid-word. Text like that does not belong in a
// toolbar at all; where it is legitimate (the status bar), it must at least
// never push the layout around.

#include <QLabel>
#include <QSize>
#include <QString>

class QPaintEvent;
class QResizeEvent;

namespace roadmaker::editor {

class ElidedLabel : public QLabel {
  Q_OBJECT

public:
  explicit ElidedLabel(QWidget* parent = nullptr);

  /// Sets the string to display. Always use this instead of QLabel::setText(),
  /// which is not virtual and would leave the tooltip stale.
  void set_full_text(const QString& text);

  [[nodiscard]] QString full_text() const { return text(); }

  /// What paintEvent() actually draws at the current width — the full text
  /// while it fits, ElideRight-truncated once it does not. Derived on demand,
  /// so it is correct without waiting for a resize event.
  [[nodiscard]] QString displayed_text() const;

  /// An elided label must not impose the full text's width as a minimum, or
  /// eliding buys nothing; only the height matters.
  [[nodiscard]] QSize minimumSizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  /// Full text as a tooltip, but only while something is hidden — an
  /// unconditional one would shadow the container's tooltip for no gain.
  void refresh_tooltip();
};

} // namespace roadmaker::editor
