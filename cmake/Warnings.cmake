# Compiler warnings — and the floating-point rules — for every target we own
# (never for fetched dependencies).
#
# The simulation is meant to be bit-reproducible across platforms, which
# needs every float operation to round the same way everywhere: IEEE adds,
# multiplies, divides and square roots do, as long as the compiler neither
# reorders them (-ffast-math would) nor fuses a multiply and an add into one
# differently-rounded instruction (which clang does by default on ARM). So:
# no fast-math anywhere, and no contraction. MSVC's /fp:precise already
# contracts nothing.
function(tynima_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /fp:precise)
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
      -ffp-contract=off  # a*b+c stays two roundings, on ARM as on x64
    )
    if(TYNIMA_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
