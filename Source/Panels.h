#pragma once

#include "UiCommon.h"

namespace pillow::ui
{
    //==========================================================================
    // 고른 밴드 하나의 설정 (그래프에서 점을 누르면 이 밴드로 바뀜)
    //  모양 · 적용 채널 · 켜기 · 삭제 + FREQ · GAIN · Q · SLOPE 노브
    //  위쪽 번호 칩: 쓰고 있는 밴드 전체 (눌러서 고르기)
    //==========================================================================
    class BandPanel : public juce::Component, private juce::Timer
    {
    public:
        explicit BandPanel (PillowRZAudioProcessor&);
        ~BandPanel() override;

        std::function<void (int)> onSelect;      // 칩 · 삭제로 선택이 바뀌면
        void setSelectedBand (int band);
        int  getSelectedBand() const noexcept { return selected; }

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        void attach (int band);
        bool knobBeingDragged() const;
        void updateTypeDependent();
        juce::Rectangle<float> chipBounds (int slot) const noexcept;

        PillowRZAudioProcessor& proc;
        int selected = -1;
        int attachedBand = -1, pendingAttach = -2;   // -2 = 대기 없음
        const juce::Slider* draggingKnob = nullptr;  // 지금 드래그 중인 노브 (제스처가 열려 있음)
        pillow::rz::Params params;

        RZKnob freqKnob, gainKnob, qKnob, slopeKnob;
        SegmentSelector shapes    { SegmentSelector::Style::ShapeIcons, pillow::bandTypeNames() };
        SegmentSelector placement { SegmentSelector::Style::Text, { "STEREO", "LEFT", "RIGHT", "MID", "SIDE" } };
        juce::ToggleButton onButton;
        juce::TextButton removeButton { "REMOVE" };

        // 어태치먼트는 컨트롤보다 뒤에 선언 -> 먼저 파괴됨
        std::unique_ptr<SliderAttachment> freqAtt, gainAtt, qAtt, slopeAtt;
        std::unique_ptr<ButtonAttachment> onAtt;
        std::unique_ptr<juce::ParameterAttachment> shapeAtt, placeAtt;

        std::array<int, pillow::rz::kMaxBands> chipBand {};   // 칩 자리 -> 밴드 번호
        int numChips = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandPanel)
    };

    //==========================================================================
    // 메인 (신호 전체): EQ 전체 켜기, EF804S 튜브, 출력 트랜스포머, 출력
    //==========================================================================
    class MainPanel : public juce::Component, private juce::Timer
    {
    public:
        explicit MainPanel (PillowRZAudioProcessor&);
        ~MainPanel() override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void setupKnob (RZKnob&, const char* paramId, bool large);

        PillowRZAudioProcessor& proc;
        RZKnob drive, focus, mix, xfLow, xfMid, xfHigh, output;
        juce::ToggleButton eqSwitch, driveSwitch;

        std::vector<std::unique_ptr<SliderAttachment>> attachments;
        std::unique_ptr<ButtonAttachment> eqAttachment, driveAttachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainPanel)
    };
}
