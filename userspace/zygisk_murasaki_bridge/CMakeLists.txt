cmake_minimum_required(VERSION 3.14)
project(murasaki_zygisk_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(murasaki_zygisk_bridge SHARED
    src/module.cpp
    src/bridge.cpp
)

target_include_directories(murasaki_zygisk_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(murasaki_zygisk_bridge PRIVATE
    ANDROID
)

target_compile_options(murasaki_zygisk_bridge PRIVATE
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_libraries(murasaki_zygisk_bridge PRIVATE
    log
    dl
)

