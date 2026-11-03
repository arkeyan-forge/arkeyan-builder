# Arkeyan-Builder, the player kit's build driver (docs/tools/arkeyan-builder.md). It reads the disc with the runtime's XDVDFS reader and
# default.xex with XenonUtils (without the disassembler); XenonRecomp and XenosRecomp run as separate programs. Its
# --menu-music decodes the game's menu theme with the runtime's XMA decoder (FFmpeg's WMA Pro, LGPL: cmake/ffmpeg.cmake,
# the same static FFmpeg as the game's; SSA_FFMPEG=OFF builds it without, and --menu-music then fails).
if(WIN32)
    # UTF-8 as the process code page, as for the game (SSARecomp.manifest); the kit's version in the exe's details.
    set(SSA_KIT_VERSION "0.0.0" CACHE STRING "The kit's version, in Arkeyan-Builder.exe's VERSIONINFO (tools/make_kit.sh)")
    string(REPLACE "." "," SKYBUILD_VERSION_PARTS "${SSA_KIT_VERSION}.0")
    configure_file(arkeyan-builder.rc.in arkeyan-builder.rc @ONLY)
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_win32.cpp ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder.rc)
else()
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_posix.cpp)
endif()
add_executable(Arkeyan-Builder main.cpp process.cpp sha256.cpp igz_shaders.cpp game_icon.cpp menu_music.cpp shader_cache_literals.cpp
    ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
target_include_directories(Arkeyan-Builder PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_include_directories(Arkeyan-Builder SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb) # stb's own warnings are not ours
target_link_libraries(Arkeyan-Builder PRIVATE SSAArtReader SSAXenonUtils)
if(WIN32)
    target_compile_definitions(Arkeyan-Builder PRIVATE _WIN32_WINNT=0x0A00) # Windows 10
    target_link_libraries(Arkeyan-Builder PRIVATE winmm)
endif()
add_executable(SSAVersionResourceTest version_resource_test.cpp accents_test.cpp process.cpp sha256.cpp
    igz_shaders.cpp game_icon.cpp menu_music.cpp shader_cache_literals.cpp ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp
    ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
target_include_directories(SSAVersionResourceTest PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_include_directories(SSAVersionResourceTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
target_link_libraries(SSAVersionResourceTest PRIVATE SSAArtReader SSAXenonUtils)
if(NOT WIN32 OR CMAKE_HOST_WIN32)
    target_compile_definitions(SSAVersionResourceTest PRIVATE SKY_ACCENTS_COMPILER="${CMAKE_CXX_COMPILER}")
else()
    # A cross compiler cannot run inside the Windows process; Wine can supply a Windows-hosted one explicitly.
    target_compile_definitions(SSAVersionResourceTest PRIVATE SKY_ACCENTS_COMPILER="")
endif()
if(WIN32)
    target_compile_definitions(SSAVersionResourceTest PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(SSAVersionResourceTest PRIVATE winmm)
endif()

add_executable(SSASkyBuildTest arkeyan_builder_test.cpp igz_shaders.cpp sha256.cpp shader_cache_literals.cpp game_icon.cpp)
target_include_directories(SSASkyBuildTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
target_link_libraries(SSASkyBuildTest PRIVATE SSAArtReader)
sky_quality(SSASkyBuildTest)
add_test(NAME SSASkyBuildTest COMMAND SSASkyBuildTest ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test)
set_tests_properties(SSASkyBuildTest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test
    TIMEOUT 120)
# Source metadata is read only; generated fixtures stay in the build folder.
set_property(TEST SSASkyBuildTest PROPERTY ENVIRONMENT "SKYLANDERS_DATA=${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test/data")
include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake)
ssa_setup_ffmpeg(Arkeyan-Builder)
ssa_setup_ffmpeg(SSAVersionResourceTest)

# The menu music's readers on synthetic files; with SSA_ISO / GIANTS_ISO / SWAPFORCE_ISO, each game's theme found on its
# disc and decoded (docs/runtime/host/game_music.md): ./ArkeyanBuilderMusicTest
if(WIN32)
    set(SKYBUILD_TEST_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_win32.cpp)
else()
    set(SKYBUILD_TEST_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_posix.cpp)
endif()
add_executable(ArkeyanBuilderMusicTest EXCLUDE_FROM_ALL menu_music_test.cpp menu_music.cpp
    ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp ${SKYBUILD_TEST_PLATFORM})
target_include_directories(ArkeyanBuilderMusicTest PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_link_libraries(ArkeyanBuilderMusicTest PRIVATE SSAArtReader)
ssa_setup_ffmpeg(ArkeyanBuilderMusicTest)

# Real install steps with synthetic inputs and a fake child runner; never launches a game.
add_executable(SSASkyBuildGuardTest install_guard_test.cpp process.cpp sha256.cpp igz_shaders.cpp shader_cache_literals.cpp game_icon.cpp menu_music.cpp
    ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
target_include_directories(SSASkyBuildGuardTest PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_include_directories(SSASkyBuildGuardTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
target_link_libraries(SSASkyBuildGuardTest PRIVATE SSAArtReader SSAXenonUtils)
if(WIN32)
    target_compile_definitions(SSASkyBuildGuardTest PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(SSASkyBuildGuardTest PRIVATE winmm)
endif()
sky_quality(SSASkyBuildGuardTest)
add_test(NAME SSASkyBuildGuardTest COMMAND SSASkyBuildGuardTest ${CMAKE_CURRENT_BINARY_DIR}/install-guard-test)
set_tests_properties(SSASkyBuildGuardTest PROPERTIES TIMEOUT 120)
ssa_setup_ffmpeg(SSASkyBuildGuardTest)
