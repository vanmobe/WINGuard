enable_testing()

add_executable(source_monitor_tests
    source_monitor_tests.cpp
)

add_executable(soundcheck_state_tests
    soundcheck_state_tests.cpp
)

add_executable(adoption_plan_tests
    adoption_plan_tests.cpp
)

add_executable(playback_allocator_tests
    playback_allocator_tests.cpp
)

add_executable(playback_naming_tests
    playback_naming_tests.cpp
)

add_executable(wing_config_tests
    wing_config_tests.cpp
    ../src/utilities/wing_config.cpp
    ../src/utilities/platform_util.cpp
)

add_executable(osc_builder_tests
    osc_builder_tests.cpp
    ../src/core/osc_builder.cpp
)

add_executable(source_input_mode_plan_tests
    source_input_mode_plan_tests.cpp
)

add_executable(stereo_channel_plan_tests
    stereo_channel_plan_tests.cpp
)

add_executable(windows_ui_layout_tests
    windows_ui_layout_tests.cpp
)

add_executable(wing_osc_probe
    osc_probe.cpp
)

add_executable(osc_address_probe
    osc_address_probe.cpp
)

add_executable(osc_write_tool
    osc_write_tool.cpp
)

target_include_directories(source_monitor_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(soundcheck_state_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(adoption_plan_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(playback_allocator_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(playback_naming_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(wing_config_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(osc_builder_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(source_input_mode_plan_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(stereo_channel_plan_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(windows_ui_layout_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
)

target_include_directories(wing_osc_probe
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib/oscpack
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nlohmann
)

target_include_directories(osc_address_probe
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/wingconnector
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib/oscpack
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nlohmann
)

target_include_directories(osc_write_tool
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib/oscpack
        ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nlohmann
)

if(MSVC)
    target_compile_options(source_monitor_tests PRIVATE /W4 /WX-)
    target_compile_options(soundcheck_state_tests PRIVATE /W4 /WX-)
    target_compile_options(windows_ui_layout_tests PRIVATE /W4 /WX-)
    target_compile_options(wing_osc_probe PRIVATE /W4 /WX-)
else()
    target_compile_options(source_monitor_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(soundcheck_state_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(adoption_plan_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(playback_allocator_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(playback_naming_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(wing_config_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(osc_builder_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(source_input_mode_plan_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(stereo_channel_plan_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(windows_ui_layout_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    target_compile_options(wing_osc_probe PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
endif()

file(GLOB OSCPACK_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/oscpack/osc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/oscpack/ip/*.cpp"
)

if(WIN32)
    file(GLOB OSCPACK_PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../lib/oscpack/ip/win32/*.cpp")
    target_link_libraries(wing_osc_probe PRIVATE ws2_32 winmm)
else()
    file(GLOB OSCPACK_PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../lib/oscpack/ip/posix/*.cpp")
    target_link_libraries(wing_osc_probe PRIVATE pthread)
endif()

target_sources(wing_osc_probe PRIVATE
    ${OSCPACK_SOURCES}
    ${OSCPACK_PLATFORM_SOURCES}
)

target_sources(osc_address_probe PRIVATE
    ${OSCPACK_SOURCES}
    ${OSCPACK_PLATFORM_SOURCES}
)

target_sources(osc_write_tool PRIVATE
    ${OSCPACK_SOURCES}
    ${OSCPACK_PLATFORM_SOURCES}
)

if(WIN32)
    target_link_libraries(osc_address_probe PRIVATE ws2_32 winmm)
    target_link_libraries(osc_write_tool PRIVATE ws2_32 winmm)
else()
    target_link_libraries(osc_address_probe PRIVATE pthread)
    target_link_libraries(osc_write_tool PRIVATE pthread)
endif()

add_test(NAME source_monitor_tests COMMAND source_monitor_tests)
add_test(NAME soundcheck_state_tests COMMAND soundcheck_state_tests)
add_test(NAME adoption_plan_tests COMMAND adoption_plan_tests)
add_test(NAME playback_allocator_tests COMMAND playback_allocator_tests)
add_test(NAME playback_naming_tests COMMAND playback_naming_tests)
add_test(NAME wing_config_tests COMMAND wing_config_tests)
add_test(NAME osc_builder_tests COMMAND osc_builder_tests)
add_test(NAME source_input_mode_plan_tests COMMAND source_input_mode_plan_tests)
add_test(NAME stereo_channel_plan_tests COMMAND stereo_channel_plan_tests)
add_test(NAME windows_ui_layout_tests COMMAND windows_ui_layout_tests)
