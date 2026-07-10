// roadmaker-editor: read-only OpenDRIVE viewer (Milestone 1).
// Usage: roadmaker-editor [file.xodr]

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <memory>

#include "app/app.hpp"
#include "render/gl_renderer.hpp"

namespace {

void glfw_error_callback(int code, const char* description) {
  spdlog::error("GLFW error {}: {}", code, description);
}

} // namespace

int main(int argc, char** argv) {
  glfwSetErrorCallback(glfw_error_callback);
  if (glfwInit() == GLFW_FALSE) {
    spdlog::critical("glfwInit failed");
    return 1;
  }

  // OpenGL 3.3 core, forward-compatible — required on macOS, harmless
  // elsewhere. All GL above 3.3 is off-limits (macOS caps at 4.1).
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

  GLFWwindow* window = glfwCreateWindow(1600, 1000, "RoadMaker", nullptr, nullptr);
  if (window == nullptr) {
    spdlog::critical("window creation failed");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  // HiDPI: scale fonts by the monitor content scale.
  float scale_x = 1.0F;
  float scale_y = 1.0F;
  glfwGetWindowContentScale(window, &scale_x, &scale_y);
  io.FontGlobalScale = 1.0F;
  ImGui::GetStyle().ScaleAllSizes(scale_x);

  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330 core");

  {
    roadmaker::editor::App app(std::make_unique<roadmaker::editor::GLRenderer>());
    if (!app.init()) {
      spdlog::critical("renderer init failed");
      ImGui_ImplOpenGL3_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      return 1;
    }
    if (argc > 1) {
      app.load_file(argv[1]);
    }

    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
      glfwPollEvents();
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();

      // Framebuffer size, not window size (HiDPI rule).
      int fb_width = 0;
      int fb_height = 0;
      glfwGetFramebufferSize(window, &fb_width, &fb_height);
      app.draw_frame(fb_width, fb_height);

      ImGui::Render();
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      glfwSwapBuffers(window);
    }
  } // App (and its GL resources) die while the context is still current.

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
