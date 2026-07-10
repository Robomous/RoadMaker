# Warning configuration for RoadMaker targets only — never applied to
# third-party dependency targets.
#
# RM_WERROR is OFF for local development and ON in CI.
option(RM_WERROR "Treat warnings as errors (CI)" OFF)

function(rm_apply_warnings target)
  if(MSVC)
    # /external:anglebrackets: third-party headers (always included via
    # <...>) are exempt from /W4//WX; our own "quoted" headers are not.
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /external:anglebrackets /external:W0)
    if(RM_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    # -Wno-deprecated-declarations: spdlog 1.17 headers trip fmt 12's
    # deprecated string_view conversion; drop when upstream spdlog catches up.
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wno-deprecated-declarations)
    if(RM_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
