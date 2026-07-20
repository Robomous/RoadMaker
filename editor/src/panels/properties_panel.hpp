#pragma once

// Properties of the selected entity: read-only rows rebuilt from the kernel
// on every selection change, plus manually bound editors (issues #14/#15,
// docs/design/m2/01_editing_framework.md §7) — the road name and the Lane
// Profile section edit the primary selection through Document commands, one
// command per discrete panel action. Text/spin editors commit on
// editingFinished only and skip the push when the value did not change, so
// refresh-on-undo never echoes a command back.

#include "roadmaker/mesh/junction_corners.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "panels/scrub_label.hpp"
#include "panels/slot_widget.hpp"
#include "render/material_catalog.hpp"

namespace roadmaker::editor {

class CornerTool;
class ElevationTool;
class LibraryListModel;

class PropertiesPanel : public QWidget {
  Q_OBJECT

public:
  PropertiesPanel(Document& document, const SelectionModel& selection, QWidget* parent = nullptr);

signals:
  /// A discrete panel action produced a user-facing result (e.g. a lane was
  /// removed) — MainWindow routes it to the viewport toast overlay. The panel
  /// never touches the viewport itself.
  void status_message(const QString& text);

  /// A slot was engaged: MainWindow should show `category` in the Library. The
  /// panel does not know the Library exists.
  void library_category_requested(const QString& category);

  /// A crosswalk asset's params were edited in the Attributes pane (p3-s2).
  /// MainWindow upserts `item` into the project-overlay manifest, saves it,
  /// refreshes the Library, and propagates the change to every following
  /// instance. Emitted once per discrete edit (editingFinished / material drop).
  void crosswalk_asset_committed(const LibraryItem& item);

  /// A prop-set asset's entries were edited in the Attributes pane (p6-s5).
  /// MainWindow upserts `item` into the project-overlay manifest and saves it.
  /// Unlike a crosswalk edit there is no propagation — baked props never
  /// reference the set. Emitted once per Save.
  void prop_set_asset_committed(const LibraryItem& item);

public:
  /// Read-only handle used to populate the crosswalk asset editor from the
  /// merged manifest. The panel never mutates it (MainWindow owns the overlay).
  void set_library_model(const LibraryListModel* model) { library_model_ = model; }

  /// Opens the crosswalk asset editor in the Attributes pane for `key`,
  /// bypassing the scene selection (LibraryPanel::asset_selected routes here).
  /// `editable` false renders the fields read-only with a hint (a built-in
  /// asset — create a project copy to edit). No-op for a non-crosswalk key.
  void edit_asset(const QString& key, bool editable);

  /// Wires the Elevation editing section to the Elevation tool's active node
  /// (issue #16). Until a tool is attached the section stays hidden; the
  /// panel never owns the tool. Call once, after both exist.
  void set_elevation_tool(ElevationTool* tool);

  /// Wires the Corner section to the Corner tool's active corner (p4-s1,
  /// issue #225). The section only appears when the panel's primary selection
  /// is a junction AND the tool's active corner belongs to that junction, so
  /// until a tool is attached it stays hidden. The panel never owns the tool.
  void set_corner_tool(CornerTool* tool);

  /// The editor's road-mark width conventions [m]. OpenDRIVE's @width has no
  /// normative values (weight standard/bold is the spec's coarse axis) —
  /// these presets are RoadMaker conventions (docs/domain/opendrive.md).
  static constexpr double kMarkWidthStandard = 0.12;
  static constexpr double kMarkWidthBold = 0.25;

private:
  void refresh();
  void refresh_lane_section();
  void refresh_elevation();
  void refresh_corner();

  /// Populates the Junction section (default corner radius + carriageway
  /// material) from `junction`, and shows it. Called from the junction branch
  /// of refresh(), after the read-only topology rows.
  void refresh_junction(const Junction& junction);

  void add_row(const QString& label, const QString& value);
  void clear_rows();

  // --- scrub-editing (P1/GW-2) ------------------------------------------------
  // One numeric attribute a ScrubLabel can drag. The table is deliberately
  // LOCAL to this panel: it is the only consumer, and a shared descriptor
  // registry would be speculative until a second one appears.

  struct ScrubBinding {
    QDoubleSpinBox* spin = nullptr; ///< live readout; also supplies the range clamp
    double units_per_pixel = 0.0;

    /// The attribute's current value, or nullopt when it is not editable right
    /// now (nothing selected, no active node) — the gesture then does nothing.
    std::function<std::optional<double>()> baseline;

    /// The command that sets the attribute to `value`, built against the
    /// preview session's BASE-state network (Document::PreviewFactory).
    std::function<std::unique_ptr<edit::Command>(const RoadNetwork&, double)> factory;
  };

  /// Registers `binding` against `label` and wires the gesture to one preview
  /// session. Returns the label so it can be dropped straight into a form row.
  ScrubLabel* install_scrub(ScrubLabel* label, ScrubBinding binding);

  void begin_scrub(std::size_t index);
  void update_scrub(double delta);
  void finish_scrub();
  void cancel_scrub();

  std::vector<ScrubBinding> scrubs_;

  /// The in-flight gesture's binding, baseline, and latest value. Empty index
  /// = no scrub (and no preview session owned by this panel).
  std::optional<std::size_t> scrub_active_;
  double scrub_baseline_ = 0.0;
  double scrub_value_ = 0.0;

  /// Commits the dropped library item into the primary prop's model slot.
  void push_object_model(const QString& key);

  /// Pins the dropped Materials item onto the primary selected marking instance
  /// (crosswalk / stencil / marking-curve) as a per-instance override
  /// (material_override), in ONE update_objects command. Toasts an unknown key;
  /// pushes nothing when the material is unchanged or the object carries no
  /// marking material.
  void push_instance_material(const QString& key);

  /// Applies the dropped road-style library item to the primary selected road.
  /// The slot is write-only (a road stores no style identity), so this is an
  /// apply action, not a stored reference like the prop Model slot.
  void push_road_style(const QString& key);

  /// Height of the Elevation tool's active node, or nullopt when no tool, no
  /// active node, or a stale road/index. Shared by the Height spin box and its
  /// scrub binding so both agree on what "current" means.
  [[nodiscard]] std::optional<double> active_node_height() const;

  /// The Corner tool's active corner solved against the current network, but
  /// ONLY while the panel's primary selection is that same junction — the
  /// single predicate the Corner section, its spin box and its scrub binding
  /// all agree on. nullopt when no tool, no active corner, a different
  /// primary selection, or a pair that no longer solves (an arm moved away).
  [[nodiscard]] std::optional<JunctionCornerInfo> active_corner_info() const;

  /// Removes the outermost lane on `side` (>0 left, <0 right) of the target
  /// section — no lane selection needed. Emits status_message on success.
  void remove_outermost_lane(int side);

  /// Pushes one command; a kernel refusal surfaces through the document's
  /// diagnostics (push_command appends one), never a crash or a stale panel
  /// — refresh() re-syncs from the network afterwards either way.
  void push(std::unique_ptr<edit::Command> command);

  /// The primary selection's lane (invalid id when road-level or empty).
  [[nodiscard]] const Lane* primary_lane() const;

  /// Whether a lane's width is a single constant record — the EXACT predicate
  /// edit::set_lane_width uses to accept or refuse (operations.cpp). Kept
  /// verbatim so the panel and the kernel never disagree about which lanes the
  /// constant-width editors (width_spin_ / its scrub) may touch. A tapered lane
  /// is edited in the 2D Editor's Lane Width tab instead.
  [[nodiscard]] static bool lane_width_is_constant(const Lane& lane);

  /// The section lane edits act on: the primary lane's, or the road's first.
  [[nodiscard]] LaneSectionId target_section() const;

  Document& document_;
  const SelectionModel& selection_;
  QFormLayout* form_;
  QLabel* placeholder_;

  QWidget* name_row_;
  QLineEdit* name_edit_;

  QGroupBox* lane_group_;
  QComboBox* type_combo_;
  QDoubleSpinBox* width_spin_;
  /// The "Width" scrub handle — kept so it can be disabled alongside width_spin_
  /// on a lane whose width varies along s (that edit belongs in the 2D Editor).
  ScrubLabel* width_scrub_label_ = nullptr;
  QComboBox* mark_combo_;
  QDoubleSpinBox* mark_width_spin_;
  /// Lane marking slot (p3-s1): a Library slot that sets the selected lane's
  /// road mark from a dropped Markings item (parity with the viewport drop).
  /// Enabled on any lane, centre included — lane 0 carries the centre-line mark.
  SlotWidget* marking_slot_;
  /// Lane Materials slot (p6-s3): a write-only Library slot that paves the
  /// selected lane's surface. Disabled for the centre lane (no material by rule).
  SlotWidget* lane_material_slot_;
  QPushButton* add_left_;
  QPushButton* add_right_;
  QPushButton* remove_left_;
  QPushButton* remove_right_;

  QGroupBox* elevation_group_;
  QLabel* elevation_node_label_;
  QDoubleSpinBox* elevation_spin_;
  ElevationTool* elevation_tool_ = nullptr;

  /// Corner section (p4-s1): the active junction fillet's radius. The arm-pair
  /// row names WHICH corner is being edited (the tool's sub-selection is not a
  /// SelectionModel entry, so the pane has to say it in words).
  QGroupBox* corner_group_;
  QFormLayout* corner_form_ = nullptr;
  QLabel* corner_arms_label_;
  QDoubleSpinBox* corner_radius_spin_;
  /// The "Corner radius" scrub handle — kept so the row can be hidden when the
  /// active pair no longer solves (GW-2 s9 drags this label).
  ScrubLabel* corner_radius_scrub_label_ = nullptr;
  /// Per-corner overlay material slots (p4-s2): the sidewalk wedge and the
  /// median nose of the arms meeting at this corner. They reflect the stored
  /// value and stay visible even on a corner too tight to carry a radius row.
  SlotWidget* corner_sidewalk_slot_;
  SlotWidget* corner_median_slot_;
  CornerTool* corner_tool_ = nullptr;

  /// Junction section (p4-s2): the junction-wide corner-radius default (the
  /// fallback every corner without its own radius uses) and the carriageway
  /// material. Shown for a selected junction, alongside its read-only rows.
  QGroupBox* junction_group_;
  QDoubleSpinBox* junction_radius_spin_;
  SlotWidget* junction_material_slot_;

  QGroupBox* signal_group_;
  QDoubleSpinBox* signal_s_spin_;
  QDoubleSpinBox* signal_t_spin_;
  QDoubleSpinBox* signal_h_spin_;
  QLabel* signal_kind_label_;

  /// Object section: a selected <object>. A prop shows the Model slot; a marking
  /// instance (crosswalk / stencil / marking-curve) shows the Material slot
  /// instead — refresh_object toggles the two rows and the group title.
  QGroupBox* object_group_;
  QLabel* object_kind_label_;
  SlotWidget* model_slot_;
  /// Per-instance marking material override (p3-s5): dropping a Materials item
  /// here pins this instance's paint (material_override), so a later change to
  /// the asset's Default Material leaves it alone (GW-2 s15, GW-5 s8/s9).
  SlotWidget* instance_material_slot_;
  /// The object group's form — kept so refresh_object can setRowVisible the
  /// Model vs Material rows per object kind.
  QFormLayout* object_form_ = nullptr;

  /// Road-style section (shown for a selected road): a write-only Library slot
  /// that applies a dropped road style to the road (p2-s8). Unlike the prop
  /// Model slot it reflects no stored value — a road keeps no style identity.
  QGroupBox* style_group_;
  SlotWidget* style_slot_;

  /// Ground-surface section (shown for a selected derived surface, p6-s2): the
  /// read-only stat rows live in form_; this group hosts the Materials slot that
  /// re-points the surface's material. It reflects the stored value.
  QGroupBox* surface_group_;
  SlotWidget* material_slot_;

  /// Commits the dropped Materials library item onto the primary selected
  /// surface. Unknown keys toast without pushing (no silent default); an
  /// unchanged material pushes nothing.
  void push_surface_material(const QString& key);

  /// Commits the dropped Materials library item as the active corner's sidewalk
  /// (resp. median) overlay material — the bare catalog name, like every other
  /// material slot. Unknown keys toast without pushing; an unchanged material
  /// pushes nothing (p4-s2).
  void push_corner_sidewalk_material(const QString& key);
  void push_corner_median_material(const QString& key);

  /// Commits the dropped Materials library item as the selected junction's
  /// carriageway material. Same unknown-key / unchanged-value rules.
  void push_junction_material(const QString& key);

  /// Commits the dropped Materials library item onto the primary selected lane
  /// as a single constant <material> record (§11.8.2). Refuses the centre lane,
  /// toasts an unknown key, and pushes nothing when the material is unchanged.
  void push_lane_material(const QString& key);

  /// Sets the primary selected lane's road mark from the dropped Markings item
  /// (§11.9). Toasts an unknown key; pushes nothing when the mark is unchanged.
  void push_marking(const QString& key);

  /// Populates the Prop section from the primary selection's object, and shows
  /// it.
  void refresh_object(const Object& object);

  /// Populates the Signal section (position spinboxes + read-only type rows)
  /// from the primary selection's signal, and shows it.
  void refresh_signal(const Signal& signal);

  /// Commits a move_signal from the spinbox values for the primary signal.
  void push_signal_move();

  // --- crosswalk asset editor (p3-s2) ----------------------------------------

  /// Builds the current asset_group_ widget values into a LibraryItem (keyed by
  /// asset_key_) and emits crosswalk_asset_committed. No-op when not editable or
  /// not in asset mode.
  void commit_asset_edit();

  /// Repaints the embedded asset preview from the current widget values.
  void refresh_asset_preview();

  /// Read-only lookup into the merged Library manifest (set by MainWindow).
  const LibraryListModel* library_model_ = nullptr;

  /// Resolves an asset's material to a tint for the embedded preview.
  MaterialCatalog materials_;

  /// True while the crosswalk asset editor owns the panel — set by edit_asset(),
  /// cleared on the next scene selection change. refresh() early-returns while
  /// set so a propagate command's mesh_changed does not close the editor.
  bool asset_mode_ = false;
  bool asset_editable_ = false;
  QString asset_key_;
  /// The asset's current Default Material key (the SlotWidget only shows a
  /// label, so the authoritative value is tracked here).
  QString asset_material_key_;

  QGroupBox* asset_group_;
  QDoubleSpinBox* asset_width_spin_;
  QDoubleSpinBox* asset_border_spin_;
  QDoubleSpinBox* asset_dash_spin_;
  QDoubleSpinBox* asset_gap_spin_;
  SlotWidget* asset_material_slot_;
  QLineEdit* asset_category_edit_;
  QLabel* asset_preview_;
  QLabel* asset_hint_;

  // --- prop-set asset editor (p6-s5) -----------------------------------------

  /// Opens the prop-set asset editor for `item` in the Attributes pane. A
  /// variable-length list of weighted model entries, saved explicitly (unlike
  /// the crosswalk editor's per-field auto-commit).
  void edit_prop_set_asset(const LibraryItem& item, bool editable);

  /// Appends one entry row (model combo + portion spin + remove button).
  void add_prop_set_row(const QString& model, double portion, bool editable);

  /// Drops every entry row and clears the tracking vector.
  void clear_prop_set_rows();

  /// Builds the current rows into a PropSet LibraryItem and emits
  /// prop_set_asset_committed. No-op when not editable / not in prop-set mode.
  void commit_prop_set_edit();

  /// One entry's widgets, so commit can read every row's model + portion and
  /// remove can drop a specific row.
  struct PropSetRow {
    QWidget* container = nullptr;
    QComboBox* model = nullptr;
    QDoubleSpinBox* portion = nullptr;
  };

  QGroupBox* prop_set_group_;
  QVBoxLayout* prop_set_entries_layout_ = nullptr;
  QPushButton* prop_set_add_button_ = nullptr;
  QPushButton* prop_set_save_button_ = nullptr;
  QLabel* prop_set_hint_;
  std::vector<PropSetRow> prop_set_rows_;
  /// The asset's label + category, tracked so a Save rebuilds the item without
  /// re-reading the (about-to-be-overwritten) manifest.
  QString prop_set_label_;
  QString prop_set_category_;
  /// True while the prop-set editor owns the panel (a subtype of asset_mode_).
  bool prop_set_mode_ = false;
};

} // namespace roadmaker::editor
