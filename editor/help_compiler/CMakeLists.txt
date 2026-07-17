# The help compiler is a build-time host tool: it turns the committed Markdown
# guide into the HTML + .qhp + .qhcp that qhelpgenerator compiles into the
# shipped .qch/.qhc. It is deliberately Qt-FREE (std::filesystem + md4c-html
# only) so it stays a lightweight tool with nothing to deploy, and so its
# library can be unit-tested without a QApplication.

add_library(helpc_lib STATIC
  helpc/toc.cpp
  helpc/render.cpp
  helpc/qhp.cpp
)
target_include_directories(helpc_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(helpc_lib PUBLIC md4c-html)
rm_apply_warnings(helpc_lib)

add_executable(rm_helpc main.cpp)
target_link_libraries(rm_helpc PRIVATE helpc_lib)
rm_apply_warnings(rm_helpc)
