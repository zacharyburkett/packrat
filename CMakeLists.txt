cmake_minimum_required(VERSION 3.20)

project(packrat VERSION 0.1.0 LANGUAGES C)

option(PACKRAT_BUILD_CLI "Build packrat CLI" ON)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBPNG IMPORTED_TARGET QUIET libpng)
endif()
if(NOT TARGET PkgConfig::LIBPNG)
    find_package(PNG REQUIRED)
endif()

add_library(packrat
    src/build.c
    src/manifest.c
    src/runtime.c
    src/status.c
)

add_library(packrat::packrat ALIAS packrat)

target_include_directories(packrat
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(packrat PUBLIC c_std_11)
if(TARGET PkgConfig::LIBPNG)
    target_link_libraries(packrat PUBLIC PkgConfig::LIBPNG)
else()
    target_link_libraries(packrat PUBLIC PNG::PNG)
endif()

if(PACKRAT_BUILD_CLI)
    add_executable(packrat_cli
        src/cli/main.c
    )
    set_target_properties(packrat_cli PROPERTIES OUTPUT_NAME packrat)
    target_link_libraries(packrat_cli PRIVATE packrat)
endif()
