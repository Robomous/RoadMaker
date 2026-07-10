# Warning configuration for RoadMaker targets only — never applied to
# third-party dependency targets.
#
# RM_WERROR is OFF for local development and ON in CI.
option(RM_WERROR "Treat warnings as errors (CI)" OFF)

function(rm_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(RM_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(RM_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
