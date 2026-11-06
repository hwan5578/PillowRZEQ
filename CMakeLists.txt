cmake_minimum_required(VERSION 3.22)

project(PillowRZ VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC 런타임 정적 링크 -> Visual C++ 재배포 패키지가 없는 PC에서도 VST3가 로드됨 (PillowSaturator와 동일)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# macOS 유니버설 바이너리가 필요하면 주석 해제
# set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)

option(PILLOW_BUILD_TESTS "Build the JUCE-free DSP core tests" ON)

# ── JUCE ─────────────────────────────────────────────────────────────────────
#  1) 이 폴더 안에 JUCE/ 가 있으면 그걸 사용 (git submodule 이나 직접 복사)
#  2) 없으면 처음 configure 할 때 GitHub에서 자동으로 받아옴 (한 번만, 몇 분 걸림)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.17-security   # 8.0.16~17: 상태 저장(XML/ValueTree) 파싱 보안 수정 포함
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ── 플러그인 ──────────────────────────────────────────────────────────────────
juce_add_plugin(PillowRZ
    COMPANY_NAME                "Pillow Audio"
    BUNDLE_ID                   com.pillowaudio.rzeq
    PLUGIN_MANUFACTURER_CODE    Pilw      # 기존 Pillow 플러그인과 같은 코드로 맞추면 DAW에서 한 제조사로 묶임
    PLUGIN_CODE                 Rz62      # 플러그인마다 고유해야 함
    PRODUCT_NAME                "Pillow RZ EQ"
    FORMATS                     VST3 AU Standalone   # AU는 macOS에서만 빌드됨
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE     # TRUE면 빌드 후 시스템 VST3 폴더로 복사 (Windows는 관리자 권한 필요)
    VST3_CATEGORIES             Fx EQ
    AU_MAIN_TYPE                kAudioUnitType_Effect)

target_sources(PillowRZ PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(PillowRZ PRIVATE Source)

target_compile_definitions(PillowRZ PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# 한국어 Windows(cp949)에서 UTF-8 소스 주석이 깨지지 않도록
target_compile_options(PillowRZ PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(PillowRZ
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ── DSP 코어 테스트 (JUCE 불필요) ─────────────────────────────────────────────
if(PILLOW_BUILD_TESTS)
    enable_testing()
    add_executable(PillowRZCoreTests tests/CoreTests.cpp)
    target_include_directories(PillowRZCoreTests PRIVATE Source)
    target_compile_options(PillowRZCoreTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME PillowRZCoreTests COMMAND PillowRZCoreTests)
endif()
