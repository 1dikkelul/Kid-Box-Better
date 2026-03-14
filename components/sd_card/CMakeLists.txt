# This file integrates the sd_card component into the ESP-IDF build.

# Register the component with the build system.
idf_component_register(
    # List the source files to be compiled.
    SRCS "sd_card.c"

    # Add the current directory to the include path.
    # This allows other components (like 'main') to find "sd_card.h".
    INCLUDE_DIRS "."

    # Specify the components this one requires.
    REQUIRES driver fatfs sdmmc
)