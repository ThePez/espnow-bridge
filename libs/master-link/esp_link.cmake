# This file lives in libs/master-link and is include()d from the STM32 project's CMakeLists.txt,
# so every path is relative to this file, not to the project.
set(ESP_LINK_DIR ${CMAKE_CURRENT_LIST_DIR})

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${ESP_LINK_DIR}/esp_link.c
)

# Add include paths (the module itself, and the protocol header shared with the ESP)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${ESP_LINK_DIR}
    ${ESP_LINK_DIR}/../spi-proto
)

# The SPI itself is set up in CubeMX. If CubeMX is not compiling the HAL SPI driver yet
# (no SPI enabled), add it here so that SPI_HandleTypeDef exists and this module builds.
get_target_property(ESP_LINK_MX_DRIVER_SRCS STM32_Drivers SOURCES)

if(NOT "${ESP_LINK_MX_DRIVER_SRCS}" MATCHES "stm32f4xx_hal_spi\\.c")
    set(ESP_LINK_HAL_SPI_SRC
        ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c
    )
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${ESP_LINK_HAL_SPI_SRC})
    set_source_files_properties(${ESP_LINK_HAL_SPI_SRC} ${ESP_LINK_DIR}/esp_link.c
        PROPERTIES COMPILE_DEFINITIONS HAL_SPI_MODULE_ENABLED
    )
endif()
