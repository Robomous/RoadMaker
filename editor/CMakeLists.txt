add_executable(roadmaker-editor
  src/app/main.cpp
  src/app/app.cpp
  src/render/gl_functions.cpp
  src/render/gl_renderer.cpp
  src/viewport/camera.cpp
)

target_include_directories(roadmaker-editor PRIVATE src)

target_link_libraries(roadmaker-editor PRIVATE
  roadmaker::core
  imgui::imgui
  glfw
)

rm_apply_warnings(roadmaker-editor)
