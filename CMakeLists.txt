cmake_minimum_required(VERSION 3.22)

cmake_policy(SET CMP0091 NEW)
project(MicVstBridge VERSION 0.4.5 LANGUAGES CXX)

if (NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Mic FX Bridge currently supports Windows with MSVC only")
endif()

include(cmake/MsvcJsonDependencies.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/JUCE/CMakeLists.txt")
    set(MIC_VST_BRIDGE_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/JUCE")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DJUCE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/.deps/JUCE"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuceForCxxOnly.cmake"
        RESULT_VARIABLE juce_patch_result
    )
    if (juce_patch_result)
        message(FATAL_ERROR "Unable to apply the JUCE compatibility patch")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DJUCE_SOURCE_DIR=${MIC_VST_BRIDGE_JUCE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuceWasapiSession.cmake"
        RESULT_VARIABLE juce_wasapi_patch_result
    )
    if (juce_wasapi_patch_result)
        message(FATAL_ERROR "Unable to apply the JUCE WASAPI session initialization fix")
    endif()
    add_subdirectory(.deps/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        # Exact JUCE 9.0.0 commit. The patch scripts additionally verify the
        # complete normalised contents of every dependency file they modify.
        GIT_TAG f8f8864172464b9adf9eba6101e1f784838d1597
        PATCH_COMMAND "${CMAKE_COMMAND}"
            "-DJUCE_SOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuceForCxxOnly.cmake"
        COMMAND "${CMAKE_COMMAND}"
            "-DJUCE_SOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuceWasapiSession.cmake"
    )
    FetchContent_MakeAvailable(JUCE)
    set(MIC_VST_BRIDGE_JUCE_SOURCE_DIR "${juce_SOURCE_DIR}")
endif()

# Embed the prepared multi-resolution icon directly, so headless builds do not
# need JUCE's native graphics initialization to convert the source PNG.
set(MIC_VST_BRIDGE_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/mic-fx-bridge.ico")

# Local MSVC/CMake compatibility path: the vendored JUCE bootstrap records
# wrappers for C translation units. Compile those wrappers as C even though
# this project only enables CXX.
get_property(juce_c_wrappers GLOBAL PROPERTY JUCE_C_WRAPPERS)
if (juce_c_wrappers)
    set_source_files_properties(${juce_c_wrappers} PROPERTIES COMPILE_OPTIONS "/TC")
endif()

set(MIC_VST_BRIDGE_APP_SOURCES
    src/Main.cpp
    src/MainComponent.cpp
    src/MainComponent.h
    src/AudioEngine.cpp
    src/AudioEngine.h
    src/PluginBrowser.cpp
    src/PluginBrowser.h
    src/PluginStatePersistence.h
    src/SettingsDiskStore.h
    src/SettingsSnapshot.h
    src/LegacySettings.h
    src/DebouncedSave.h
    src/UiLayout.h
    src/RmsMeter.h
    src/PeakAccumulator.h
    src/MeterReadout.h
    src/StreamDiagnostics.h
    src/InputChannelRouting.h
    src/DeviceManagerSetup.h
    src/RecoveryState.cpp
    src/RecoveryState.h
    src/UiText.h
)

function(add_mic_vst_bridge_app target_name)
    juce_add_gui_app(${target_name}
        PRODUCT_NAME "Mic FX Bridge"
        COMPANY_NAME "Neu5Gc"
        COMPANY_COPYRIGHT "Copyright (c) 2026 Neu5Gc"
        BUNDLE_ID "local.micfxbridge.app"
    )

    # JUCE's resource rule does not track Info.txt itself. Keep Windows version
    # metadata in sync when the project version changes in an existing build tree.
    get_target_property(generated_sources ${target_name} JUCE_GENERATED_SOURCES_DIRECTORY)
    get_target_property(info_file ${target_name} JUCE_INFO_FILE)
    add_custom_command(
        OUTPUT "${generated_sources}/${target_name}_resources.rc"
        APPEND DEPENDS "${info_file}"
    )

    set(icon_resource "${generated_sources}/${target_name}_icon.rc")
    configure_file(assets/icons/app-icon.rc.in "${icon_resource}" @ONLY)
    set_source_files_properties("${icon_resource}" PROPERTIES
        OBJECT_DEPENDS "${MIC_VST_BRIDGE_APP_ICON}")

    target_sources(${target_name} PRIVATE
        ${MIC_VST_BRIDGE_APP_SOURCES}
        "${icon_resource}"
    )

    target_compile_definitions(${target_name} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_VST=0
        MIC_VST_BRIDGE_ENGLISH=1
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=0
        JUCE_ASIO=0
    )

    target_link_libraries(${target_name} PRIVATE
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    )

    juce_generate_juce_header(${target_name})
endfunction()

# New releases are English-only. Keep the existing English target/output name
# and the legacy shared settings format; historical bilingual releases remain.
add_mic_vst_bridge_app(MicVstBridgeEn)

# Deterministic, opt-in renderer for the checked-in flat SVG icon. It keeps the
# source PNG on the exact three-colour brand palette and is excluded from normal
# application builds.
juce_add_console_app(IconAssetRenderer PRODUCT_NAME "Icon Asset Renderer")
set_target_properties(IconAssetRenderer PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_sources(IconAssetRenderer PRIVATE tools/IconAssetRenderer.cpp)
target_compile_definitions(IconAssetRenderer PRIVATE JUCE_USE_CURL=0)
target_link_libraries(IconAssetRenderer PRIVATE
    juce::juce_build_tools
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# Optional manual integration test. It is excluded from the default build and
# deliberately not registered with CTest because it loads a third-party plugin.
juce_add_console_app(Vst3HostSmoke
    PRODUCT_NAME "VST3 Host Smoke"
)
set_target_properties(Vst3HostSmoke PROPERTIES EXCLUDE_FROM_ALL TRUE)

target_sources(Vst3HostSmoke PRIVATE
    tests/Vst3HostSmoke.cpp
    tests/Vst3ChainStability.cpp
    tests/SettingsReadProbe.cpp
)

set(MIC_VST_BRIDGE_LOCAL_SAVED_PROBES 0)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/SavedChainRestoreProbe.cpp"
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/SavedGainProbe.cpp")
    target_sources(Vst3HostSmoke PRIVATE
        tests/SavedChainRestoreProbe.cpp
        tests/SavedGainProbe.cpp
    )
    set(MIC_VST_BRIDGE_LOCAL_SAVED_PROBES 1)
endif()

target_compile_definitions(Vst3HostSmoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_VST=0
    MIC_VST_BRIDGE_LOCAL_SAVED_PROBES=${MIC_VST_BRIDGE_LOCAL_SAVED_PROBES}
)

target_link_libraries(Vst3HostSmoke PRIVATE
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

enable_testing()

# Optional local input-only hardware probe. Its source is intentionally ignored
# because it contains workstation-specific device selection.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/WasapiInputProbe.cpp")
    juce_add_console_app(WasapiInputProbe PRODUCT_NAME "WASAPI Input Probe")
    set_target_properties(WasapiInputProbe PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(WasapiInputProbe PRIVATE tests/WasapiInputProbe.cpp)
    target_compile_definitions(WasapiInputProbe PRIVATE
        JUCE_USE_CURL=0 JUCE_WASAPI=1 JUCE_DIRECTSOUND=0 JUCE_ASIO=0)
    target_link_libraries(WasapiInputProbe PRIVATE
        juce::juce_audio_devices
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

juce_add_console_app(DeviceManagerSetupTests PRODUCT_NAME "Device Manager Setup Tests")
target_sources(DeviceManagerSetupTests PRIVATE tests/DeviceManagerSetupTests.cpp)
target_compile_definitions(DeviceManagerSetupTests PRIVATE
    JUCE_USE_CURL=0 JUCE_WASAPI=1 JUCE_DIRECTSOUND=0 JUCE_ASIO=0)
target_link_libraries(DeviceManagerSetupTests PRIVATE
    juce::juce_audio_devices
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_test(NAME DeviceManagerSetupTests COMMAND DeviceManagerSetupTests)

foreach(meter_test IN ITEMS InputChannelRoutingTests PeakAccumulatorTests StreamDiagnosticsTests DebouncedSaveTests)
    add_executable(${meter_test} "tests/${meter_test}.cpp")
    target_include_directories(${meter_test} PRIVATE src)
    target_compile_features(${meter_test} PRIVATE cxx_std_20)
    target_compile_options(${meter_test} PRIVATE /W4 /permissive-)
    add_test(NAME ${meter_test} COMMAND ${meter_test})
endforeach()

add_test(NAME WasapiSessionPatchRegression
    COMMAND "${CMAKE_COMMAND}"
        "-DJUCE_SOURCE_DIR=${MIC_VST_BRIDGE_JUCE_SOURCE_DIR}"
        "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/CheckWasapiSessionPatch.cmake")

add_executable(RecoveryStateTests
    tests/RecoveryStateTests.cpp
    src/RecoveryState.cpp
)

target_include_directories(RecoveryStateTests PRIVATE src)
target_compile_features(RecoveryStateTests PRIVATE cxx_std_20)

if (MSVC)
    target_compile_options(RecoveryStateTests PRIVATE /W4 /permissive-)
else()
    target_compile_options(RecoveryStateTests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME RecoveryStateTests COMMAND RecoveryStateTests)

add_executable(RecoveryStateStressTests
    tests/RecoveryStateStressTests.cpp
    src/RecoveryState.cpp
)
target_include_directories(RecoveryStateStressTests PRIVATE src)
target_compile_features(RecoveryStateStressTests PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(RecoveryStateStressTests PRIVATE /W4 /permissive-)
else()
    target_compile_options(RecoveryStateStressTests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME RecoveryStateStressTests COMMAND RecoveryStateStressTests)

add_executable(RmsMeterTests tests/RmsMeterTests.cpp)
target_include_directories(RmsMeterTests PRIVATE src)
target_compile_features(RmsMeterTests PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(RmsMeterTests PRIVATE /W4 /permissive-)
else()
    target_compile_options(RmsMeterTests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME RmsMeterTests COMMAND RmsMeterTests)

juce_add_console_app(PluginStatePersistenceTests PRODUCT_NAME "Plugin State Persistence Tests")
target_sources(PluginStatePersistenceTests PRIVATE tests/PluginStatePersistenceTests.cpp)
target_include_directories(PluginStatePersistenceTests PRIVATE src)
target_compile_definitions(PluginStatePersistenceTests PRIVATE JUCE_USE_CURL=0)
target_link_libraries(PluginStatePersistenceTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME PluginStatePersistenceTests COMMAND PluginStatePersistenceTests)

juce_add_console_app(ChainPresetTests PRODUCT_NAME "Chain Preset Tests")
target_sources(ChainPresetTests PRIVATE tests/ChainPresetTests.cpp)
target_include_directories(ChainPresetTests PRIVATE src)
target_compile_definitions(ChainPresetTests PRIVATE JUCE_USE_CURL=0)
target_link_libraries(ChainPresetTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME ChainPresetTests COMMAND ChainPresetTests)

juce_add_console_app(SettingsSnapshotTests PRODUCT_NAME "Settings Snapshot Tests")
target_sources(SettingsSnapshotTests PRIVATE
    tests/SettingsSnapshotTests.cpp
)
target_include_directories(SettingsSnapshotTests PRIVATE src)
target_compile_definitions(SettingsSnapshotTests PRIVATE JUCE_USE_CURL=0)
target_link_libraries(SettingsSnapshotTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME SettingsSnapshotTests COMMAND SettingsSnapshotTests)

juce_add_console_app(SettingsDiskStoreTests PRODUCT_NAME "Settings Disk Store Tests")
target_sources(SettingsDiskStoreTests PRIVATE tests/SettingsDiskStoreTests.cpp)
target_include_directories(SettingsDiskStoreTests PRIVATE src)
target_compile_definitions(SettingsDiskStoreTests PRIVATE JUCE_USE_CURL=0)
target_link_libraries(SettingsDiskStoreTests PRIVATE
    juce::juce_data_structures
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME SettingsDiskStoreTests COMMAND SettingsDiskStoreTests)

juce_add_console_app(PluginDiscoveryTests PRODUCT_NAME "Plugin Discovery Tests")
target_sources(PluginDiscoveryTests PRIVATE tests/PluginDiscoveryTests.cpp)
target_compile_definitions(PluginDiscoveryTests PRIVATE
    JUCE_USE_CURL=0
    MIC_VST_BRIDGE_DISCOVERY_TEST=1
)
target_link_libraries(PluginDiscoveryTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME PluginDiscoveryTests COMMAND PluginDiscoveryTests)

# JUCE 9 treats ordinary const char* strings as ASCII. Catch localized text
# accidentally passed through that constructor before shipping another build.
find_program(MIC_VST_BRIDGE_POWERSHELL NAMES pwsh powershell REQUIRED)
add_test(NAME MsvcDependencyRegression
    COMMAND "${MIC_VST_BRIDGE_POWERSHELL}"
        -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/CheckMsvcDependencies.ps1"
        -TestBinaryDirectory "${CMAKE_CURRENT_BINARY_DIR}"
        -CMake "${CMAKE_COMMAND}"
        -JuceSourceDirectory "${MIC_VST_BRIDGE_JUCE_SOURCE_DIR}"
)
set_tests_properties(MsvcDependencyRegression PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

add_test(NAME Utf8LiteralRegression
    COMMAND "${MIC_VST_BRIDGE_POWERSHELL}"
        -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/CheckUtf8Literals.ps1"
        -SourceDirectory "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

add_test(NAME LocalizedLiteralRegression
    COMMAND "${MIC_VST_BRIDGE_POWERSHELL}"
        -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/CheckLocalizedLiterals.ps1"
        -SourceDirectory "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

foreach(ui_language IN ITEMS English)
    set(ui_text_target "UiText${ui_language}Tests")
    juce_add_console_app(${ui_text_target} PRODUCT_NAME "UI Text ${ui_language} Tests")
    target_sources(${ui_text_target} PRIVATE tests/UiTextTests.cpp)
    target_include_directories(${ui_text_target} PRIVATE src)
    target_compile_definitions(${ui_text_target} PRIVATE MIC_VST_BRIDGE_ENGLISH=1)
    target_compile_definitions(${ui_text_target} PRIVATE JUCE_USE_CURL=0)
    target_link_libraries(${ui_text_target} PRIVATE
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
    add_test(NAME ${ui_text_target} COMMAND ${ui_text_target})
endforeach()
