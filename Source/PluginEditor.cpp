#include "PluginEditor.h"

using namespace pillow::ui;

namespace
{
    constexpr int kWidth  = 1080;
    constexpr int kHeight = 700;
}

//==============================================================================
PillowRZAudioProcessorEditor::PillowRZAudioProcessorEditor (PillowRZAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), graph (p), bandPanel (p), mainPanel (p)
{
    addAndMakeVisible (graph);
    addAndMakeVisible (bandPanel);
    addAndMakeVisible (mainPanel);

    // ZERO LATENCY(기본) / HQ(오버샘플링, 수 샘플 지연)
    qualitySwitch.getProperties().set ("rzStyle", "segmented");
    qualitySwitch.getProperties().set ("rzLeft",  "ZERO LATENCY");
    qualitySwitch.getProperties().set ("rzRight", "HQ");
    qualitySwitch.setButtonText ("Quality");
    qualitySwitch.setTooltip ("ZERO LATENCY: no delay, analog-matched curves\n"
                              "HQ: oversampled (a few samples of delay), cleanest drive and air bands");
    qualitySwitch.setWantsKeyboardFocus (false);   // Return / 숫자패드 Enter가 모드를 바꾸지 않게
    addAndMakeVisible (qualitySwitch);
    qualityAttachment = std::make_unique<ButtonAttachment> (processorRef.apvts, pillow::ids::quality, qualitySwitch);

    // 그래프에서 점을 고르면 아래 패널이 그 밴드로, 패널에서 고르면 그래프 점도
    graph.onSelect     = [this] (int band) { bandPanel.setSelectedBand (band); };
    bandPanel.onSelect = [this] (int band) { graph.setSelectedBand (band); };

    setMouseClickGrabsKeyboardFocus (false);

    // 자식 컴포넌트를 다 만든 뒤에 지정해야 노브 값 표시(텍스트 박스)까지 새 색으로 다시 만들어짐
    setLookAndFeel (&lnf);
    setSize (kWidth, kHeight);
}

PillowRZAudioProcessorEditor::~PillowRZAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

bool PillowRZAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    // 포커스가 노브 값 표시 등에 있어도 Delete · Backspace · Esc 는 그래프(고른 밴드)로. 나머지 키는 호스트로
    return graph.keyPressed (key);
}

void PillowRZAudioProcessorEditor::resized()
{
    qualitySwitch.setBounds (kWidth - 16 - 220, 14, 220, 28);

    graph.setBounds     (16,  60, 732, 392);
    bandPanel.setBounds (16, 464, 732, 224);
    mainPanel.setBounds (760, 60, 304, 628);
}

void PillowRZAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient (palette::bg, 0.0f, 0.0f,
                                             palette::bgDark, 0.0f, (float) getHeight(), false));
    g.fillAll();

    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f, true, 0.3f));
    g.drawText ("PILLOW AUDIO", 22, 10, 220, 14, juce::Justification::centredLeft, false);
    g.setColour (palette::text);
    g.setFont (uiFont (24.0f, true));
    g.drawText ("RZ EQ", 20, 23, 110, 30, juce::Justification::centredLeft, false);
    g.setColour (palette::dim);
    g.setFont (uiFont (11.5f));
    g.drawText ("free bands  /  RZ062 curves  /  EF804S", 106, 32, 300, 16, juce::Justification::centredLeft, false);
}
