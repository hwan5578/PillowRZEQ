#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/RZ062Core.h"

namespace pillow::ids
{
    inline constexpr const char* mode     = "mode";
    inline constexpr const char* steps    = "steps";
    inline constexpr const char* treble   = "treble";
    inline constexpr const char* trebleQ  = "trebleQ";
    inline constexpr const char* bass     = "bass";
    inline constexpr const char* bassQ    = "bassQ";
    inline constexpr const char* tilt     = "tilt";
    inline constexpr const char* presFreq = "presFreq";
    inline constexpr const char* presGain = "presGain";
    inline constexpr const char* presQ    = "presQ";
    inline constexpr const char* tube     = "tube";
    inline constexpr const char* output   = "output";
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
}

//==============================================================================
class PillowRZAudioProcessor : public juce::AudioProcessor
{
public:
    PillowRZAudioProcessor();
    ~PillowRZAudioProcessor() override = default;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    // 호스트 바이패스 시에도 오버샘플링 지연만큼 똑같이 지연시켜야 클릭/콤필터가 안 생김
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
    bool isModeB() const noexcept       { return paramMode->load() >= 0.5f; }
    bool isSteppedMode() const noexcept { return paramSteps->load() >= 0.5f; }

    // 응답 곡선 계산용 처리 레이트 (오버샘플링 포함). 아직 준비 전이면 48k 기준.
    double getProcessingRate() const noexcept { return processingRate.load(); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

private:
    std::atomic<float>* paramMode     = nullptr;
    std::atomic<float>* paramSteps    = nullptr;
    std::atomic<float>* paramTreble   = nullptr;
    std::atomic<float>* paramTrebleQ  = nullptr;
    std::atomic<float>* paramBass     = nullptr;
    std::atomic<float>* paramBassQ    = nullptr;
    std::atomic<float>* paramTilt     = nullptr;
    std::atomic<float>* paramPresFreq = nullptr;
    std::atomic<float>* paramPresGain = nullptr;
    std::atomic<float>* paramPresQ    = nullptr;
    std::atomic<float>* paramTube     = nullptr;
    std::atomic<float>* paramOutput   = nullptr;

    pillow::rz::Engine engine;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int maxBlockSize = 0;
    int numProcessChannels = 0;
    std::atomic<double> processingRate { pillow::rz::processingRateFor (48000.0) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillowRZAudioProcessor)
};
