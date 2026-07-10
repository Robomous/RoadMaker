# Sanitizer support: configure with -DRM_SANITIZE=address,undefined
# (comma-separated list passed straight to -fsanitize=).
#
# Not supported on MSVC (Windows CI uses /analyze instead) and unreliable on
# macOS CI runners — intended for local Linux/macOS dev and the Linux CI job.
set(RM_SANITIZE "" CACHE STRING "Comma-separated sanitizers (e.g. address,undefined)")

if(RM_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=${RM_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${RM_SANITIZE})
endif()
