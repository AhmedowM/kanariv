# AppleClang toolchain (--toolchain apple): resolves the Xcode compilers
# via xcrun so the resolver pins no machine-specific paths. No brew LLVM.
find_program(_kanariv_xcrun xcrun REQUIRED)
execute_process(COMMAND "${_kanariv_xcrun}" -f clang
  OUTPUT_VARIABLE _kanariv_apple_cc OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _kanariv_apple_cc_rc)
execute_process(COMMAND "${_kanariv_xcrun}" -f clang++
  OUTPUT_VARIABLE _kanariv_apple_cxx OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _kanariv_apple_cxx_rc)
if(NOT _kanariv_apple_cc_rc EQUAL 0 OR NOT _kanariv_apple_cxx_rc EQUAL 0)
  message(FATAL_ERROR "xcrun could not resolve Xcode clang/clang++ -- install Xcode command line tools")
endif()
set(CMAKE_C_COMPILER "${_kanariv_apple_cc}")
set(CMAKE_CXX_COMPILER "${_kanariv_apple_cxx}")
unset(_kanariv_xcrun)
unset(_kanariv_apple_cc)
unset(_kanariv_apple_cxx)
unset(_kanariv_apple_cc_rc)
unset(_kanariv_apple_cxx_rc)
