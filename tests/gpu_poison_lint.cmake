# ctest: textures, uninitialized buffers and render passes in src/ go through render/GpuPoison.hpp
# (makeTexture, makeBuffer, makeRenderEncoder), so REFORCE_GPU_POISON covers all of them.
# Input: SRC_DIR.
file(GLOB_RECURSE sources "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.mm" "${SRC_DIR}/*.hpp" "${SRC_DIR}/*.h")
set(bad "")
foreach(f IN LISTS sources)
    if(f MATCHES "/render/GpuPoison\\.cpp$")
        continue()
    endif()
    file(STRINGS "${f}" hits REGEX
        "->newTexture\\(|->renderCommandEncoder\\(|->newBuffer\\((NS::UInteger|[0-9]|sizeof)|newTextureWithDescriptor|renderCommandEncoderWithDescriptor")
    foreach(h IN LISTS hits)
        file(RELATIVE_PATH rel "${SRC_DIR}" "${f}")
        string(STRIP "${h}" h)
        string(APPEND bad "  src/${rel}: ${h}\n")
    endforeach()
endforeach()
if(bad)
    message(FATAL_ERROR "Use makeTexture / makeBuffer / makeRenderEncoder (render/GpuPoison.hpp) instead of:\n${bad}")
endif()
