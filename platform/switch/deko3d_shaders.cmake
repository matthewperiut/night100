# uam produces Maxwell machine code; these are not runtime OpenGL shaders.
find_program(NIGHT100_UAM uam HINTS "$ENV{DEVKITPRO}/tools/bin" /opt/devkitpro/tools/bin REQUIRED)
set(NIGHT100_DEKO_SHADER_FILES)
foreach(NIGHT100_DEKO_SHADER IN ITEMS native.vert screen.vert native.frag video.vert video.frag render_texture.frag)
    if(NIGHT100_DEKO_SHADER MATCHES "[.]vert$")
        set(NIGHT100_DEKO_STAGE vert)
    else()
        set(NIGHT100_DEKO_STAGE frag)
    endif()
    set(NIGHT100_DEKO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/romfs/shaders/${NIGHT100_DEKO_SHADER}.dksh")
    add_custom_command(OUTPUT "${NIGHT100_DEKO_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/romfs/shaders"
        COMMAND "${NIGHT100_UAM}" -s "${NIGHT100_DEKO_STAGE}" -o "${NIGHT100_DEKO_OUTPUT}"
            "${NIGHT100_SOURCE_ROOT}/shaders/switch/${NIGHT100_DEKO_SHADER}"
        DEPENDS "${NIGHT100_SOURCE_ROOT}/shaders/switch/${NIGHT100_DEKO_SHADER}"
        VERBATIM)
    list(APPEND NIGHT100_DEKO_SHADER_FILES "${NIGHT100_DEKO_OUTPUT}")
endforeach()
add_custom_target(night100_deko3d_shaders DEPENDS ${NIGHT100_DEKO_SHADER_FILES})
