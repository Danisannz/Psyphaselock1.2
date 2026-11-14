cmake_minimum_required(VERSION 3.22)
project(PsyPhaseLock VERSION 1.2.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PsyPhaseLock
    COMPANY_NAME            "Dani San"
    PLUGIN_MANUFACTURER_CODE Psyt
    PLUGIN_CODE             Ppl3
    FORMATS                 VST3 Standalone
    PRODUCT_NAME            "DaniSan PsyPhase Lock"
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       FALSE
    VST3_CATEGORIES         Fx
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(PsyPhaseLock PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(PsyPhaseLock PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(PsyPhaseLock
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
