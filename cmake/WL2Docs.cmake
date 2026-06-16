# Optional `docs` target: Doxygen API/reference generation.
#
# Included from the root CMakeLists.txt (same scope), so CMAKE_CURRENT_SOURCE_DIR
# and CMAKE_CURRENT_BINARY_DIR refer to the project root and its build tree.

if(WL2_BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in
            ${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile
            @ONLY)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/docs/doxygen/html/index.html
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating Winglib2 Doxygen documentation"
            VERBATIM)
    else()
        message(STATUS "Doxygen not found; docs target disabled")
    endif()
endif()
