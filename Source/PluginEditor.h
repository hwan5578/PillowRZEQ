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
        inline const juce::Colour accent    { 0xfff2a65a };   // 모듈 1 곡선 / 활성 표시
        inline const juce::Colour accent2   { 0xff6cc4bd };   // 모듈 2 곡선
        inline const juce::Colour grid      { 0xff27313c };
        inline const juce::Colour scale     { 0xffcfc8b8 };
    }

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    //==========================================================================
    // 원본처럼 디텐트(계단) 눈금을 표시하고, 스텝 모드에서 포인터가 계단에 붙는 노브
    class RZKnob : public juce::Slider
    {
    public:
        RZKnob();

        std::function<bool()> isStepped;            // 스텝 모드 여부
        std::function<double (double)> quantize;    // 값 -> 실제 적용되는 스텝 값
        std::vector<double> detents;                // 눈금 위치(값)
        bool large  = false;
        bool dimmed = false;                        // 기능이 꺼져 있을 때 흐리게 (조작은 가능)
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
    // 모듈 하나 (원본 RZ062 한 대): Treble / Pivot·Presence / Bass / Drive·Level
    class ModulePanel : public juce::Component
    {
    public:
        struct State
        {
            bool modeB = true, linked = true, mono = false, midSide = false;
            bool stepped = false, eqOn = true, driveOn = false;

            bool operator== (const State& o) const noexcept
            {
                return modeB == o.modeB && linked == o.linked && mono == o.mono && midSide == o.midSide
                    && stepped == o.stepped && eqOn == o.eqOn && driveOn == o.driveOn;
            }
            bool operator!= (const State& o) const noexcept { return ! (*this == o); }
        };

        ModulePanel (PillowRZAudioProcessor&, int moduleIndex);

        void paint (juce::Graphics&) override;
        void paintOverChildren (juce::Graphics&) override;
        void resized() override;

        void setState (const State&);

    private:
        void setupKnob (RZKnob&, const char* baseId, bool large);
        juce::String title() const;
        bool isFollower() const noexcept { return index == 1 && (state.linked || state.mono); }

        PillowRZAudioProcessor& proc;
        const int index;
        State state;
        bool hasState = false;

        RZKnob treble, trebleQ, bass, bassQ, tilt, presGain, presFreq, presQ, drive, level;
        juce::ToggleButton typeSwitch;

        // 어태치먼트는 노브보다 뒤에 선언 -> 먼저 파괴됨
        std::vector<std::unique_ptr<SliderAttachment>> attachments;
        std::unique_ptr<ButtonAttachment> typeAttachment;

        juce::Rectangle<int> rowTreble, rowMid, rowBass, rowTube;
    };

    //==========================================================================
    // 튜브(EF804S) 공통 설정 + 출력 트랜스포머 구간별 색채
    class GlobalPanel : public juce::Component
    {
    public:
        explicit GlobalPanel (PillowRZAudioProcessor&);

        void paint (juce::Graphics&) override;
        void resized() override;

        void setDriveOn (bool on);

    private:
        void setupKnob (RZKnob&, const char* paramId);

        PillowRZAudioProcessor& proc;
        RZKnob focus, mix, xfLow, xfMid, xfHigh;
        juce::ToggleButton eqSwitch, driveSwitch;

        std::vector<std::unique_ptr<SliderAttachment>> attachments;
        std::unique_ptr<ButtonAttachment> eqAttachment, driveAttachment;

        bool driveOn = false;
    };

    //==========================================================================
    // 실제 DSP와 같은 계수로 계산한 응답 곡선 (모듈 1, 링크가 풀려 있으면 모듈 2도)
    class ResponseView : public juce::Component, private juce::Timer
    {
    public:
        explicit ResponseView (PillowRZAudioProcessor&);
        ~ResponseView() override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void rebuildPaths();
        int numCurves() const noexcept;

        juce::Rectangle<float> plotArea() const;
        static float xForFreq (double f, juce::Rectangle<float> plot);
        static float yForDb (double db, juce::Rectangle<float> plot);

        static constexpr double kTopDb = 18.0, kBottomDb = -24.0;

        PillowRZAudioProcessor& proc;
        pillow::rz::Params params;
        double rate = 0.0;
        int channels = 2;
        bool hasParams = false;

        std::array<juce::Path, 2> curves, fills;
        std::array<juce::Point<float>, 2> markers;
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
    void refreshState();

    PillowRZAudioProcessor& processorRef;
    pillow::ui::RZLookAndFeel lnf;

    pillow::ui::ResponseView response;
    pillow::ui::ModulePanel module1, module2;
    pillow::ui::GlobalPanel global;
    juce::ToggleButton routingSwitch, linkSwitch, stepsSwitch;

    std::unique_ptr<pillow::ui::ButtonAttachment> routingAttachment, linkAttachment, stepsAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillowRZAudioProcessorEditor)
};
