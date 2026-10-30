# SkyBuild, the player kit's build driver (docs/tools/skybuild.md). It reads the disc with the runtime's XDVDFS reader and
# default.xex with XenonUtils (without the disassembler); XenonRecomp and XenosRecomp run as separate programs.
if(WIN32)
    # UTF-8 as the process code page, as for the game (SSARecomp.manifest); the kit's version in the exe's details.
    set(SSA_KIT_VERSION "0.0.0" CACHE STRING "The kit's version, in SkyBuild.exe's VERSIONINFO (tools/make_kit.sh)")
    string(REPLACE "." "," SKYBUILD_VERSION_PARTS "${SSA_KIT_VERSION}.0")
    configure_file(skybuild.rc.in skybuild.rc @ONLY)
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_win32.cpp ${CMAKE_CURRENT_BINARY_DIR}/skybuild.rc)
else()
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_posix.cpp)
endif()
add_executable(SkyBuild main.cpp process.cpp sha256.cpp igz_shaders.cpp game_icon.cpp
    ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
target_include_directories(SkyBuild PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_include_directories(SkyBuild SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb) # stb's own warnings are not ours
target_link_libraries(SkyBuild PRIVATE SSAArtReader SSAXenonUtils)
if(WIN32)
    target_compile_definitions(SkyBuild PRIVATE _WIN32_WINNT=0x0A00) # Windows 10
    target_link_libraries(SkyBuild PRIVATE winmm)
endif()
add_executable(SSAVersionResourceTest version_resource_test.cpp)
