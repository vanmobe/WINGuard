cmake_minimum_required(VERSION 3.15)

cmake_policy(SET CMP0091 NEW)

project(AUDIOLABWingReaperVirtualSoundcheck VERSION 1.2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Validate required REAPER SDK headers
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/reaper-sdk/reaper_plugin.h" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/reaper-sdk/reaper_plugin_functions.h")
    message(FATAL_ERROR
        "Missing REAPER SDK headers in lib/reaper-sdk/. "
        "Required: reaper_plugin.h and reaper_plugin_functions.h")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/oscpack/osc/OscOutboundPacketStream.h")
    message(FATAL_ERROR
        "Missing oscpack sources in lib/oscpack/. "
        "Clone https://github.com/RossBencina/oscpack.git into lib/oscpack")
endif()

# Enable Objective-C for macOS settings dialog
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
    set(CMAKE_OBJC_FLAGS "${CMAKE_OBJC_FLAGS} -fPIC")
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fPIC -std=c++17")
endif()

# Platform detection
if(APPLE)
    set(EXTENSION_SUFFIX ".dylib")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
elseif(WIN32)
    set(EXTENSION_SUFFIX ".dll")
else()
    message(FATAL_ERROR "Unsupported platform. This project builds only on macOS and Windows.")
endif()

# Source files
set(SOURCES
    # Extension layer
    src/extension/main.cpp
    src/extension/reaper_extension.cpp
    
    # Core OSC communication
    src/core/wing_osc.cpp
    src/core/osc_routing.cpp
    src/core/osc_builder.cpp
    
    # Utilities
    src/utilities/wing_config.cpp
    src/utilities/logger.cpp
    src/utilities/platform_util.cpp
    src/utilities/string_format.cpp
    
    # Track management
    src/track/track_manager.cpp

    # Cross-platform UI bridge
    src/ui/dialog_bridge.cpp
)

# Platform-specific source files
if(APPLE)
    list(APPEND SOURCES 
        src/ui/settings_dialog_macos.mm
        src/ui/wing_connector_dialog_macos.mm
    )
    set_source_files_properties(
        src/ui/settings_dialog_macos.mm
        src/ui/wing_connector_dialog_macos.mm
        PROPERTIES
        LANGUAGE OBJCXX
    )
elseif(WIN32)
    list(APPEND SOURCES
        src/ui/wing_connector_dialog_windows.cpp
    )
endif()

# Header files
set(HEADERS
    include/wingconnector/wing_osc.h
    include/wingconnector/wing_config.h
    include/wingconnector/track_manager.h
    include/wingconnector/reaper_extension.h
)

# Create the extension library
add_library(reaper_wingconnector SHARED ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(reaper_wingconnector
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

# Treat external dependencies as system headers to avoid breaking builds on
# third-party warnings emitted by SDK/vendor code.
target_include_directories(reaper_wingconnector
    SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/reaper-sdk
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/oscpack
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/nlohmann
)

# Compiler definitions
target_compile_definitions(reaper_wingconnector
    PRIVATE
        REAPER_EXTENSION
)

# Compiler flags
if(MSVC)
    # Keep the packaged extension self-contained so REAPER can load it on a
    # clean Windows installation without a separate Visual C++ Redistributable.
    set_property(TARGET reaper_wingconnector PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(
        reaper_wingconnector
        PRIVATE
            /W4
            /WX-
            /wd4100  # unreferenced formal parameter (REAPER SDK)
            /wd4505  # unreferenced local function removed (REAPER SDK)
    )
else()
    target_compile_options(
        reaper_wingconnector
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-error
            -Wno-unused-parameter
    )
endif()

# Link oscpack library
find_library(OSCPACK_LIB
    NAMES oscpack
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/lib/oscpack
)

if(OSCPACK_LIB)
    target_link_libraries(reaper_wingconnector PRIVATE ${OSCPACK_LIB})
else()
    message(STATUS "oscpack not found, will use included source")
    # Add oscpack source files if not found as library
    file(GLOB OSCPACK_SOURCES 
        "lib/oscpack/osc/*.cpp"
        "lib/oscpack/ip/*.cpp"
    )
    if(WIN32)
        file(GLOB OSCPACK_PLATFORM_SOURCES "lib/oscpack/ip/win32/*.cpp")
    else()
        file(GLOB OSCPACK_PLATFORM_SOURCES "lib/oscpack/ip/posix/*.cpp")
    endif()

    if(APPLE)
        set_source_files_properties(
            ${OSCPACK_SOURCES}
            ${OSCPACK_PLATFORM_SOURCES}
            PROPERTIES
            COMPILE_OPTIONS "-Wno-deprecated-declarations"
        )
    endif()
    
    target_sources(reaper_wingconnector PRIVATE 
        ${OSCPACK_SOURCES} 
        ${OSCPACK_PLATFORM_SOURCES}
    )
endif()

# Platform-specific settings
if(APPLE)
    set_target_properties(reaper_wingconnector PROPERTIES
        SUFFIX ${EXTENSION_SUFFIX}
        PREFIX ""
        MACOSX_RPATH ON
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
    # Link with Cocoa for native dialogs and CoreFoundation
    target_link_libraries(reaper_wingconnector PRIVATE 
        "-framework Cocoa"
        "-framework CoreFoundation"
    )
    
elseif(WIN32)
    set_target_properties(reaper_wingconnector PROPERTIES
        SUFFIX ${EXTENSION_SUFFIX}
        PREFIX ""
    )
    target_link_libraries(reaper_wingconnector PRIVATE ws2_32 winmm comctl32 gdiplus)
endif()

# Set output name
set_target_properties(reaper_wingconnector PROPERTIES
    OUTPUT_NAME "reaper_wingconnector"
)

# Installation
install(TARGETS reaper_wingconnector
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)

# Copy config file
install(FILES config.json OPTIONAL
    DESTINATION .
)

install(FILES assets/wingguard-logo.png
    DESTINATION .
)

# Print build info
message(STATUS "AUDIOLAB.wing.reaper.virtualsoundcheck ${PROJECT_VERSION}")
message(STATUS "Building for: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Extension suffix: ${EXTENSION_SUFFIX}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

# Optional: Build tests
option(BUILD_TESTS "Build test programs" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
