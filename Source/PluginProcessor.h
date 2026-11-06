#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/RZ062Core.h"

namespace pillow::ids
{
    // 전역
    inline constexpr const char* quality = "quality";   // v0.3: 0 = ZERO LATENCY, 1 = HQ
    inline constexpr const char* routing = "routing";
    inline constexpr const char* link    = "link";
    inline constexpr const char* steps   = "steps";
    inline constexpr const char* eqOn    = "eqOn";
    inline constexpr const char* driveOn = "driveOn";
    inline constexpr const char* focus   = "focus";
    inline constexpr const char* mix     = "mix";
    inline constexpr const char* xfLow   = "xfLow";
    inline constexpr const char* xfMid   = "xfMid";
    inline constexpr const char* xfHigh  = "xfHigh";

    // 모듈별 — 모듈 1은 v0.1과 같은 ID(기존 세션 호환), 모듈 2는 뒤에 "2"
    inline constexpr const char* mode     = "mode";
    inline constexpr const char* treble   = "treble";
    inline constexpr const char* trebleQ  = "trebleQ";
    inline constexpr const char* bass     = "bass";
    inline constexpr const char* bassQ    = "bassQ";
    inline constexpr const char* tilt     = "tilt";
    inline constexpr const char* trebleHz = "trebleHz";   // v0.4: 기준 주파수 (연속)
    inline constexpr const char* bassHz   = "bassHz";     // v0.4
    inline constexpr const char* presHz   = "presHz";     // v0.4: 연속 (v0.3 이하의 4단계 presFreq를 대체)
    inline constexpr const char* presFreqLegacy = "presFreq";   // v0.3 이하 세션 변환용 (파라미터로는 없음)
    inline constexpr const char* presGain = "presGain";
    inline constexpr const char* presQ    = "presQ";
    inline constexpr const char* drive    = "drive";
    inline constexpr const char* output   = "output";

    inline juce::String forModule (const char* base, int module)
    {
        return module <= 0 ? juce::String (base) : juce::String (base) + "2";
    }
}

namespace pillow
{
    // 표시용 문자열 (DAW 자동화 화면과 플러그인 UI에서 같이 사용)
    inline juce::String formatDb (float v)
    {
        const float r = std::round (v * 10.0f) / 10.0f;
        if (std::abs (r) < 0.05f)
            return "0.0 dB";
        return (r > 0.0f ? "+" : "") + juce::String (r, 1) + " dB";
    }

    inline juce::String formatQ (float q)  { return "Q " + juce::String (q, 2); }

    inline juce::String formatHz (float hz)
    {
        if (hz >= 9995.0f) return juce::String (hz / 1000.0f, 1) + " kHz";
        if (hz >= 999.5f)  return juce::String (hz / 1000.0f, 2) + " kHz";
        if (hz >= 99.95f)  return juce::String (juce::roundToInt (hz)) + " Hz";
        const float r = std::round (hz * 10.0f) / 10.0f;
        return (std::abs (r - std::round (r)) < 0.05f ? juce::String (juce::roundToInt (r)) : juce::String (r, 1)) + " Hz";
    }

    // "2.8k", "2800", "2.8 kHz" 모두 받음. 단위 없이 범위보다 작은 수를 치면 kHz로 봄 (예: Presence에 "2.5")
    inline float parseHz (const juce::String& text, float minHz)
    {
        const auto t = text.trim().toLowerCase();
        float v = t.retainCharacters ("0123456789.").getFloatValue();
        if (t.containsChar ('k') || (v < minHz && v * 1000.0f >= minHz))
            v *= 1000.0f;
        return v;
    }
}

//==============================================================================
class PillowRZAudioProcessor : public juce::AudioProcessor,
                               private juce::Timer
{
public:
    PillowRZAudioProcessor();
    ~PillowRZAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    // 호스트 바이패스 시에도 (HQ 모드면) 오버샘플링 지연만큼 똑같이 지연시켜야 클릭/콤필터가 안 생김
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlockBypassed;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }   // 필터 잔향 여유

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // UI에서도 쓰는 헬퍼 (atomic 읽기라 어느 스레드에서든 안전)
    pillow::rz::Params readParams() const noexcept;
    bool isSteppedMode() const noexcept { return paramSteps->load() >= 0.5f; }
    int getNumChannelsInUse() const noexcept { return numChannelsInUse.load(); }

    // 응답 곡선 계산용 처리 레이트 (지금 소리를 내는 모드 기준). 아직 준비 전이면 48k 기준.
    double getProcessingRate() const noexcept { return processingRate.load(); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

private:
    // 모드 전환: 5 ms 페이드아웃 -> (소리가 0이 되는 그 샘플에서) 경로 교체·초기화 -> 5 ms 페이드인
    enum class Fade { None, Out, In };

    void timerCallback() override;                // 레이턴시 보고 (메시지 스레드, 오디오 스레드는 플래그만)
    int latencyFor (bool hq) const noexcept;
    bool wantsHq() const noexcept;                // 실제로 다른 경로가 되는 경우만 true (176.4k 이상은 늘 false)
    void switchPath (bool toHq) noexcept;         // 오디오 스레드
    void resetActivePath() noexcept;
    void runActivePath (juce::dsp::AudioBlock<float>& block, int numCh) noexcept;
    void applyFade (juce::dsp::AudioBlock<float>& block, int numCh) noexcept;

    struct ModulePtrs
    {
        std::atomic<float>* mode = nullptr;
        std::atomic<float>* treble = nullptr;
        std::atomic<float>* trebleQ = nullptr;
        std::atomic<float>* bass = nullptr;
        std::atomic<float>* bassQ = nullptr;
        std::atomic<float>* tilt = nullptr;
        std::atomic<float>* trebleHz = nullptr;
        std::atomic<float>* bassHz = nullptr;
        std::atomic<float>* presHz = nullptr;
        std::atomic<float>* presGain = nullptr;
        std::atomic<float>* presQ = nullptr;
        std::atomic<float>* drive = nullptr;
        std::atomic<float>* output = nullptr;
    };

    std::atomic<float>* paramQuality = nullptr;
    std::atomic<float>* paramRouting = nullptr;
    std::atomic<float>* paramLink    = nullptr;
    std::atomic<float>* paramSteps   = nullptr;
    std::atomic<float>* paramEqOn    = nullptr;
    std::atomic<float>* paramDriveOn = nullptr;
    std::atomic<float>* paramFocus   = nullptr;
    std::atomic<float>* paramMix     = nullptr;
    std::atomic<float>* paramXfLow   = nullptr;
    std::atomic<float>* paramXfMid   = nullptr;
    std::atomic<float>* paramXfHigh  = nullptr;
    std::array<ModulePtrs, 2> modulePtrs;

    // ZERO LATENCY: 세션 레이트 그대로 (ADAA 안티에일리어싱 + 매칭 EQ), 레이턴시 0
    // HQ: 오버샘플링 (44.1/48k 4배, 88.2/96k 2배, 그 위는 그대로), 레이턴시 = 오버샘플링 필터 지연
    pillow::rz::Engine engineZero, engineHq;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;   // HQ에서 배율이 1보다 클 때만
    double baseRate = 48000.0;
    int hqOrder = 0;
    int maxBlockSize = 0;
    int numProcessChannels = 0;
    bool prepared = false;

    bool  hq = false;                 // 오디오 스레드 전용: 지금 쓰는 경로
    Fade  fade = Fade::None;
    float fadeGain = 1.0f, fadeStep = 0.0f;

    std::atomic<int>    pendingLatency { 0 };
    std::atomic<bool>   latencyDirty { false };
    std::atomic<int>    numChannelsInUse { 2 };
    std::atomic<double> processingRate { 48000.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillowRZAudioProcessor)
};
