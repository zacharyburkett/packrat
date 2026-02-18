cmake_minimum_required(VERSION 3.20)

project(packrat VERSION 0.1.0 LANGUAGES C)

option(PACKRAT_BUILD_CLI "Build packrat CLI" ON)
option(PACKRAT_BUILD_GUI_CORE "Build reusable packrat GUI core library" OFF)
option(PACKRAT_BUILD_GUI "Build packrat GUI tool (SDL3 + Nuklear)" OFF)
option(
    PACKRAT_NUKLEAR_AUTO_FETCH
    "Allow fission to auto-fetch Nuklear when PACKRAT_BUILD_GUI(_CORE)=ON"
    ON
)
set(
    PACKRAT_FISSION_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/../fission"
    CACHE PATH
    "Path to fission repository root"
)
set(
    PACKRAT_NUKLEAR_INCLUDE_DIR
    ""
    CACHE PATH
    "Optional path to directory containing nuklear.h"
)

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

if(PACKRAT_BUILD_GUI)
    set(PACKRAT_BUILD_GUI_CORE ON)
endif()

if(PACKRAT_BUILD_GUI_CORE)
    if(NOT TARGET fission::fission)
        if(EXISTS "${PACKRAT_FISSION_PATH}/CMakeLists.txt")
            set(FISSION_NUKLEAR_AUTO_FETCH ${PACKRAT_NUKLEAR_AUTO_FETCH} CACHE BOOL "" FORCE)
            if(
                DEFINED PACKRAT_NUKLEAR_INCLUDE_DIR AND
                NOT PACKRAT_NUKLEAR_INCLUDE_DIR STREQUAL ""
            )
                set(FISSION_NUKLEAR_INCLUDE_DIR "${PACKRAT_NUKLEAR_INCLUDE_DIR}" CACHE PATH "" FORCE)
            endif()
            add_subdirectory(
                "${PACKRAT_FISSION_PATH}"
                "${CMAKE_BINARY_DIR}/_deps/fission"
                EXCLUDE_FROM_ALL
            )
        else()
            find_package(fission QUIET CONFIG)
        endif()
    endif()

    if(NOT TARGET fission::fission)
        message(FATAL_ERROR
            "PACKRAT_BUILD_GUI_CORE requires fission::fission. "
            "Set PACKRAT_FISSION_PATH or install fission."
        )
    endif()

    add_library(packrat_gui_core
        src/gui/app.c
    )
    add_library(packrat::gui_core ALIAS packrat_gui_core)
    target_compile_features(packrat_gui_core PUBLIC c_std_11)
    target_include_directories(
        packrat_gui_core
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(
        packrat_gui_core
        PUBLIC
            packrat
            fission::fission
    )
endif()

if(PACKRAT_BUILD_GUI)
    find_package(SDL3 QUIET CONFIG)
    if(NOT SDL3_FOUND)
        find_package(SDL3 REQUIRED)
    endif()

    set(PACKRAT_SDL3_TARGET "")
    if(TARGET SDL3::SDL3)
        set(PACKRAT_SDL3_TARGET SDL3::SDL3)
    elseif(TARGET SDL3::SDL3-static)
        set(PACKRAT_SDL3_TARGET SDL3::SDL3-static)
    elseif(TARGET SDL3::SDL3-shared)
        set(PACKRAT_SDL3_TARGET SDL3::SDL3-shared)
    else()
        message(FATAL_ERROR "SDL3 target not found. Expected SDL3::SDL3 or SDL3::* variants.")
    endif()

    find_package(OpenGL REQUIRED)
    if(NOT TARGET fission::nuklear_render)
        message(FATAL_ERROR
            "PACKRAT_BUILD_GUI requires fission::nuklear_render (OpenGL path)."
        )
    endif()

    add_executable(packrat_gui
        apps/gui/main.c
        src/gui/nuklear_backend.c
    )
    target_compile_features(packrat_gui PRIVATE c_std_11)
    target_include_directories(
        packrat_gui
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(
        packrat_gui
        PRIVATE
            packrat_gui_core
            fission::nuklear_render
            OpenGL::GL
            ${PACKRAT_SDL3_TARGET}
    )

    if(APPLE)
        target_compile_definitions(packrat_gui PRIVATE GL_SILENCE_DEPRECATION)
    endif()
endif()
