add_executable(z_generals WIN32)

# Use a binary name that doesn't conflict with original game.
# GeneralsX @build BenderAI 15/02/2026 Linux branding update
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME "generalszh${RTS_BUILD_OUTPUT_SUFFIX}")
else()
    # GeneralsX @build BenderAI 15/02/2026 Linux branding update
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME GeneralsXZH)
endif()

# GeneralsX @build BenderAI 13/02/2026 Windows-specific libraries
if(WIN32)
    target_link_libraries(z_generals PRIVATE
        binkstub
        comctl32
        core_debug
        d3d8
        dinput8
        dxguid
        imm32
        milesstub
        vfw32
        winmm
    )
endif()

# Cross-platform core libraries
target_link_libraries(z_generals PRIVATE
    core_profile_legacy
    d3dx8
    z_gameengine
    z_gameenginedevice
    zi_always
)

# Linux SDL3 windowing/input (Phase 1)
# GeneralsX @build BenderAI 11/02/2026
if(SAGE_USE_SDL3)
    target_link_libraries(z_generals PRIVATE sdl3lib)
endif()

# TODO Originally referred to build host and user, replace with git info perhaps?
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h
"#pragma once

#define VERSION_LOCALBUILDNUM 0
#define VERSION_BUILDUSER \"\"
#define VERSION_BUILDLOC \"\"
"
)

# Based on original binary values for these variables.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_BUILDNUM 601
"
)

if(MSVC)
    target_link_options(z_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(z_generals PRIVATE
    $<$<BOOL:${WIN32}>:WinMain.cpp>
    $<$<BOOL:${WIN32}>:WinMain.h>
    # Linux SDL3 entry point (Phase 1.5 - TheSuperHackers @build 10/02/2026)
    $<$<NOT:$<BOOL:${WIN32}>>:SDL3Main.cpp>
    # Linux stubs for P2-P3 platform functions (P2-P3 - TheSuperHackers @build 13/02/2026 BenderAI Session 37)
    $<$<NOT:$<BOOL:${WIN32}>>:LinuxStubs.cpp>
)

# GeneralsX @feature dvorovrus 26/09/2026 Native UIKit profile launcher for iOS.
# Keep launcher UI in its own dylib: launcher-only changes can then be rebuilt and
# repackaged without recompiling the whole GeneralsX engine.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # In-game UIKit ESC overlay. This is compiled into the actual game binary,
    # separate from the native launcher dylib.
    target_sources(z_generals PRIVATE
        IOSGameOverlay.h
        IOSGameOverlay.mm
    )
    set_source_files_properties(IOSGameOverlay.mm PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "-fobjc-arc"
    )
    set(GX_PROJECT_VERSION "0.0.0")
    set(GX_ENGINE_VERSION "0.0.0")
    set(GX_LAUNCHER_VERSION "0.0.0")
    set(GX_VERSION_FILE "${CMAKE_SOURCE_DIR}/ios/version.env")
    if(EXISTS "${GX_VERSION_FILE}")
        file(STRINGS "${GX_VERSION_FILE}" GX_VERSION_LINES)
        foreach(GX_VERSION_LINE IN LISTS GX_VERSION_LINES)
            if(GX_VERSION_LINE MATCHES "^PROJECT_VERSION=(.+)$")
                set(GX_PROJECT_VERSION "${CMAKE_MATCH_1}")
            elseif(GX_VERSION_LINE MATCHES "^ENGINE_VERSION=(.+)$")
                set(GX_ENGINE_VERSION "${CMAKE_MATCH_1}")
            elseif(GX_VERSION_LINE MATCHES "^LAUNCHER_VERSION=(.+)$")
                set(GX_LAUNCHER_VERSION "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()

    set(GX_BUILD_COMMIT "$ENV{GITHUB_SHA}")
    if(GX_BUILD_COMMIT STREQUAL "")
        set(GX_BUILD_COMMIT "unknown")
    endif()
    set(GX_BUILD_RUN "$ENV{GITHUB_RUN_ID}")
    if(GX_BUILD_RUN STREQUAL "")
        set(GX_BUILD_RUN "unknown")
    endif()

    add_library(generalsx_ios_launcher SHARED
        IOSProfileLauncher.h
        IOSProfileLauncher.mm
        IOSGameFileManager.h
        IOSGeneralsVPN.h
        IOSGeneralsVPN.mm
        IOSGameFileManager.mm
    )
    set_target_properties(generalsx_ios_launcher PROPERTIES
        OUTPUT_NAME "GeneralsXLauncher"
        MACOSX_RPATH ON
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
    target_include_directories(generalsx_ios_launcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    set_source_files_properties(IOSGeneralsVPN.mm PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "-fobjc-arc"
    )

    set_source_files_properties(IOSProfileLauncher.mm PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "-fobjc-arc"
    )

    # The launcher is a standalone Objective-C++ dylib. Unlike the main
    # executable, it must link its Foundation and Objective-C runtime
    # dependencies explicitly instead of inheriting them from other targets.
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
    find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
    find_library(NETWORKEXTENSION_FRAMEWORK NetworkExtension REQUIRED)
    find_library(OBJC_LIBRARY objc REQUIRED)
    find_library(ZLIB_LIBRARY z REQUIRED)
    target_link_libraries(generalsx_ios_launcher PRIVATE
        ${FOUNDATION_FRAMEWORK}
        ${UIKIT_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${AVFOUNDATION_FRAMEWORK}
        ${COREMEDIA_FRAMEWORK}
        ${NETWORKEXTENSION_FRAMEWORK}
        ${OBJC_LIBRARY}
        ${ZLIB_LIBRARY}
    )
    # The ESC overlay is compiled into z_generals itself, so its UIKit symbols
    # must also be linked into the game executable (not only the launcher dylib).
    target_link_libraries(z_generals PRIVATE
        ${UIKIT_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${OBJC_LIBRARY}
    )
    target_compile_definitions(generalsx_ios_launcher PRIVATE
        GX_PROJECT_VERSION=\"${GX_PROJECT_VERSION}\"
        GX_ENGINE_VERSION=\"${GX_ENGINE_VERSION}\"
        GX_LAUNCHER_VERSION=\"${GX_LAUNCHER_VERSION}\"
        GX_LAUNCHER_COMMIT=\"${GX_BUILD_COMMIT}\"
        GX_ENGINE_COMMIT=\"${GX_BUILD_COMMIT}\"
        GX_BASE_SHELL_RUN=\"${GX_BUILD_RUN}\"
        GX_LAUNCHER_RUN=\"${GX_BUILD_RUN}\"
    )
    target_link_libraries(z_generals PRIVATE
        generalsx_ios_launcher
    )
endif()

# RC files optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(z_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(z_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalszh${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(z_generals)
endif()
