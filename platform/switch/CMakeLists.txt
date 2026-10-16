cmake_minimum_required(VERSION 3.24)
project(Night100Switch VERSION 1.0.0 LANGUAGES C CXX)
if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=../../cmake/switch/toolchain.cmake")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(NIGHT100_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NIGHT100_SOURCE_ROOT "${NIGHT100_ROOT}")
set(PS2_ROOT "${NIGHT100_ROOT}/vendor/PS2Recomp")
set(RUNTIME_ROOT "${PS2_ROOT}/ps2xRuntime")
set(SSE2NEON_SOURCE "${NIGHT100_ROOT}/build/toolchain/_deps/sse2neon-src" CACHE PATH "Pinned sse2neon checkout")
if(NOT EXISTS "${SSE2NEON_SOURCE}/sse2neon.h")
    message(FATAL_ERROR "Run ./night100 bootstrap to populate the pinned portability headers")
endif()
option(NIGHT100_DEKO_VALIDATION "Enable deko3d driver validation during Switch bring-up" ON)
option(NIGHT100_GPU_PROBE_ONLY "Configure only the assets-free GPU hardware probe" OFF)
if(NIGHT100_DEKO_VALIDATION)
    set(NIGHT100_DEKO_LIBRARY deko3dd)
else()
    set(NIGHT100_DEKO_LIBRARY deko3d)
endif()
add_compile_definitions(USE_SSE2NEON PS2X_HAS_FFMPEG=0 PS2X_ENABLE_IOP_RPC_TRACE=0)
option(NIGHT100_DIRTY_LINES "Mark a 64-byte guest RAM line on every ordinary guest store (decoupled-render probe; default off)" OFF)
if(NIGHT100_DIRTY_LINES)
    # Global: every translation unit must see the same ps2TraceGuestWrite body.
    add_compile_definitions(NIGHT100_DIRTY_LINES=1)
endif()
add_compile_options(-O3 -ffp-contract=off -mno-outline-atomics)
# ccache can only cache precompiled-header users when the preprocessed output
# keeps the PCH reference (sloppiness=pch_defines,time_macros is set in ccache.conf).
if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
    add_compile_options(-fpch-preprocess)
endif()
# Profile-guided optimisation. Opt-in through the environment so the default build is unchanged:
#   NIGHT100_PGO_DIR=<dir> NIGHT100_PGO_MODE=gen  -> instrumented build writes .gcda files to <dir>
#   NIGHT100_PGO_DIR=<dir> NIGHT100_PGO_MODE=use  -> optimise with the .gcda files in <dir>
set(NIGHT100_PGO_DIR "$ENV{NIGHT100_PGO_DIR}")
if(NIGHT100_PGO_DIR)
    if("$ENV{NIGHT100_PGO_MODE}" STREQUAL "gen")
        # Counters are dumped by SwitchPgo (__gcov_dump) at replay end and on L+R+Down.
        add_compile_definitions(NIGHT100_PGO_GEN=1 "NIGHT100_PGO_RUNTIME_DIR=\"${NIGHT100_PGO_DIR}\"")
        add_compile_options(-fprofile-generate=${NIGHT100_PGO_DIR})
        add_compile_options(-g0)  # drop debug info: keeps the instrumented NRO near normal size for transfer
        add_link_options(-fprofile-generate=${NIGHT100_PGO_DIR})
    else()
        add_compile_options(-fprofile-use=${NIGHT100_PGO_DIR} -fprofile-partial-training -Wno-missing-profile)
    endif()
endif()
include_directories("${SSE2NEON_SOURCE}" "${RUNTIME_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
include("${CMAKE_CURRENT_SOURCE_DIR}/deko3d_shaders.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/deko3d_probe.cmake")
if(NIGHT100_GPU_PROBE_ONLY)
    return()
endif()
find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
add_library(night100_switch_host STATIC host/host.cpp)
target_include_directories(night100_switch_host PUBLIC include)
target_link_libraries(night100_switch_host PUBLIC nx PNG::PNG)
add_subdirectory("${PS2_ROOT}/ps2xIOP" "${CMAKE_CURRENT_BINARY_DIR}/ps2_iop")
set(RUNTIME_FILES game_overrides.cpp ps2_audio.cpp ps2_audio_vag.cpp ps2_iop_host.cpp ps2_memory.cpp ps2_pad.cpp
    ps2_rom_device.cpp ps2_vfs.cpp ps2_runtime.cpp ps2_runtime_state.cpp ps2_runtime_machine_state.cpp
    ps2_vif1_interpreter.cpp games_database.cpp resource_disc.cpp
    gs/ps2_gif_arbiter.cpp gs/ps2_gs_memory.cpp gs/gs_frontend.cpp gs/gs_cpu_backend.cpp
    vu/ps2_vu1_core.cpp vu/ps2_vu1_upper.cpp vu/ps2_vu1_lower.cpp)
list(TRANSFORM RUNTIME_FILES PREPEND "${RUNTIME_ROOT}/src/lib/")
file(GLOB_RECURSE KERNEL_FILES CONFIGURE_DEPENDS "${RUNTIME_ROOT}/src/lib/Kernel/*.cpp")
add_library(ps2_runtime STATIC ${RUNTIME_FILES} ${KERNEL_FILES})
target_include_directories(ps2_runtime PUBLIC "${RUNTIME_ROOT}/include" "${RUNTIME_ROOT}/src/lib/Kernel")
target_link_libraries(ps2_runtime PUBLIC ps2_iop night100_switch_host ZLIB::ZLIB)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${NIGHT100_ROOT}/generated/*.cpp")
add_library(night100_switch_game STATIC ${GAME_SOURCES})
target_include_directories(night100_switch_game PUBLIC "${NIGHT100_ROOT}/generated")
target_link_libraries(night100_switch_game PUBLIC ps2_runtime)
# Precompiled headers embed per-directory state, so ccache cannot share the game
# batches between build directories; with ccache in use, plain includes are shared
# and the unity batches of 16 already amortize the header parse.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
    target_precompile_headers(night100_switch_game PRIVATE <ps2_runtime_macros.h> <ps2_runtime.h>)
endif()
set_target_properties(night100_switch_game PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 16)
set_source_files_properties("${NIGHT100_ROOT}/generated/register_functions.cpp" PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE COMPILE_OPTIONS "-O0")
set(NIGHT100_SWITCH_PORT_SOURCES
    src/platform/switch/main.cpp src/platform/switch/telemetry.cpp src/platform/switch/cpu_sampler.cpp src/platform/switch/cpu_benchmark.cpp src/platform/switch/stall_watchdog.cpp
    src/overrides.cpp src/native_scene.cpp src/native_cpu.cpp src/native_particles.cpp
    src/controller_prompts.cpp src/platform/switch/video.cpp src/movie_audio.cpp src/resource_aliases.cpp
    src/platform/switch/replay_ui.cpp src/platform/switch/clock_boost.cpp src/platform/switch/pgo.cpp src/replay_harness.cpp src/input_replay.cpp
    src/replay_clock.cpp src/replay_catalog.cpp src/machine_snapshot.cpp src/frame_telemetry.cpp
    src/native_vu1_lastprogram.cpp src/frame_phases.cpp src/widescreen.cpp src/render_image.cpp src/graphics/deko3d_backend.cpp)
list(TRANSFORM NIGHT100_SWITCH_PORT_SOURCES PREPEND "${NIGHT100_ROOT}/")
if(EXISTS "${NIGHT100_ROOT}/src/platform/switch/main.cpp")
    set(NIGHT100_FFMPEG_PREFIX "${NIGHT100_ROOT}/build/switch-ffmpeg/install" CACHE PATH "NVDEC-enabled Switch FFmpeg installation")
    foreach(component IN ITEMS avformat avcodec avutil)
        if(NOT EXISTS "${NIGHT100_FFMPEG_PREFIX}/lib/lib${component}.a")
            message(FATAL_ERROR "Build Switch NVDEC FFmpeg before linking: missing lib${component}.a")
        endif()
    endforeach()
    add_executable(night100 ${NIGHT100_SWITCH_PORT_SOURCES})
    target_include_directories(night100 PRIVATE "${NIGHT100_ROOT}/src" "${NIGHT100_ROOT}/generated" "${NIGHT100_FFMPEG_PREFIX}/include")
    target_compile_definitions(night100 PRIVATE NIGHT100_DEFAULT_CONTROLLER_PROFILE="sw")
    # Registration references every generated function; whole-archive retains the table.
    target_link_libraries(night100 PRIVATE -Wl,--whole-archive night100_switch_game -Wl,--no-whole-archive ps2_runtime
        "${NIGHT100_FFMPEG_PREFIX}/lib/libavformat.a" "${NIGHT100_FFMPEG_PREFIX}/lib/libavcodec.a"
        "${NIGHT100_FFMPEG_PREFIX}/lib/libavutil.a" ${NIGHT100_DEKO_LIBRARY} ZLIB::ZLIB nx m)
    target_link_options(night100 PRIVATE -fPIE)
    set_target_properties(night100 PROPERTIES SUFFIX ".elf")
    add_dependencies(night100 night100_deko3d_shaders)
    nx_generate_nacp(OUTPUT night100.nacp NAME "Night100" AUTHOR "matthewperiut" VERSION "1.0.0")
    # Track generated shaders and authored controls as NRO inputs. Passing only
    # a directory to elf2nro would leave stale embedded assets after edits.
    set(NIGHT100_ROMFS_FILES ${NIGHT100_DEKO_SHADER_FILES})
    file(GLOB NIGHT100_CONTROL_FILES CONFIGURE_DEPENDS
        "${NIGHT100_ROOT}/controllers/sw/*.png" "${NIGHT100_ROOT}/controllers/sw/*.svg")
    list(APPEND NIGHT100_CONTROL_FILES "${NIGHT100_ROOT}/controllers/prompts.map")
    foreach(source IN LISTS NIGHT100_CONTROL_FILES)
        file(RELATIVE_PATH relative "${NIGHT100_ROOT}" "${source}")
        set(output "${CMAKE_CURRENT_BINARY_DIR}/romfs/${relative}")
        get_filename_component(directory "${output}" DIRECTORY)
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${output}"
            DEPENDS "${source}" VERBATIM)
        list(APPEND NIGHT100_ROMFS_FILES "${output}")
    endforeach()
    add_custom_target(night100_switch_romfs DEPENDS ${NIGHT100_ROMFS_FILES})
    set_target_properties(night100_switch_romfs PROPERTIES
        DKP_ASSET_FOLDER "${CMAKE_CURRENT_BINARY_DIR}/romfs" DKP_ASSET_FILES "${NIGHT100_ROMFS_FILES}")
    nx_create_nro(night100 NACP "${CMAKE_CURRENT_BINARY_DIR}/night100.nacp" ROMFS night100_switch_romfs)
endif()
