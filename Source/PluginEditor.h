#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

namespace pillow::ui
{
    //==========================================================================
    namespace palette
    {
        inline const juce::Colour bg        { 0xff1c232c };
        inline const juce::Colour bgDark    { 0xff131820 };
        inline const juce::Colour panel     { 0xff2a3846 };   // Klangfilm 모듈풍 블루그레이
        inline const juce::Colour panelEdge { 0xff3e5165 };
        inline const juce::Colour text      { 0xffefe8d8 };
        inline const juce::Colour dim       { 0xff8d9cad };
        inline const juce::Colour accent    { 0xfff2a65a };   // 응답 곡선 / 활성 표시
        inline const juce::Colour grid      { 0xff27313c };
        inline const juce::Colour scale     { 0xffcfc8b8 };
    }

    //==========================================================================
    // 원본처럼 디텐트(계단) 눈금을 표시하고, 스텝 모드에서 포인터가 계단에 붙는 노브
    class RZKnob : public juce::Slider
    {
    public:
        RZKnob();

        std::function<bool()> isStepped;            // 스텝 모드 여부
        std::function<double (double)> quantize;    // 값 -> 실제 적용되는 스텝 값
        std::vector<double> detents;                // 눈금 위치(값)
        bool large = false;
    };

    //==========================================================================
    class RZLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        RZLookAndFeel();

        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPosProportional, float rotaryStartAngle,
                               float rotaryEndAngle, juce::Slider&) override;

        void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

        juce::Font getLabelFont (juce::Label&) override;
    };

    //==========================================================================
    // 실제 DSP와 같은 계수로 계산한 응답 곡선
    class ResponseView : public juce::Component, private juce::Timer
    {
    public:
        explicit ResponseView (PillowRZAudioProcessor&);
        ~ResponseView() override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void rebuildPath();

        juce::Rectangle<float> plotArea() const;
        static float xForFreq (double f, juce::Rectangle<float> plot);
        static float yForDb (double db, juce::Rectangle<float> plot);

        static constexpr double kTopDb = 18.0, kBottomDb = -24.0;

        PillowRZAudioProcessor& proc;
        pillow::rz::Params params;
        double rate = 0.0;
        bool hasParams = false;

        juce::Path curve, fill;
        juce::Point<float> marker;
        bool showMarker = false;
    };
}

//==============================================================================
class PillowRZAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PillowRZAudioProcessorEditor (PillowRZAudioProcessor&);
    ~PillowRZAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateModeVisibility();
    void setupKnob (pillow::ui::RZKnob& knob, const char* paramId, bool large);

    PillowRZAudioProcessor& processorRef;
    pillow::ui::RZLookAndFeel lnf;

    pillow::ui::ResponseView response;
    pillow::ui::RZKnob treble, trebleQ, bass, bassQ, tilt, presGain, presFreq, presQ, tube, output;
    juce::ToggleButton modeSwitch, stepsSwitch;

    // 어태치먼트는 노브보다 뒤에 선언 -> 먼저 파괴됨
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::unique_ptr<ButtonAttachment> modeAttachment, stepsAttachment;

    bool lastModeB = true, lastStepped = false;

    juce::Rectangle<int> modulePanel, rowTreble, rowMid, rowBass, footer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillowRZAudioProcessorEditor)
};
