# Arkeyan-Builder, the player kit's build driver (docs/tools/arkeyan-builder.md). It reads the disc with the runtime's XDVDFS reader and
# default.xex with XenonUtils (without the disassembler); XenonRecomp and XenosRecomp run as separate programs. Its
# --menu-music decodes the game's menu theme with the runtime's XMA decoder (FFmpeg's WMA Pro, LGPL: cmake/ffmpeg.cmake,
# the same static FFmpeg as the game's; ARKEYAN_FFMPEG=OFF builds it without, and --menu-music then fails).
if(WIN32)
    # UTF-8 as the process code page, as for the game (SSARecomp.manifest); the kit's version in the exe's details.
    set(ARKEYAN_KIT_VERSION "0.0.0" CACHE STRING "The kit's version, in Arkeyan-Builder.exe's VERSIONINFO (tools/make_kit.sh)")
    string(REPLACE "." "," SKYBUILD_VERSION_PARTS "${ARKEYAN_KIT_VERSION}.0")
    configure_file(arkeyan-builder.rc.in arkeyan-builder.rc @ONLY)
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_win32.cpp ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder.rc)
else()
    set(SKYBUILD_PLATFORM ${CMAKE_SOURCE_DIR}/runtime/platform/platform_posix.cpp)
endif()
# The HD texture step (hd_textures.h): the pack's DDS writer and content hash are the runtime's own.
set(SKYBUILD_HD_SOURCES hd_upscale.cpp hd_textures.cpp ${CMAKE_SOURCE_DIR}/runtime/host/dds.cpp
    ${CMAKE_SOURCE_DIR}/runtime/host/texture_pack.cpp)
# The Windows compiler arrives as a .zip, inflated in process (toolchain_fetch.cpp): miniz's inflate and CRC-32, the same
# public-domain source as the runtime's ArkeyanMiniz (a library of its own here: the runtime is not part of a tools build).
add_library(ArkeyanBuilderMiniz STATIC ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/testing/external/miniz/miniz.c)
target_compile_definitions(ArkeyanBuilderMiniz PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_include_directories(ArkeyanBuilderMiniz SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/testing/external/miniz)
add_executable(Arkeyan-Builder main.cpp edition.cpp fingerprint.cpp process.cpp sha256.cpp toolchain_fetch.cpp igz_shaders.cpp game_icon.cpp menu_music.cpp shader_cache_literals.cpp
    ${SKYBUILD_HD_SOURCES} ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
target_include_directories(Arkeyan-Builder PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_include_directories(Arkeyan-Builder SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)
target_include_directories(Arkeyan-Builder SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb) # stb's own warnings are not ours
target_link_libraries(Arkeyan-Builder PRIVATE ArkeyanArtReader ArkeyanXenonUtils ArkeyanBuilderMiniz)
if(WIN32)
    target_compile_definitions(Arkeyan-Builder PRIVATE _WIN32_WINNT=0x0A00) # Windows 10
    target_link_libraries(Arkeyan-Builder PRIVATE winmm winhttp)
endif()
add_executable(ArkeyanVersionResourceTest version_resource_test.cpp accents_test.cpp edition.cpp fingerprint.cpp process.cpp sha256.cpp toolchain_fetch.cpp
    igz_shaders.cpp game_icon.cpp menu_music.cpp shader_cache_literals.cpp ${SKYBUILD_HD_SOURCES} ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp
    ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
target_include_directories(ArkeyanVersionResourceTest PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_include_directories(ArkeyanVersionResourceTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)
target_include_directories(ArkeyanVersionResourceTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
target_link_libraries(ArkeyanVersionResourceTest PRIVATE ArkeyanArtReader ArkeyanXenonUtils ArkeyanBuilderMiniz)
if(NOT WIN32 OR CMAKE_HOST_WIN32)
    target_compile_definitions(ArkeyanVersionResourceTest PRIVATE SKY_ACCENTS_COMPILER="${CMAKE_CXX_COMPILER}")
else()
    # A cross compiler cannot run inside the Windows process; Wine can supply a Windows-hosted one explicitly.
    target_compile_definitions(ArkeyanVersionResourceTest PRIVATE SKY_ACCENTS_COMPILER="")
endif()
if(WIN32)
    target_compile_definitions(ArkeyanVersionResourceTest PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(ArkeyanVersionResourceTest PRIVATE winmm winhttp)
endif()

add_executable(ArkeyanBuilderTest arkeyan_builder_test.cpp igz_shaders.cpp sha256.cpp shader_cache_literals.cpp game_icon.cpp)
target_include_directories(ArkeyanBuilderTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
target_link_libraries(ArkeyanBuilderTest PRIVATE ArkeyanArtReader)
sky_quality(ArkeyanBuilderTest)
add_test(NAME ArkeyanBuilderTest COMMAND ArkeyanBuilderTest ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test)
set_tests_properties(ArkeyanBuilderTest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test
    TIMEOUT 120)
# Source metadata is read only; generated fixtures stay in the build folder.
set_property(TEST ArkeyanBuilderTest PROPERTY ENVIRONMENT "SKYLANDERS_DATA=${CMAKE_CURRENT_BINARY_DIR}/arkeyan-builder-test/data")
include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake)
arkeyan_setup_ffmpeg(Arkeyan-Builder)
arkeyan_setup_ffmpeg(ArkeyanVersionResourceTest)

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
target_link_libraries(ArkeyanBuilderMusicTest PRIVATE ArkeyanArtReader)
arkeyan_setup_ffmpeg(ArkeyanBuilderMusicTest)

# Real install steps with synthetic inputs and a fake child runner; never launches a game.
add_executable(ArkeyanBuilderGuardTest install_guard_test.cpp edition.cpp fingerprint.cpp process.cpp sha256.cpp toolchain_fetch.cpp igz_shaders.cpp shader_cache_literals.cpp game_icon.cpp menu_music.cpp
    ${SKYBUILD_HD_SOURCES} ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
target_include_directories(ArkeyanBuilderGuardTest PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_include_directories(ArkeyanBuilderGuardTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)
target_include_directories(ArkeyanBuilderGuardTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
target_link_libraries(ArkeyanBuilderGuardTest PRIVATE ArkeyanArtReader ArkeyanXenonUtils ArkeyanBuilderMiniz)
if(WIN32)
    target_compile_definitions(ArkeyanBuilderGuardTest PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(ArkeyanBuilderGuardTest PRIVATE winmm winhttp)
endif()
sky_quality(ArkeyanBuilderGuardTest)
add_test(NAME ArkeyanBuilderGuardTest COMMAND ArkeyanBuilderGuardTest ${CMAKE_CURRENT_BINARY_DIR}/install-guard-test)
set_tests_properties(ArkeyanBuilderGuardTest PROPERTIES TIMEOUT 120)
arkeyan_setup_ffmpeg(ArkeyanBuilderGuardTest)

# The edition finder (edition.h): the report and the game.txt image hashes; with SSA_ISO, SWAPFORCE_ISO, TRAPTEAM_ISO
# (each optional), the discs' images and synthetic header-only editions: ./ArkeyanEditionTest <repo> <scratch dir>
foreach(target ArkeyanEditionTest ArkeyanEditionPrints)
    if(target STREQUAL ArkeyanEditionTest)
        add_executable(${target} edition_test.cpp)
    else()
        # Kit time (tools/make_kit.sh): the fingerprints of a game's addresses.h, from our disc, into the kit.
        add_executable(${target} edition_prints.cpp)
    endif()
    target_sources(${target} PRIVATE edition.cpp fingerprint.cpp sha256.cpp ${CMAKE_SOURCE_DIR}/runtime/vfs.cpp
        ${CMAKE_SOURCE_DIR}/runtime/host/xex_title.cpp ${SKYBUILD_PLATFORM})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
    target_link_libraries(${target} PRIVATE ArkeyanArtReader ArkeyanXenonUtils)
    sky_quality(${target})
endforeach()

# The compiler's .zip extraction on invented archives (stored and deflated entries, a bad CRC, paths that leave the folder).
add_executable(ArkeyanToolchainFetchTest toolchain_fetch_test.cpp toolchain_fetch.cpp process.cpp)
target_include_directories(ArkeyanToolchainFetchTest PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/runtime/platform/win32)
target_link_libraries(ArkeyanToolchainFetchTest PRIVATE ArkeyanBuilderMiniz)
if(WIN32)
    target_compile_definitions(ArkeyanToolchainFetchTest PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(ArkeyanToolchainFetchTest PRIVATE winhttp)
endif()
sky_quality(ArkeyanToolchainFetchTest)
add_test(NAME ArkeyanToolchainFetchTest COMMAND ArkeyanToolchainFetchTest ${CMAKE_CURRENT_BINARY_DIR}/toolchain-fetch-test)
set_tests_properties(ArkeyanToolchainFetchTest PROPERTIES TIMEOUT 120)

# The HD texture pass on synthetic pictures (analysis, the 2x, BC1/BC3 round trips, the pack file): no game data.
add_executable(ArkeyanHdTexturesTest hd_textures_test.cpp ${SKYBUILD_HD_SOURCES})
target_include_directories(ArkeyanHdTexturesTest PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_include_directories(ArkeyanHdTexturesTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)
target_link_libraries(ArkeyanHdTexturesTest PRIVATE ArkeyanArtReader)
sky_quality(ArkeyanHdTexturesTest)
add_test(NAME ArkeyanHdTexturesTest COMMAND ArkeyanHdTexturesTest ${CMAKE_CURRENT_BINARY_DIR}/hd-textures-test)
set_tests_properties(ArkeyanHdTexturesTest PROPERTIES TIMEOUT 120)
