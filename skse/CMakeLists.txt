cmake_minimum_required(VERSION 3.24)
project(jc2mech_skse VERSION 0.1.0 LANGUAGES C CXX)

# Separate from the root build: needs vcpkg (manifest in this folder) for CommonLibSSE-NG's deps.
# Configure with the presets in this folder.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(FetchContent)

# v9.x supports runtime 1.7.104 (Address Library v13+). Keep all runtimes on so the release's
# prebuilt CommonLib bundle can be used instead of a 15-minute source build.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibSSE-NG.git
    GIT_TAG v9.3.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(CommonLibSSE)
if(NOT COMMAND add_commonlibsse_plugin)  # only the prebuilt path includes the helper itself
    include(${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake)
endif()

# Dear ImGui (vcpkg, dx11 backend) for the in-game options overlay, src/menu.cpp.
find_package(imgui CONFIG REQUIRED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)

# Players see it as Skydive (Skydive.dll, Skydive.ini, Skydive.log); the code keeps jc2mech.
add_commonlibsse_plugin(jc2mech
    NAME Skydive
    AUTHOR apinanaivot
    SOURCES
        src/plugin.cpp
        src/skyrim_host.cpp
        src/animation.cpp
        src/visuals.cpp
        src/grapple.cpp
        src/sounds.cpp
        src/body.cpp
        src/settings.cpp
        src/menu.cpp
        src/spells.cpp)
target_compile_features(jc2mech PRIVATE cxx_std_23)
target_precompile_headers(jc2mech PRIVATE src/pch.h)
target_link_libraries(jc2mech PRIVATE jc2core imgui::imgui d3d11 dxgi)
if(MSVC)
    target_compile_options(jc2mech PRIVATE /W4 /permissive- /utf-8)
endif()
set_target_properties(jc2mech PROPERTIES FOLDER "skse" OUTPUT_NAME Skydive)

# Copy the DLL (and the default ini, if none is there yet) into Skyrim after each build.
set(SKYRIM_DATA_DIR "" CACHE PATH "Skyrim's Data folder; the plugin is copied to Data/SKSE/Plugins")
if(SKYRIM_DATA_DIR)
    set(plugin_dir "${SKYRIM_DATA_DIR}/SKSE/Plugins")
    add_custom_command(TARGET jc2mech POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${plugin_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:jc2mech>" "${plugin_dir}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/Skydive.ini -DDST=${plugin_dir}/Skydive.ini
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake
        VERBATIM)
endif()
