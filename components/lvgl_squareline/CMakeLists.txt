# This file integrates the SquareLine-generated code into the ESP-IDF build.
# It will NOT be overwritten by SquareLine Studio exports.

# Set the path to the generated files.
# Point SquareLine Studio to export into this 'generated' subdirectory.
set(SL_SRC_DIR "generated")

# Find all the C source files that SquareLine created.
if(CMAKE_SCRIPT_MODE_FILE)
    # ESP-IDF parses component requirements in script mode; CONFIGURE_DEPENDS is not allowed there.
    set(COMPONENT_SRCS "")
else()
    file(GLOB_RECURSE COMPONENT_SRCS CONFIGURE_DEPENDS "${SL_SRC_DIR}/*.c")
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