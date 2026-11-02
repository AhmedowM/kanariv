# Docs.cmake -- Doxygen documentation target (enabled via KANARIV_BUILD_DOCS=ON).
# How to build docs:
#   cmake -B build -G Ninja -DKANARIV_BUILD_DOCS=ON
#   cmake --build build --target doc-check
#     generates HTML to build/docs/docs/html and XML to build/docs/docs/xml
# Requires Doxygen when KANARIV_BUILD_DOCS=ON. Graphviz/dot remains optional.
# Single Doxyfile template: docs/Doxyfile.in (configured via configure_file @VAR@).

function(kanariv_add_docs)
  find_package(Doxygen REQUIRED OPTIONAL_COMPONENTS dot)
  if(DOXYGEN_DOT_FOUND)
    set(DOXYGEN_HAVE_DOT YES)
  else()
    set(DOXYGEN_HAVE_DOT NO)
  endif()
  set(DOXYGEN_WARNING_LOG "${CMAKE_CURRENT_BINARY_DIR}/docs/doxygen-warnings.log")

  # Doxyfile template (configured via configure_file @VAR@ substitution)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    @ONLY
  )
  set(_kanariv_doxyfile "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")

  doxygen_add_docs(doc CONFIG_FILE "${_kanariv_doxyfile}" COMMENT "Generating Doxygen docs")
  if(TARGET doc)
    # Doxygen on Windows does not auto-create intermediate dirs for nested HTML_OUTPUT;
    # ensure the build-tree output directory exists (docs stay out of the source tree).
    # HTML_OUTPUT in docs/Doxyfile.in is "docs/html" relative to OUTPUT_DIRECTORY
    # (= binary dir), so this matches the configured Doxyfile exactly.
    add_custom_target(doc_mkdir
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/docs/html"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/docs/xml"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/docs"
      COMMENT "Preparing docs output directory"
      VERBATIM
    )
    add_dependencies(doc doc_mkdir)

    add_custom_target(doc-check
      COMMAND ${CMAKE_COMMAND}
        -DHTML_INDEX="${CMAKE_CURRENT_BINARY_DIR}/docs/html/index.html"
        -DXML_INDEX="${CMAKE_CURRENT_BINARY_DIR}/docs/xml/index.xml"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckDocs.cmake"
      DEPENDS doc
      COMMENT "Checking generated documentation outputs"
      VERBATIM
    )
  endif()
endfunction()
