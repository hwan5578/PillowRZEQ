#pragma once

#include <juce_dsp/juce_dsp.h>
#include "UiCommon.h"

namespace pillow::ui
{
    //==========================================================================
    // Pro-Q 방식 그래프
    //  - 실제 DSP와 같은 계수로 계산한 전체 곡선 (L/R · M/S 밴드가 있으면 채널별 곡선)
    //  - 밴드마다 점 하나: 드래그 = 주파수 · 게인 (컷 · 노치는 위아래 = Q), Shift = 미세 조정
    //  - 휠 = Q, 빈 곳 더블클릭 = 밴드 추가, 오른쪽 클릭 = 메뉴, Alt + 클릭 = 켜기/끄기, Delete = 삭제
    //  - 뒤에 실시간 스펙트럼 (처리 전 선 + 처리 후 면)
    //==========================================================================
    class GraphView : public juce::Component, private juce::Timer
    {
    public:
        explicit GraphView (PillowRZAudioProcessor&);
        ~GraphView() override;

        std::function<void (int)> onSelect;          // 화면에서 밴드를 고르면 (-1 = 선택 없음)
        void setSelectedBand (int band);             // 바깥(밴드 패널)에서 고를 때 (onSelect 안 부름)
        int  getSelectedBand() const noexcept { return selected; }

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        bool keyPressed (const juce::KeyPress&) override;

        // 좌표 변환 (확인용으로 공개)
        float  xForFreq (double hz) const noexcept;
        double freqForX (float x) const noexcept;
        float  yForDb (double db) const noexcept;
        double dbForY (float y) const noexcept;
        juce::Point<float> nodePosition (int band) const noexcept;   // 화면 안으로 잘린 위치
        void refreshNow();                                           // 타이머를 기다리지 않고 바로 반영

    private:
        static constexpr double kMinHz = 10.0, kMaxHz = 30000.0;
        static constexpr double kSpecTopDb = 6.0, kSpecBottomDb = -96.0, kSpecFloorDb = -140.0;
        static constexpr int    kPointStep = 2;          // 곡선 점 간격 (px)

        void timerCallback() override;
        bool refreshParams();
        void rebuildCurves();

        void ensureFft (double sessionRate);
        void updateAnalyzer();
        void rebuildSpectrumPaths();
        float ySpec (double db) const noexcept;

        int  bandAt (juce::Point<float>) const noexcept;
        void select (int band);
        void showBandMenu (int band);
        void showAddMenu (juce::Point<float>);
        void handleBandMenu (int band, int result);
        void cycleRange();
        void cycleAnalyzer();
        void applyUiSettings (int range, int analyzer);
        void endDrag();

        PillowRZAudioProcessor& proc;
        pillow::rz::Params params;
        double rate = 48000.0;
        int channels = 2;
        bool hasParams = false;

        juce::Rectangle<float> plot;          // 그래프 전체
        float eqTop = 0.0f, eqBottom = 0.0f;  // dB 눈금 영역
        int rangeDb = 12;                     // 6 / 12 / 24
        int analyzerMode = 2;                 // 0 끔, 1 POST, 2 PRE + POST
        juce::Rectangle<float> rangeButton, analyzerButton;

        struct Curve { juce::Path path; juce::Colour colour; juce::String name; bool dashed = false; };
        std::vector<Curve> totals;
        std::array<juce::Path, pillow::rz::kMaxBands> bandCurves;
        juce::Path selectedFill;

        int selected = -1, hovered = -1, dragging = -1;
        int dragSource = -1;                  // 드래그 중인 포인터 (다른 손가락 · 펜은 무시)
        juce::Point<float> dragVirtual, dragLast, dragStartNode, mousePos;
        bool mouseInside = false, dragYIsQ = false;
        float dragStartQ = 1.0f;
        juce::RangedAudioParameter* dragFreqParam = nullptr;
        juce::RangedAudioParameter* dragYParam = nullptr;

        // 분석기
        std::unique_ptr<juce::dsp::FFT> fft;
        int fftOrder = 0, fftSize = 0;
        double fftRate = 0.0;
        std::vector<float> window, fftData, pullBuffer, pointFreq;

        struct Spectrum
        {
            std::vector<float> ring, level;
            int writePos = 0, silentTicks = 0;
            bool fresh = false;
            juce::Path line, fill;
        };
        std::array<Spectrum, 2> spectra;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphView)
    };
}
