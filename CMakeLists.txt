cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(MixX VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(MX_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND MX_FORMATS AU)
endif()

juce_add_plugin(MixX
    COMPANY_NAME "Tune Talks"
    BUNDLE_ID "com.tunetalks.mixx"
    PLUGIN_MANUFACTURER_CODE Tntk
    PLUGIN_CODE MixX
    FORMATS ${MX_FORMATS}
    PRODUCT_NAME "Mix-X"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx
    DESCRIPTION "Mix analyser: tells you what to fix in EQ, compression, sidechain, saturation")

set(MX_SOURCES Source/Analyzer.cpp Source/Diagnostics.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_sources(MixX PRIVATE ${MX_SOURCES})
target_include_directories(MixX PRIVATE Source)

target_compile_definitions(MixX PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

set(MX_LINUX_MIN_DEFS JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0 JUCE_USE_XRENDER=0 JUCE_ALSA=0 JUCE_JACK=0)
if(MX_MINIMAL_LINUX)
    target_compile_definitions(MixX PUBLIC ${MX_LINUX_MIN_DEFS})
endif()

target_link_libraries(MixX
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# Developer test harness: cmake -DMX_BUILD_TESTS=ON
if(MX_BUILD_TESTS)
    juce_add_console_app(MixXTests PRODUCT_NAME "MixXTests")
    target_sources(MixXTests PRIVATE Tests/TestMain.cpp ${MX_SOURCES})
    target_include_directories(MixXTests PRIVATE Source)
    target_compile_definitions(MixXTests PRIVATE JucePlugin_Name="Mix-X" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    if(MX_MINIMAL_LINUX)
        target_compile_definitions(MixXTests PRIVATE ${MX_LINUX_MIN_DEFS})
    endif()
    target_link_libraries(MixXTests PRIVATE juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
endif()

# Threshold trainer: cmake -DMX_BUILD_TESTS=ON, then run MixXTrainer [variants] [realAudioDir]
if(MX_BUILD_TESTS)
    juce_add_console_app(MixXTrainer PRODUCT_NAME "MixXTrainer")
    target_sources(MixXTrainer PRIVATE Tests/Trainer.cpp Source/Analyzer.cpp Source/Diagnostics.cpp)
    target_include_directories(MixXTrainer PRIVATE Source)
    target_compile_definitions(MixXTrainer PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
    if(MX_MINIMAL_LINUX)
        target_compile_definitions(MixXTrainer PRIVATE ${MX_LINUX_MIN_DEFS})
    endif()
    target_link_libraries(MixXTrainer PRIVATE juce::juce_audio_formats juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
endif()

# Calibration on a folder of finished masters: MixXCalibrate <folder> [genre]
if(MX_BUILD_TESTS)
    juce_add_console_app(MixXCalibrate PRODUCT_NAME "MixXCalibrate")
    target_sources(MixXCalibrate PRIVATE Tests/Calibrate.cpp Source/Analyzer.cpp Source/Diagnostics.cpp)
    target_include_directories(MixXCalibrate PRIVATE Source)
    target_compile_definitions(MixXCalibrate PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
    if(MX_MINIMAL_LINUX)
        target_compile_definitions(MixXCalibrate PRIVATE ${MX_LINUX_MIN_DEFS})
    endif()
    target_link_libraries(MixXCalibrate PRIVATE juce::juce_audio_formats juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
endif()
