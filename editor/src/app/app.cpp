#include "app/app.hpp"

#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace roadmaker::editor {

namespace {

/// Same palette as the glTF exporter, kept in the editor so the viewport
/// matches exported files.
std::array<float, 4> lane_color(LaneType type) {
  switch (type) {
  case LaneType::Driving:
    return {0.25F, 0.25F, 0.27F, 1.0F};
  case LaneType::Stop:
    return {0.45F, 0.22F, 0.20F, 1.0F};
  case LaneType::Shoulder:
    return {0.42F, 0.42F, 0.39F, 1.0F};
  case LaneType::Biking:
    return {0.55F, 0.28F, 0.24F, 1.0F};
  case LaneType::Sidewalk:
    return {0.65F, 0.65F, 0.63F, 1.0F};
  case LaneType::Border:
    return {0.50F, 0.50F, 0.50F, 1.0F};
  case LaneType::Restricted:
    return {0.50F, 0.40F, 0.30F, 1.0F};
  case LaneType::Parking:
    return {0.30F, 0.32F, 0.48F, 1.0F};
  case LaneType::Median:
    return {0.30F, 0.45F, 0.30F, 1.0F};
  case LaneType::Curb:
    return {0.55F, 0.55F, 0.50F, 1.0F};
  case LaneType::None:
  case LaneType::Other:
    return {0.35F, 0.35F, 0.35F, 1.0F};
  }
  return {0.35F, 0.35F, 0.35F, 1.0F};
}

RenderMeshData to_render_data(const std::vector<double>& positions,
                              const std::vector<double>& normals,
                              const std::vector<std::uint32_t>& indices,
                              const std::array<float, 4>& color) {
  // Explicit double -> float narrowing: this is the kernel -> render
  // boundary, the one place precision is deliberately dropped.
  auto narrow = [](const std::vector<double>& values) {
    std::vector<float> out;
    out.reserve(values.size());
    for (const double v : values) {
      out.push_back(static_cast<float>(v));
    }
    return out;
  };
  RenderMeshData data;
  data.positions = narrow(positions);
  data.normals = narrow(normals);
  data.indices = indices;
  data.color = color;
  return data;
}

/// Ground grid: 1 m lines over a 200 m square, thicker style left to color.
RenderMeshData make_grid() {
  RenderMeshData data;
  data.kind = PrimitiveKind::Lines;
  data.color = {0.28F, 0.29F, 0.31F, 1.0F};
  constexpr int kHalf = 100;
  std::uint32_t index = 0;
  for (int i = -kHalf; i <= kHalf; ++i) {
    const auto a = static_cast<float>(i);
    const auto h = static_cast<float>(kHalf);
    // Line parallel to Y at x=i.
    data.positions.insert(data.positions.end(), {a, -h, 0.0F, a, h, 0.0F});
    // Line parallel to X at y=i.
    data.positions.insert(data.positions.end(), {-h, a, 0.0F, h, a, 0.0F});
    for (int k = 0; k < 4; ++k) {
      data.indices.push_back(index++);
    }
  }
  return data;
}

} // namespace

App::App(std::unique_ptr<Renderer> renderer) : renderer_(std::move(renderer)) {}

App::~App() = default;

bool App::init() {
  if (!renderer_->init()) {
    return false;
  }
  grid_ = renderer_->upload(make_grid());
  return true;
}

void App::load_file(const std::filesystem::path& path) {
  auto result = roadmaker::load_xodr(path);
  if (!result) {
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .location = result.error().context,
        .message = result.error().message,
    });
    spdlog::error("load failed: {} ({})", result.error().message, result.error().context);
    return;
  }
  network_ = std::move(result->network);
  diagnostics_ = std::move(result->diagnostics);
  loaded_path_ = path.string();
  selected_road_ = {};
  selected_lane_ = {};
  spdlog::info("loaded {} ({} roads, {} diagnostics)",
               loaded_path_,
               network_.road_count(),
               diagnostics_.size());
  rebuild_scene();
}

void App::rebuild_scene() {
  renderer_->clear_meshes();
  scene_.clear();
  grid_ = renderer_->upload(make_grid());

  mesh_ = build_network_mesh(network_);

  // Scene bounds for camera framing.
  std::array<float, 3> lo{1e9F, 1e9F, 1e9F};
  std::array<float, 3> hi{-1e9F, -1e9F, -1e9F};
  auto grow = [&](const std::vector<double>& positions) {
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
      for (int axis = 0; axis < 3; ++axis) {
        const auto v = static_cast<float>(positions[i + static_cast<std::size_t>(axis)]);
        lo[static_cast<std::size_t>(axis)] = std::min(lo[static_cast<std::size_t>(axis)], v);
        hi[static_cast<std::size_t>(axis)] = std::max(hi[static_cast<std::size_t>(axis)], v);
      }
    }
  };

  for (const RoadMesh& road : mesh_.roads) {
    grow(road.positions);
    for (const RoadMesh::LanePatch& patch : road.lanes) {
      RenderMeshData data =
          to_render_data(road.positions, road.normals, patch.indices, lane_color(patch.material));
      scene_.push_back(
          SceneItem{.handle = renderer_->upload(data), .road = road.road, .lane = patch.lane});
    }
    for (const SubMesh& marking : road.markings) {
      RenderMeshData data = to_render_data(
          marking.positions, marking.normals, marking.indices, {0.92F, 0.92F, 0.87F, 1.0F});
      scene_.push_back(SceneItem{.handle = renderer_->upload(data), .road = road.road});
    }
  }
  for (const SubMesh& floor : mesh_.junction_floors) {
    RenderMeshData data =
        to_render_data(floor.positions, floor.normals, floor.indices, {0.18F, 0.18F, 0.19F, 1.0F});
    scene_.push_back(SceneItem{.handle = renderer_->upload(data)});
    grow(floor.positions);
  }

  if (lo[0] < hi[0]) {
    const std::array<float, 3> center{
        (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    const float dx = hi[0] - lo[0];
    const float dy = hi[1] - lo[1];
    camera_.frame(center, std::max({dx, dy, 10.0F}) / 2.0F);
  }
}

bool App::is_highlighted(const SceneItem& item) const {
  if (selected_lane_.is_valid()) {
    return item.lane == selected_lane_;
  }
  if (selected_road_.is_valid()) {
    return item.road == selected_road_;
  }
  return false;
}

void App::draw_frame(int fb_width, int fb_height) {
  draw_menu_bar();

  // Full-window dockspace; panels dock around the central viewport.
  ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

  draw_scene_tree();
  draw_inspector();
  draw_log();
  draw_viewport_controls();

  // Camera input when no ImGui widget wants the mouse.
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantCaptureMouse) {
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      camera_.orbit(-io.MouseDelta.x * 0.008F, io.MouseDelta.y * 0.008F);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
        ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
      camera_.pan(io.MouseDelta.x, io.MouseDelta.y);
    }
    if (io.MouseWheel != 0.0F) {
      camera_.zoom(io.MouseWheel);
    }
  }

  // 3D scene under the UI (framebuffer-sized viewport — HiDPI rule).
  std::vector<DrawItem> items;
  items.reserve(scene_.size() + 1);
  if (grid_.valid()) {
    items.push_back(DrawItem{.mesh = grid_});
  }
  for (const SceneItem& item : scene_) {
    items.push_back(DrawItem{.mesh = item.handle, .highlighted = is_highlighted(item)});
  }
  const float aspect =
      fb_height > 0 ? static_cast<float>(fb_width) / static_cast<float>(fb_height) : 1.0F;
  renderer_->render(items, camera_.matrices(aspect), fb_width, fb_height);
}

void App::draw_menu_bar() {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Open .xodr...")) {
        open_dialog_ = true;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  if (open_dialog_) {
    ImGui::OpenPopup("Open OpenDRIVE file");
    open_dialog_ = false;
  }
  if (ImGui::BeginPopupModal("Open OpenDRIVE file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Path to .xodr:");
    ImGui::InputText("##path", open_path_buffer_.data(), open_path_buffer_.size());
    if (ImGui::Button("Open")) {
      load_file(std::filesystem::path(open_path_buffer_.data()));
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void App::draw_scene_tree() {
  ImGui::Begin("Scene");
  if (network_.road_count() == 0) {
    ImGui::TextDisabled("No file loaded (File > Open, or pass a .xodr on the command line).");
  }
  network_.for_each_road([&](RoadId road_id, Road& road) {
    const std::string label =
        (road.name.empty() ? "road " + road.odr_id : road.name) + "##road" + road.odr_id;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (selected_road_ == road_id && !selected_lane_.is_valid()) {
      flags |= ImGuiTreeNodeFlags_Selected;
    }
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
      selected_road_ = road_id;
      selected_lane_ = {};
    }
    if (open) {
      for (const LaneSectionId section_id : road.sections) {
        const LaneSection& section = *network_.lane_section(section_id);
        const std::string section_label =
            "section s0=" + std::to_string(section.s0) + "##" + road.odr_id;
        if (ImGui::TreeNodeEx(section_label.c_str(), ImGuiTreeNodeFlags_OpenOnArrow)) {
          for (const LaneId lane_id : section.lanes) {
            const Lane& lane = *network_.lane(lane_id);
            ImGuiTreeNodeFlags lane_flags =
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (selected_lane_ == lane_id) {
              lane_flags |= ImGuiTreeNodeFlags_Selected;
            }
            const std::string lane_label = "lane " + std::to_string(lane.odr_id);
            ImGui::TreeNodeEx((lane_label + "##" + std::to_string(lane_id.index)).c_str(),
                              lane_flags);
            if (ImGui::IsItemClicked()) {
              selected_road_ = road_id;
              selected_lane_ = lane_id;
            }
          }
          ImGui::TreePop();
        }
      }
      ImGui::TreePop();
    }
  });
  ImGui::End();
}

void App::draw_inspector() {
  ImGui::Begin("Inspector");
  if (const Road* road = network_.road(selected_road_)) {
    ImGui::Text("Road: %s", road->name.empty() ? road->odr_id.c_str() : road->name.c_str());
    ImGui::Text("OpenDRIVE id: %s", road->odr_id.c_str());
    ImGui::Text("Length: %.3f m", road->length);
    ImGui::Text("Geometry records: %zu", road->plan_view.records().size());
    ImGui::Text("Lane sections: %zu", road->sections.size());
    if (const Lane* lane = network_.lane(selected_lane_)) {
      ImGui::Separator();
      ImGui::Text("Lane %d", lane->odr_id);
      ImGui::Text("Width records: %zu", lane->widths.size());
      ImGui::Text("Road marks: %zu", lane->road_marks.size());
    }
  } else {
    ImGui::TextDisabled("Select a road or lane in the Scene panel.");
  }
  ImGui::End();
}

void App::draw_log() {
  ImGui::Begin("Log");
  ImGui::Text("%s", loaded_path_.empty() ? "no file" : loaded_path_.c_str());
  ImGui::Separator();
  for (const Diagnostic& diagnostic : diagnostics_) {
    ImVec4 color{0.7F, 0.7F, 0.7F, 1.0F};
    const char* prefix = "INFO";
    if (diagnostic.severity == Severity::Warning) {
      color = ImVec4{0.95F, 0.8F, 0.3F, 1.0F};
      prefix = "WARN";
    } else if (diagnostic.severity == Severity::Error) {
      color = ImVec4{0.95F, 0.35F, 0.3F, 1.0F};
      prefix = "ERROR";
    }
    ImGui::TextColored(
        color, "[%s] %s: %s", prefix, diagnostic.location.c_str(), diagnostic.message.c_str());
  }
  ImGui::End();
}

void App::draw_viewport_controls() {
  ImGui::Begin("View");
  ImGui::TextUnformatted("LMB drag: orbit");
  ImGui::TextUnformatted("RMB/MMB drag: pan");
  ImGui::TextUnformatted("Scroll: zoom");
  ImGui::Text("Distance: %.1f m", camera_.distance());
  ImGui::Text("Meshes: %zu", scene_.size());
  ImGui::End();
}

} // namespace roadmaker::editor
