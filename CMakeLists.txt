# SkyBuild, the player kit's build driver (docs/tools/skybuild.md). It reads the disc with the runtime's XDVDFS reader and
# default.xex with XenonUtils (without the disassembler); XenonRecomp and XenosRecomp run as separate programs.
if(WIN32)
    # UTF-8 as the process code page, as for the game (SSARecomp.manifest).
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_win32.cpp skybuild.rc)
else()
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_posix.cpp)
endif()
add_executable(SkyBuild main.cpp process.cpp sha256.cpp igz_shaders.cpp ${CMAKE_SOURCE_DIR}/runtime/host/art/alchemy_archive.cpp
    ${CMAKE_SOURCE_DIR}/runtime/host/art/xenos_texture.cpp ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${CMAKE_SOURCE_DIR}/runtime/host/collection/datadir.cpp ${SKYBUILD_PLATFORM})
target_include_directories(SkyBuild PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_link_libraries(SkyBuild PRIVATE SSAXenonUtils)
if(WIN32)
    target_compile_definitions(SkyBuild PRIVATE _WIN32_WINNT=0x0A00) # Windows 10
    target_link_libraries(SkyBuild PRIVATE winmm)
endif()
