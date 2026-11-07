#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/RZ062Core.h"
#include "AnalyzerTap.h"

namespace pillow::ids
{
    // 전역 — v0.4 이하와 같은 ID는 의미·범위도 같음 (기존 자동화가 그대로 이어짐)
    inline constexpr const char* quality = "quality";   // 0 = ZERO LATENCY, 1 = HQ
    inline constexpr const char* eqOn    = "eqOn";
    inline constexpr const char* driveOn = "driveOn";   // 튜브를 신호 전체에 (ALL). v0.5까지는 튜브 켜기 스위치
    inline constexpr const char* drive   = "drive";     // EF804S 드라이브 (v0.4까지는 모듈 1의 드라이브)
    inline constexpr const char* focus   = "focus";
    inline constexpr const char* mix     = "mix";
    inline constexpr const char* xfLow   = "xfLow";
    inline constexpr const char* xfMid   = "xfMid";
    inline constexpr const char* xfHigh  = "xfHigh";
    inline constexpr const char* output  = "output";    // 최종 출력 (v0.4까지는 모듈 1의 Level)
    inline constexpr const char* xfAll   = "xfAll";     // v0.6: 트랜스포머를 신호 전체에 (ALL)

    // 밴드 항목 — 실제 ID는 band (i, 항목) = "b1_freq" ... "b16_place"
    inline constexpr const char* used  = "used";
    inline constexpr const char* on    = "on";
    inline constexpr const char* type  = "type";
    inline constexpr const char* freq  = "freq";
    inline constexpr const char* gain  = "gain";
    inline constexpr const char* q     = "q";
    inline constexpr const char* slope = "slope";
    inline constexpr const char* place = "place";
    inline constexpr const char* tube  = "tube";        // v0.6: 이 밴드 부분을 EF804S로
    inline constexpr const char* xfmr  = "xfmr";        // v0.6: 이 밴드 부분을 트랜스포머로

    inline juce::String band (int index, const char* field)
    {
        return "b" + juce::String (index + 1) + "_" + field;
    }

    // 화면 설정 (파라미터가 아니라 상태 트리의 속성으로 저장)
    inline constexpr const char* uiRange    = "uiRange";      // 그래프 범위 6 / 12 / 24 dB
    inline constexpr const char* uiAnalyzer = "uiAnalyzer";   // 0 = 끔, 1 = POST, 2 = PRE + POST
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

    inline juce::String formatQ (float q)  { return q >= 9.995f ? juce::String (q, 1) : juce::String (q, 2); }

    inline juce::String formatHz (float hz)
    {
        if (hz >= 9995.0f) return juce::String (hz / 1000.0f, 1) + " kHz";
        if (hz >= 999.5f)  return juce::String (hz / 1000.0f, 2) + " kHz";
        if (hz >= 99.95f)  return juce::String (juce::roundToInt (hz)) + " Hz";
        const float r = std::round (hz * 10.0f) / 10.0f;
        return (std::abs (r - std::round (r)) < 0.05f ? juce::String (juce::roundToInt (r)) : juce::String (r, 1)) + " Hz";
    }

    // "2.8k", "2800", "2.8 kHz" 모두 받음
    inline float parseHz (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        float v = t.retainCharacters ("0123456789.").getFloatValue();
        if (t.containsChar ('k'))
            v *= 1000.0f;
        return v;
    }

    inline juce::StringArray bandTypeNames()  { return { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt" }; }
    inline juce::StringArray placementNames() { return { "Stereo", "Left", "Right", "Mid", "Side" }; }
    inline juce::StringArray slopeNames()
    {
        juce::StringArray s;
        for (int v : rz::kCutSlopes) s.add (juce::String (v) + " dB/oct");
        return s;
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
    double getTailLengthSeconds() const override { return 0.5; }   // 낮은 주파수 · 높은 Q 밴드의 잔향 여유

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
    int getNumChannelsInUse() const noexcept { return numChannelsInUse.load(); }

    // 응답 곡선 계산용 처리 레이트 (지금 소리를 내는 모드 기준). 아직 준비 전이면 48k 기준.
    double getProcessingRate() const noexcept { return processingRate.load(); }
    double getSessionRate() const noexcept    { return sessionRate.load(); }

    // 분석기: 0 = 처리 전(PRE), 1 = 처리 후(POST). 에디터가 열려 있을 때만 채움
    pillow::AnalyzerTap& getAnalyzerTap (int which) noexcept { return taps[(size_t) juce::jlimit (0, 1, which)]; }
    void setAnalyzerActive (bool shouldBeActive) noexcept { analyzerActive.store (shouldBeActive); }

    // v0.4 이하 세션(모듈 방식)을 밴드로 바꿈. 바꿨으면 true (테스트에서도 사용)
    static bool migrateLegacyState (juce::ValueTree& state);

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
    void pushAnalyzer (int which, const juce::AudioBuffer<float>& buffer, int numCh) noexcept;
    void fillMissingParameters (juce::ValueTree& state) const;

    struct BandPtrs
    {
        std::atomic<float>* used  = nullptr;
        std::atomic<float>* on    = nullptr;
        std::atomic<float>* type  = nullptr;
        std::atomic<float>* freq  = nullptr;
        std::atomic<float>* gain  = nullptr;
        std::atomic<float>* q     = nullptr;
        std::atomic<float>* slope = nullptr;
        std::atomic<float>* place = nullptr;
        std::atomic<float>* tube  = nullptr;
        std::atomic<float>* xfmr  = nullptr;
    };

    std::atomic<float>* paramQuality = nullptr;
    std::atomic<float>* paramEqOn    = nullptr;
    std::atomic<float>* paramDriveOn = nullptr;
    std::atomic<float>* paramDrive   = nullptr;
    std::atomic<float>* paramFocus   = nullptr;
    std::atomic<float>* paramMix     = nullptr;
    std::atomic<float>* paramXfLow   = nullptr;
    std::atomic<float>* paramXfMid   = nullptr;
    std::atomic<float>* paramXfHigh  = nullptr;
    std::atomic<float>* paramOutput  = nullptr;
    std::atomic<float>* paramXfAll   = nullptr;
    std::array<BandPtrs, pillow::rz::kMaxBands> bandPtrs;

    // ZERO LATENCY: 세션 레이트 그대로 (ADAA 안티에일리어싱 + 매칭 EQ), 레이턴시 0
    // HQ: 오버샘플링 (44.1/48k 4배, 88.2/96k 2배, 그 위는 그대로), 레이턴시 = 오버샘플링 필터 지연
    pillow::rz::Engine engineZero, engineHq;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;   // HQ에서 배율이 1보다 클 때만
    double baseRate = 48000.0;
    int hqOrder = 0;
    int maxBlockSize = 0;
    int numProcessChannels = 0;
    std::atomic<bool> prepared { false };
    std::atomic<int>  hqLatency { 0 };   // HQ로 처리할 때의 레이턴시 (이 세션 레이트에서)

    bool  hq = false;                 // 오디오 스레드 전용: 지금 쓰는 경로
    Fade  fade = Fade::None;
    float fadeGain = 1.0f, fadeStep = 0.0f;

    std::atomic<int>    pendingLatency { 0 };
    std::atomic<bool>   latencyDirty { false };
    std::atomic<int>    numChannelsInUse { 2 };
    std::atomic<double> processingRate { 48000.0 };
    std::atomic<double> sessionRate { 48000.0 };

    std::array<pillow::AnalyzerTap, 2> taps;
    std::atomic<bool> analyzerActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillowRZAudioProcessor)
};
