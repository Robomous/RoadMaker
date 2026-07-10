#pragma once

#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "render/renderer.hpp"
#include "viewport/camera.hpp"

namespace roadmaker::editor {

/// Read-only M1 viewer application state + UI. Editing tools begin in M2.
class App {
public:
  explicit App(std::unique_ptr<Renderer> renderer);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  App(App&&) = delete;
  App& operator=(App&&) = delete;

  [[nodiscard]] bool init();

  /// Loads a .xodr, rebuilds the render scene; parse problems land in the
  /// log panel, structural errors keep the previous document.
  void load_file(const std::filesystem::path& path);

  /// Draws the whole frame: dockspace, panels, and the 3D viewport.
  /// `fb_width`/`fb_height` are framebuffer pixels (HiDPI-safe).
  void draw_frame(int fb_width, int fb_height);

private:
  struct SceneItem {
    RenderMeshHandle handle;
    RoadId road;
    LaneId lane; // invalid for markings / floors / grid
  };

  void rebuild_scene();
  void draw_menu_bar();
  void draw_scene_tree();
  void draw_inspector();
  void draw_log();
  void draw_viewport_controls();
  [[nodiscard]] bool is_highlighted(const SceneItem& item) const;

  std::unique_ptr<Renderer> renderer_;
  OrbitCamera camera_;

  RoadNetwork network_;
  std::vector<Diagnostic> diagnostics_;
  NetworkMesh mesh_;
  std::string loaded_path_;

  std::vector<SceneItem> scene_;
  RenderMeshHandle grid_;

  RoadId selected_road_;
  LaneId selected_lane_;

  std::array<char, 1024> open_path_buffer_{};
  bool open_dialog_ = false;
};

} // namespace roadmaker::editor
