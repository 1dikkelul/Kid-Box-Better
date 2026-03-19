# This file integrates the SquareLine-generated code into the ESP-IDF build.
# It will NOT be overwritten by SquareLine Studio exports.

# Set the path to the generated files.
# Point SquareLine Studio to export into this 'generated' subdirectory.
set(SL_SRC_DIR "generated")

# Build the source list from SquareLine's generated filelist.
# This prevents stale hardcoded ui_img_*.c entries from breaking the build
# when assets are added/removed in SquareLine Studio.
set(SL_FILELIST "${CMAKE_CURRENT_LIST_DIR}/${SL_SRC_DIR}/filelist.txt")
set(COMPONENT_SRCS "")

if(EXISTS "${SL_FILELIST}")
    # Reconfigure automatically when SquareLine updates filelist.txt.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SL_FILELIST}")

    file(STRINGS "${SL_FILELIST}" SL_FILE_NAMES)
    foreach(SL_FILE IN LISTS SL_FILE_NAMES)
        string(STRIP "${SL_FILE}" SL_FILE)
        if(SL_FILE STREQUAL "")
            continue()
        endif()
        list(APPEND COMPONENT_SRCS "${SL_SRC_DIR}/${SL_FILE}")
    endforeach()
else()
    # Fallback for first-time setup if filelist.txt has not been exported yet.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        file(GLOB COMPONENT_SRCS CONFIGURE_DEPENDS "${SL_SRC_DIR}/*.c")
    endif()
endif()

# Register the component with the build system.
idf_component_register(
    # List the source files to be compiled.
    SRCS "${COMPONENT_SRCS}"

    # Add the 'generated' directory to the include path.
    # This allows other components (like 'main') to find "ui.h".
    INCLUDE_DIRS "${SL_SRC_DIR}"

    # Specify that this component requires the 'lvgl' component.
    REQUIRES lvgl
)