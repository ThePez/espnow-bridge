# STM32 side of the console. The ESP32 builds this folder as a component through CMakeLists.txt
# instead. This file is include()d from the STM32 project's CMakeLists.txt, so every path is
# relative to this file, not to the project.
set(EMBEDDED_CONSOLE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${EMBEDDED_CONSOLE_DIR}/console.c
    ${EMBEDDED_CONSOLE_DIR}/commands.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${EMBEDDED_CONSOLE_DIR}
)
