#pragma once

#include "UiCommon.h"
#include "GraphView.h"
#include "Panels.h"

//==============================================================================
// v0.8 화면
//  ┌ 머리: 이름 · 레이턴시 · ZERO LATENCY / HQ ─────────────────────┐
//  │ 그래프 (밴드 점 · 곡선 · 다이내믹 · 분석기)│ 메인: EQ              │
//  │                                            │  EF804S 튜브 (ALL)    │
//  ├ 밴드 패널 (고른 밴드 하나의 설정) ─────────┤  트랜스포머 (ALL)     │
//  │  모양 · 채널 · 켜기 · 듣기 · FREQ · GAIN · Q · SLOPE   │ 출력      │
//  │  [ANALOG]   TUBE (DRIVE · FOCUS · MIX) · XFMR (AMOUNT) │           │
//  │  [DYNAMICS] DYN (RANGE · THRESHOLD · ATTACK · RELEASE) · SPECTRAL  │
//  └────────────────────────────────────────────┴───────────────────────┘
//==============================================================================
class PillowRZAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit PillowRZAudioProcessorEditor (PillowRZAudioProcessor&);
    ~PillowRZAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    pillow::ui::GraphView& getGraph() noexcept     { return graph; }       // 확인용
    pillow::ui::BandPanel& getBandPanel() noexcept { return bandPanel; }

private:
    void timerCallback() override;          // 레이턴시 표시

    PillowRZAudioProcessor& processorRef;
    int shownLatency = -1;
    pillow::ui::RZLookAndFeel lnf;

    pillow::ui::GraphView graph;
    pillow::ui::BandPanel bandPanel;
    pillow::ui::MainPanel mainPanel;
    juce::ToggleButton qualitySwitch;
    std::unique_ptr<pillow::ui::ButtonAttachment> qualityAttachment;
    juce::TooltipWindow tooltips { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillowRZAudioProcessorEditor)
};
