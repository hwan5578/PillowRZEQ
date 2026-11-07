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
        inline const juce::Colour graphBg   { 0xff10151b };
        inline const juce::Colour panel     { 0xff2a3846 };   // Klangfilm 모듈풍 블루그레이
        inline const juce::Colour panelEdge { 0xff3e5165 };
        inline const juce::Colour text      { 0xffefe8d8 };
        inline const juce::Colour dim       { 0xff8d9cad };
        inline const juce::Colour accent    { 0xfff2a65a };   // 메인 곡선 / 켜짐 표시
        inline const juce::Colour accent2   { 0xff6cc4bd };
        inline const juce::Colour grid      { 0xff222b35 };
        inline const juce::Colour gridMajor { 0xff2c3743 };
        inline const juce::Colour scale     { 0xffcfc8b8 };
    }

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    juce::Font uiFont (float height, bool bold = false, float kerning = 0.0f);

    // 밴드마다 고정 색 (그래프 점 · 곡선 · 밴드 패널이 같은 색)
    juce::Colour bandColour (int index);

    void drawPanelBackground (juce::Graphics&, juce::Rectangle<float>);
    void drawSmallLabel (juce::Graphics&, const juce::Component&, const juce::String&);
    void drawSectionTitle (juce::Graphics&, int x, int y, const juce::String& title, const juce::String& sub);

    // 밴드 모양 아이콘 (선택 버튼 · 메뉴에서 같이 사용)
    void drawShapeIcon (juce::Graphics&, pillow::rz::BandType, juce::Rectangle<float>, float thickness);

    //==========================================================================
    // 파라미터 조작 (호스트 자동화 · 실행 취소가 한 번의 조작으로 기록되게 제스처로 감쌈)
    juce::RangedAudioParameter* bandParam (PillowRZAudioProcessor&, int band, const char* field);
    void setParameter (juce::RangedAudioParameter*, float value);   // 시작 · 값 · 끝을 한 번에

    // 빈 칸에 밴드 추가 (없으면 -1). 마지막에 used를 켜서, 도중 값으로 소리가 나는 일이 없게
    int addBand (PillowRZAudioProcessor&, pillow::rz::BandType, float freq, float gain,
                 pillow::rz::Placement = pillow::rz::Placement::Stereo);
    void removeBand (PillowRZAudioProcessor&, int band);

    //==========================================================================
    // RZ 스타일 노브
    //  - Shift + 드래그 / 휠: 10배 미세 조정 (드래그 도중에 눌러도 값이 튀지 않음)
    //  - 값 글자를 클릭하면 직접 입력, 노브 더블클릭은 기본값
    class RZKnob : public juce::Slider
    {
    public:
        RZKnob();

        bool large  = false;
        bool dimmed = false;                        // 지금 설정에서 쓰이지 않을 때 흐리게 (조작은 가능)
        std::vector<double> detents;                // 비어 있지 않으면 이 값들에 눈금 (선택형 파라미터)

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        juce::Point<float> lastDragPos, virtualDragPos;
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

        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

        juce::Font getLabelFont (juce::Label&) override;
        juce::Font getPopupMenuFont() override;
    };

    //==========================================================================
    // 가로로 나뉜 선택 버튼 (밴드 모양 · 적용 채널). 파라미터와는 ParameterAttachment로 연결
    class SegmentSelector : public juce::Component,
                            public juce::SettableTooltipClient
    {
    public:
        enum class Style { Text, ShapeIcons };

        SegmentSelector (Style, juce::StringArray labels);

        std::function<void (int)> onChange;
        void setIndex (int);
        int  getIndex() const noexcept { return index; }

        void setItemEnabled (int item, bool enabled);
        void setAccent (juce::Colour c) { accent = c; repaint(); }

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

    private:
        int itemAt (juce::Point<float>) const noexcept;
        juce::Rectangle<float> itemBounds (int) const noexcept;

        Style style;
        juce::StringArray labels;
        std::vector<bool> enabled;
        int index = 0, hover = -1;
        juce::Colour accent = palette::accent;
    };
}
