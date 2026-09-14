# Compiler warnings for every target we own (never for fetched dependencies).
function(tynima_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    # fopen, strtol and friends are standard C; MSVC's "unsafe function" warning
    # (C4996) about them would otherwise become an error under /WX.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(TYNIMA_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic
      -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
      -Wcast-align -Wnull-dereference -Wformat=2
      -Wdouble-promotion # float math silently widened to double is a real cost in engine code
    )
    if(TYNIMA_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
