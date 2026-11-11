#pragma once

#include "UiCommon.h"

namespace pillow::ui
{
    //==========================================================================
    // 고른 밴드 하나의 설정 (그래프에서 점을 누르면 이 밴드로 바뀜)
    //  윗줄: 모양 · 적용 채널 · 켜기 · 듣기 · 삭제 + FREQ · GAIN · Q · SLOPE 노브
    //  아랫줄 탭:
    //   ANALOG   - 이 밴드만의 EF804S 튜브 (TUBE + DRIVE · FOCUS · MIX), 트랜스포머 (XFMR + AMOUNT)
    //   DYNAMICS - 다이내믹 EQ (DYN + RANGE · THRESHOLD/AUTO · ATTACK · RELEASE), 스펙트럴 (SPECTRAL + DENSITY)
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
        void showTab (int tab);                  // 0 = ANALOG, 1 = DYNAMICS (화면 상태로 저장)
        int  getTab() const noexcept { return tab; }

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        void attach (int band);
        bool knobBeingDragged() const;
        void updateTypeDependent();
        void updateVisibility();
        void stopListening();
        static juce::String transformerCharacter (const pillow::rz::BandParams&);
        juce::Rectangle<float> chipBounds (int slot) const noexcept;
        std::array<RZKnob*, 13> allKnobs() noexcept
        {
            return { &freqKnob, &gainKnob, &qKnob, &slopeKnob, &driveKnob, &focusKnob, &mixKnob, &xfAmtKnob,
                     &rangeKnob, &threshKnob, &attackKnob, &releaseKnob, &densityKnob };
        }

        PillowRZAudioProcessor& proc;
        int selected = -1, tab = 0;
        int attachedBand = -1, pendingAttach = -2;   // -2 = 대기 없음
        const juce::Slider* draggingKnob = nullptr;  // 지금 드래그 중인 노브 (제스처가 열려 있음)
        pillow::rz::Params params;
        float dynShown = 0.0f;                       // 지금 게인 변화 (표시)

        RZKnob freqKnob, gainKnob, qKnob, slopeKnob;
        RZKnob driveKnob, focusKnob, mixKnob, xfAmtKnob;
        RZKnob rangeKnob, threshKnob, attackKnob, releaseKnob, densityKnob;
        SegmentSelector shapes    { SegmentSelector::Style::ShapeIcons, pillow::bandTypeNames() };
        SegmentSelector placement { SegmentSelector::Style::Text, { "STEREO", "LEFT", "RIGHT", "MID", "SIDE" } };
        SegmentSelector tabs      { SegmentSelector::Style::Text, { "ANALOG", "DYNAMICS" } };
        juce::ToggleButton onButton, tubeButton, xfmrButton, dynButton, autoButton, specButton, listenButton;
        juce::TextButton removeButton { "REMOVE" };

        // 어태치먼트는 컨트롤보다 뒤에 선언 -> 먼저 파괴됨
        std::unique_ptr<SliderAttachment> freqAtt, gainAtt, qAtt, slopeAtt, driveAtt, focusAtt, mixAtt, xfAmtAtt;
        std::unique_ptr<SliderAttachment> rangeAtt, threshAtt, attackAtt, releaseAtt, densityAtt;
        std::unique_ptr<ButtonAttachment> onAtt, tubeAtt, xfmrAtt, dynAtt, autoAtt, specAtt;
        std::unique_ptr<juce::ParameterAttachment> shapeAtt, placeAtt;

        std::array<int, pillow::rz::kMaxBands> chipBand {};   // 칩 자리 -> 밴드 번호
        int numChips = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandPanel)
    };

    //==========================================================================
    // 메인: EQ 전체 켜기, 신호 전체에 거는 EF804S 튜브 (ALL · DRIVE · FOCUS) · 출력 트랜스포머 (ALL · AMOUNT),
    //  출력 (MIX = 플러그인 전체 원음/처리음, LEVEL). 밴드마다의 튜브 · 트랜스포머는 밴드 패널에서 따로
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
        RZKnob drive, focus, xfAmount, mix, output;
        juce::ToggleButton eqSwitch, tubeAllSwitch, xfAllSwitch;

        std::vector<std::unique_ptr<SliderAttachment>> attachments;
        std::unique_ptr<ButtonAttachment> eqAttachment, tubeAllAttachment, xfAllAttachment;

        juce::String tubeUsage, xfUsage;      // "신호 전체" / "밴드 2, 5 따로" / 꺼짐

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainPanel)
    };
}
