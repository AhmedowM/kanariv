# Resolve brew LLVM Clang on macOS (shared by the clang and auto
# dispatchers). Plain `clang` on PATH is AppleClang, which lags our C++23
# baseline; GCC builds miniaudio with a dummy backend (no audio).
#
# NOTE: the cache variable does NOT survive try_compile (compiler ABI
# probes re-run this file in a fresh scope), but the environment does --
# so callers export KANARIV_APPLE_CLANG=1 and both forms are honored.
if(DEFINED ENV{KANARIV_APPLE_CLANG})
  message(STATUS "AppleClang mode: keeping caller-provided compiler "
    "(unsupported)")
  return()
endif()
if(DEFINED ENV{LLVM_ROOT})
  set(_kanariv_llvm_root "$ENV{LLVM_ROOT}")
else()
  execute_process(COMMAND brew --prefix llvm
    OUTPUT_VARIABLE _kanariv_llvm_root OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE _kanariv_brew_rc)
  if(NOT _kanariv_brew_rc EQUAL 0)
    foreach(_cand /opt/homebrew/opt/llvm /usr/local/opt/llvm)
      if(EXISTS "${_cand}/bin/clang")
        set(_kanariv_llvm_root "${_cand}")
        break()
      endif()
    endforeach()
  endif()
endif()
if(NOT EXISTS "${_kanariv_llvm_root}/bin/clang")
  # Allow AppleClang (or any other compiler) when the user explicitly opts in.
  if(KANARIV_ALLOW_UNSUPPORTED_COMPILER OR DEFINED ENV{KANARIV_ALLOW_UNSUPPORTED_COMPILER})
    message(STATUS "KANARIV_ALLOW_UNSUPPORTED_COMPILER=ON: keeping caller-provided "
      "compiler (unsupported on macOS)")
    return()
  endif()
  message(FATAL_ERROR
    "LLVM Clang required on macOS (brew install llvm). "
    "AppleClang lags our C++23 baseline; GCC yields a dummy audio backend. "
    "Experts: set CMAKE_C/CXX_COMPILER manually and pass "
    "-DKANARIV_ALLOW_UNSUPPORTED_COMPILER=ON.")
endif()
set(CMAKE_C_COMPILER "${_kanariv_llvm_root}/bin/clang")
set(CMAKE_CXX_COMPILER "${_kanariv_llvm_root}/bin/clang++")

# Force brew's own libc++ headers and library. The SDK copy lags our C++23
# baseline (`std::expected`, `std::print`); nothing else reliably redirects
# the search there. -nostdinc++ drops every default C++ include (compiler
# intrinsics in the resource dir are unaffected), -cxx-isystem adds exactly
# ours ahead of any -isystem (e.g. FFmpeg's /opt/homebrew/include) so brew
# headers always win regardless of flag order. -stdlib=libc++ is needed at
# both compile (header selection) and link time. -L + rpath pair the matching
# dylib so binaries never mix new headers with the old system libc++ at
# runtime. Layout varies (lib/ vs lib/c++/, versioned dylibs), so derive the
# lib dir from the dylib itself instead of assuming it.
# NOTE: `std::move_only_function` and `std::generator` have no libc++
# implementation at all (missing even in LLVM 23); they are polyfilled
# in-tree (utils/function.hpp, utils/generator.hpp), not a header-search
# issue.
if(NOT EXISTS "${_kanariv_llvm_root}/include/c++/v1/__config")
  message(FATAL_ERROR
    "brew LLVM libc++ headers not found under ${_kanariv_llvm_root}. "
    "Reinstall with: brew reinstall llvm")
endif()
file(GLOB _kanariv_libcxx
  "${_kanariv_llvm_root}/lib/libc++*.dylib"
  "${_kanariv_llvm_root}/lib/c++/libc++*.dylib")
if(NOT _kanariv_libcxx)
  message(FATAL_ERROR
    "brew LLVM libc++ library not found under ${_kanariv_llvm_root}. "
    "Reinstall with: brew reinstall llvm")
endif()
list(GET _kanariv_libcxx 0 _kanariv_libcxx_first)
get_filename_component(_kanariv_libcxx_dir "${_kanariv_libcxx_first}" DIRECTORY)
add_compile_options(
  $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
  $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>
  $<$<COMPILE_LANGUAGE:CXX>:-cxx-isystem>
  $<$<COMPILE_LANGUAGE:CXX>:${_kanariv_llvm_root}/include/c++/v1>)
add_link_options(
  -stdlib=libc++
  "-L${_kanariv_libcxx_dir}"
  "-Wl,-rpath,${_kanariv_libcxx_dir}")
unset(_kanariv_llvm_root)
unset(_kanariv_brew_rc)
unset(_cand)
unset(_kanariv_libcxx)
unset(_kanariv_libcxx_first)
unset(_kanariv_libcxx_dir)
