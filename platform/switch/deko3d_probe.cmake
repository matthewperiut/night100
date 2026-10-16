# Assets-free hardware fixtures share the real renderer and shader binaries.
# This target deliberately has no dependency on translated game code, the EE/IOP
# runtime, the Raylib compatibility host, FFmpeg, or original disc resources.
add_executable(night100_gpu_probe
    "${NIGHT100_ROOT}/src/platform/switch/gpu_probe.cpp"
    "${NIGHT100_ROOT}/src/platform/switch/telemetry.cpp"
    "${NIGHT100_ROOT}/src/graphics/deko3d_backend.cpp"
    "${RUNTIME_ROOT}/src/lib/gs/gs_cpu_backend.cpp"
    "${RUNTIME_ROOT}/src/lib/gs/ps2_gs_memory.cpp")
target_include_directories(night100_gpu_probe PRIVATE "${NIGHT100_ROOT}/src" "${RUNTIME_ROOT}/include")
target_link_libraries(night100_gpu_probe PRIVATE ${NIGHT100_DEKO_LIBRARY} nx m)
target_link_options(night100_gpu_probe PRIVATE -fPIE)
set_target_properties(night100_gpu_probe PROPERTIES SUFFIX ".elf" OUTPUT_NAME "night100-gpu-probe")
add_dependencies(night100_gpu_probe night100_deko3d_shaders)
set(NIGHT100_GPU_PROBE_ROMFS_FILES)
foreach(shader IN LISTS NIGHT100_DEKO_SHADER_FILES)
    get_filename_component(filename "${shader}" NAME)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/probe-romfs/shaders/${filename}")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/probe-romfs/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${shader}" "${output}"
        DEPENDS "${shader}" VERBATIM)
    list(APPEND NIGHT100_GPU_PROBE_ROMFS_FILES "${output}")
endforeach()
add_custom_target(night100_gpu_probe_assets DEPENDS ${NIGHT100_GPU_PROBE_ROMFS_FILES})
set_target_properties(night100_gpu_probe_assets PROPERTIES
    DKP_ASSET_FOLDER "${CMAKE_CURRENT_BINARY_DIR}/probe-romfs"
    DKP_ASSET_FILES "${NIGHT100_GPU_PROBE_ROMFS_FILES}")
nx_generate_nacp(OUTPUT night100-gpu-probe.nacp NAME "Night100 GPU Probe"
    AUTHOR "Night100 contributors" VERSION "0.1.0")
nx_create_nro(night100_gpu_probe OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/night100-gpu-probe.nro"
    NACP "${CMAKE_CURRENT_BINARY_DIR}/night100-gpu-probe.nacp" ROMFS night100_gpu_probe_assets)
