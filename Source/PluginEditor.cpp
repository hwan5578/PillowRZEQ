#include "PluginEditor.h"

namespace rz = pillow::rz;
using namespace pillow::ui;

namespace
{
    constexpr int kWidth  = 900;
    constexpr int kHeight = 540;

    juce::Font uiFont (float height, bool bold = false, float kerning = 0.0f)
    {
        juce::Font f (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
        return kerning > 0.0f ? f.withExtraKerningFactor (kerning) : f;
    }
}

//==============================================================================
//  RZKnob
//==============================================================================
RZKnob::RZKnob()
    : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow)
{
    setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                         juce::MathConstants<float>::pi * 2.75f, true);
    setMouseDragSensitivity (220);
}

//==============================================================================
//  LookAndFeel
//==============================================================================
RZLookAndFeel::RZLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId,       palette::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId,  palette::accent.withAlpha (0.4f));
    setColour (juce::Label::textColourId,               palette::text);
    setColour (juce::Label::textWhenEditingColourId,    palette::text);
    setColour (juce::TextEditor::textColourId,          palette::text);
    setColour (juce::TextEditor::backgroundColourId,    palette::bgDark);
    setColour (juce::TextEditor::highlightColourId,     palette::accent.withAlpha (0.4f));
    setColour (juce::CaretComponent::caretColourId,     palette::accent);
}

juce::Font RZLookAndFeel::getLabelFont (juce::Label&)
{
    return uiFont (12.5f);
}

void RZLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPos, float startAngle, float endAngle,
                                      juce::Slider& slider)
{
    auto* knob = dynamic_cast<RZKnob*> (&slider);
    const bool stepped = knob != nullptr && knob->isStepped && knob->quantize && knob->isStepped();

    if (stepped)
        sliderPos = (float) slider.valueToProportionOfLength (knob->quantize (slider.getValue()));

    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (2.0f);
    const float size   = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto  area   = bounds.withSizeKeepingCentre (size, size);
    const auto  centre = area.getCentre();
    const float radius = size * 0.5f;
    const float alpha  = slider.isEnabled() ? 1.0f : 0.35f;
    const bool  large  = knob != nullptr && knob->large;

    // 눈금: 스텝 모드면 실제 디텐트 위치, 아니면 균등 11칸
    auto tick = [&] (float proportion, float len, float thick)
    {
        const float a  = startAngle + proportion * (endAngle - startAngle);
        const auto  p1 = centre.getPointOnCircumference (radius - 1.0f, a);
        const auto  p2 = centre.getPointOnCircumference (radius - 1.0f - len, a);
        g.drawLine (juce::Line<float> (p1, p2), thick);
    };

    g.setColour (palette::scale.withMultipliedAlpha (0.75f * alpha));
    if (stepped && knob != nullptr && ! knob->detents.empty())
    {
        for (double d : knob->detents)
            tick ((float) slider.valueToProportionOfLength (d), large ? 7.0f : 5.0f, 1.6f);
    }
    else
    {
        for (int i = 0; i <= 10; ++i)
            tick ((float) i / 10.0f, (i % 5 == 0) ? (large ? 7.0f : 5.0f) : 3.5f, (i % 5 == 0) ? 1.6f : 1.0f);
    }

    // 노브 몸체 (검은 베이클라이트)
    const float knobR    = radius * (large ? 0.74f : 0.70f);
    const auto  knobArea = juce::Rectangle<float> (knobR * 2.0f, knobR * 2.0f).withCentre (centre);

    g.setColour (juce::Colours::black.withAlpha (0.45f * alpha));
    g.fillEllipse (knobArea.translated (0.0f, 2.5f).expanded (1.0f));

    juce::ColourGradient body (juce::Colour (0xff474b52).withMultipliedAlpha (alpha),
                               centre.x - knobR * 0.45f, centre.y - knobR * 0.65f,
                               juce::Colour (0xff0c0d0f).withMultipliedAlpha (alpha),
                               centre.x + knobR * 0.55f, centre.y + knobR * 0.85f, true);
    g.setGradientFill (body);
    g.fillEllipse (knobArea);

    g.setColour (juce::Colours::white.withAlpha (0.10f * alpha));
    g.drawEllipse (knobArea.reduced (0.5f), 1.0f);

    const auto cap = knobArea.reduced (knobR * 0.30f);
    juce::ColourGradient capGrad (juce::Colour (0xff2c2f34).withMultipliedAlpha (alpha), cap.getX(), cap.getY(),
                                  juce::Colour (0xff111215).withMultipliedAlpha (alpha), cap.getRight(), cap.getBottom(), false);
    g.setGradientFill (capGrad);
    g.fillEllipse (cap);

    // 포인터
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    juce::Path pointer;
    const float pw = large ? 3.4f : 2.6f;
    pointer.addRoundedRectangle (-pw * 0.5f, -knobR * 0.94f, pw, knobR * 0.52f, pw * 0.5f);
    g.setColour (palette::text.withMultipliedAlpha (alpha));
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
}

void RZLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                      bool highlighted, bool /*down*/)
{
    auto r = button.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = button.getToggleState();

    if (button.getProperties()["rzStyle"].toString() == "segmented")
    {
        // A | B 세그먼트 스위치 (off = A, on = B)
        const float radius = r.getHeight() * 0.5f;
        g.setColour (palette::bgDark);
        g.fillRoundedRectangle (r, radius);
        g.setColour (palette::panelEdge.withAlpha (highlighted ? 1.0f : 0.7f));
        g.drawRoundedRectangle (r, radius, 1.0f);

        const auto left   = r.withWidth (r.getWidth() * 0.5f);
        const auto right  = r.withTrimmedLeft (r.getWidth() * 0.5f);
        const auto active = (on ? right : left).reduced (2.5f);
        g.setColour (palette::accent);
        g.fillRoundedRectangle (active, active.getHeight() * 0.5f);

        g.setFont (uiFont (11.5f, true, 0.08f));
        g.setColour (on ? palette::dim : palette::bgDark);
        g.drawText ("A  TILT", left, juce::Justification::centred, false);
        g.setColour (on ? palette::bgDark : palette::dim);
        g.drawText ("B  PRESENCE", right, juce::Justification::centred, false);
        return;
    }

    // LED 토글
    g.setColour (palette::bgDark);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (palette::panelEdge.withAlpha (highlighted ? 1.0f : 0.7f));
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);

    auto ledArea = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.32f);
    if (on)
    {
        g.setColour (palette::accent.withAlpha (0.35f));
        g.fillEllipse (ledArea.expanded (3.0f));
    }
    g.setColour (on ? palette::accent : juce::Colour (0xff3a434d));
    g.fillEllipse (ledArea);

    g.setFont (uiFont (11.5f, true, 0.08f));
    g.setColour (on ? palette::text : palette::dim);
    g.drawText (button.getButtonText(), r.withTrimmedRight (10.0f), juce::Justification::centredLeft, false);
}

//==============================================================================
//  ResponseView
//==============================================================================
ResponseView::ResponseView (PillowRZAudioProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

ResponseView::~ResponseView()
{
    stopTimer();
}

juce::Rectangle<float> ResponseView::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (38.0f).withTrimmedRight (14.0f)
                                     .withTrimmedTop (14.0f).withTrimmedBottom (24.0f);
}

float ResponseView::xForFreq (double f, juce::Rectangle<float> plot)
{
    const double t = std::log (f / 20.0) / std::log (1000.0);
    return plot.getX() + (float) t * plot.getWidth();
}

float ResponseView::yForDb (double db, juce::Rectangle<float> plot)
{
    const double t = (kTopDb - juce::jlimit (kBottomDb, kTopDb, db)) / (kTopDb - kBottomDb);
    return plot.getY() + (float) t * plot.getHeight();
}

void ResponseView::timerCallback()
{
    const auto p = proc.readParams();
    const double r = proc.getProcessingRate();
    if (! hasParams || p != params || ! juce::exactlyEqual (r, rate))
    {
        params = p;
        rate = r;
        hasParams = true;
        rebuildPath();
        repaint();
    }
}

void ResponseView::resized()
{
    rebuildPath();
}

void ResponseView::rebuildPath()
{
    curve.clear();
    fill.clear();
    showMarker = false;
    if (! hasParams || getWidth() <= 0)
        return;

    const auto plot = plotArea();
    const rz::ResponseModel model (params, rate);

    constexpr int numPoints = 256;
    for (int i = 0; i < numPoints; ++i)
    {
        const double f = 20.0 * std::pow (1000.0, (double) i / (double) (numPoints - 1));
        const float px = xForFreq (f, plot);
        const float py = yForDb (model.magnitudeDb (f), plot);
        if (i == 0) curve.startNewSubPath (px, py);
        else        curve.lineTo (px, py);
    }

    fill = curve;
    const float y0 = yForDb (0.0, plot);
    fill.lineTo (plot.getRight(), y0);
    fill.lineTo (plot.getX(), y0);
    fill.closeSubPath();

    // B 모드: 프레즌스 중심, A 모드: 틸트 회전축 표시
    const bool isB = params.mode == rz::Mode::B_Presence;
    const double mf = isB ? rz::kPresenceHz[(size_t) juce::jlimit (0, 3, params.presIndex)] : rz::kTiltPivotHz;
    marker = { xForFreq (mf, plot), yForDb (model.magnitudeDb (mf), plot) };
    showMarker = true;
}

void ResponseView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (palette::bgDark);
    g.fillRoundedRectangle (bounds, 10.0f);
    g.setColour (palette::panelEdge.withAlpha (0.6f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, 1.0f);

    const auto plot = plotArea();
    g.setFont (uiFont (10.5f));

    // 주파수 격자
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float gx = xForFreq (f, plot);
        g.setColour (palette::grid);
        g.drawVerticalLine (juce::roundToInt (gx), plot.getY(), plot.getBottom());
        g.setColour (palette::dim);
        const juce::String label = f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k"
                                               : juce::String (juce::roundToInt (f));
        g.drawText (label, juce::Rectangle<float> (gx - 18.0f, plot.getBottom() + 4.0f, 36.0f, 14.0f),
                    juce::Justification::centred, false);
    }

    // dB 격자
    for (int db = (int) kBottomDb; db <= (int) kTopDb; db += 6)
    {
        const float gy = yForDb ((double) db, plot);
        g.setColour (db == 0 ? palette::dim.withAlpha (0.5f) : palette::grid);
        g.drawHorizontalLine (juce::roundToInt (gy), plot.getX(), plot.getRight());
        g.setColour (palette::dim);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                    juce::Rectangle<float> (0.0f, gy - 7.0f, plot.getX() - 7.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }

    // 곡선
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (plot.toNearestInt().expanded (0, 2));

        g.setColour (palette::accent.withAlpha (0.14f));
        g.fillPath (fill);
        g.setColour (palette::accent);
        g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        if (showMarker)
        {
            g.setColour (palette::bgDark);
            g.fillEllipse (marker.x - 5.0f, marker.y - 5.0f, 10.0f, 10.0f);
            g.setColour (palette::accent);
            g.drawEllipse (marker.x - 4.0f, marker.y - 4.0f, 8.0f, 8.0f, 2.0f);
        }
    }

    // 범례
    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f, true, 0.1f));
    const bool isB = params.mode == rz::Mode::B_Presence;
    g.drawText (isB ? "MODULE B  -  PRESENCE" : "MODULE A  -  TILT",
                plot.withHeight (16.0f).translated (8.0f, 4.0f), juce::Justification::centredLeft, false);
}

//==============================================================================
//  Editor
//==============================================================================
PillowRZAudioProcessorEditor::PillowRZAudioProcessorEditor (PillowRZAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), response (p)
{
    addAndMakeVisible (response);

    setupKnob (treble,   pillow::ids::treble,   true);
    setupKnob (trebleQ,  pillow::ids::trebleQ,  false);
    setupKnob (bass,     pillow::ids::bass,     true);
    setupKnob (bassQ,    pillow::ids::bassQ,    false);
    setupKnob (tilt,     pillow::ids::tilt,     true);
    setupKnob (presGain, pillow::ids::presGain, true);
    setupKnob (presFreq, pillow::ids::presFreq, false);
    setupKnob (presQ,    pillow::ids::presQ,    false);
    setupKnob (tube,     pillow::ids::tube,     false);
    setupKnob (output,   pillow::ids::output,   false);

    // 원본 스텝(디텐트) 동작
    const std::function<bool()> stepped = [this] { return processorRef.isSteppedMode(); };

    treble.isStepped = stepped;
    treble.quantize  = [] (double v) { return (double) rz::quantizeTreble ((float) v); };
    treble.detents   = rz::trebleDetents();

    bass.isStepped = stepped;
    bass.quantize  = [] (double v) { return (double) rz::quantizeBass ((float) v); };
    bass.detents   = rz::bassDetents();

    tilt.isStepped = stepped;
    tilt.quantize  = [] (double v) { return (double) rz::quantizeTilt ((float) v); };
    tilt.detents   = rz::tiltDetents();

    presGain.isStepped = stepped;
    presGain.quantize  = [] (double v) { return (double) rz::quantizePresence ((float) v); };
    presGain.detents   = rz::presenceDetents();

    presFreq.isStepped = [] { return true; };
    presFreq.quantize  = [] (double v) { return std::round (v); };
    presFreq.detents   = { 0.0, 1.0, 2.0, 3.0 };

    // 텍스트 박스에는 "실제로 적용되는 값"(스텝 반영)을 표시
    for (auto* k : { &treble, &bass, &tilt, &presGain })
    {
        k->textFromValueFunction = [this, k] (double v)
        {
            const double shown = (processorRef.isSteppedMode() && k->quantize) ? k->quantize (v) : v;
            return pillow::formatDb ((float) shown);
        };
        k->updateText();
    }

    modeSwitch.getProperties().set ("rzStyle", "segmented");
    modeSwitch.setButtonText ("Mode");
    addAndMakeVisible (modeSwitch);
    modeAttachment = std::make_unique<ButtonAttachment> (processorRef.apvts, pillow::ids::mode, modeSwitch);

    stepsSwitch.setButtonText ("VINTAGE STEPS");
    addAndMakeVisible (stepsSwitch);
    stepsAttachment = std::make_unique<ButtonAttachment> (processorRef.apvts, pillow::ids::steps, stepsSwitch);

    lastModeB   = processorRef.isModeB();
    lastStepped = processorRef.isSteppedMode();
    updateModeVisibility();

    // 자식 컴포넌트를 다 만든 뒤에 지정해야 노브 값 표시(텍스트 박스)까지 새 색으로 다시 만들어짐
    setLookAndFeel (&lnf);

    setSize (kWidth, kHeight);
    startTimerHz (15);
}

PillowRZAudioProcessorEditor::~PillowRZAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void PillowRZAudioProcessorEditor::setupKnob (RZKnob& knob, const char* paramId, bool large)
{
    knob.large = large;
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, large ? 86 : 74, 18);
    addAndMakeVisible (knob);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (processorRef.apvts, paramId, knob));
}

void PillowRZAudioProcessorEditor::timerCallback()
{
    const bool modeB = processorRef.isModeB();
    if (modeB != lastModeB)
    {
        lastModeB = modeB;
        updateModeVisibility();
        repaint();
    }

    const bool st = processorRef.isSteppedMode();
    if (st != lastStepped)
    {
        lastStepped = st;
        for (auto* k : { &treble, &bass, &tilt, &presGain })
        {
            k->updateText();
            k->repaint();
        }
    }
}

void PillowRZAudioProcessorEditor::updateModeVisibility()
{
    tilt.setVisible (! lastModeB);
    presGain.setVisible (lastModeB);
    presFreq.setVisible (lastModeB);
    presQ.setVisible (lastModeB);
}

//==============================================================================
void PillowRZAudioProcessorEditor::resized()
{
    modeSwitch.setBounds (kWidth - 24 - 150 - 12 - 250, 14, 250, 28);
    stepsSwitch.setBounds (kWidth - 24 - 150, 14, 150, 28);

    response.setBounds (20, 68, 540, 300);

    footer = { 20, 380, 540, 148 };
    tube.setBounds   (footer.getX() + 18,  footer.getY() + 30, 92, 108);
    output.setBounds (footer.getX() + 128, footer.getY() + 30, 92, 108);

    modulePanel = { 576, 68, 304, 460 };
    rowTreble = { modulePanel.getX(), modulePanel.getY() + 6,   modulePanel.getWidth(), 148 };
    rowMid    = { modulePanel.getX(), modulePanel.getY() + 156, modulePanel.getWidth(), 148 };
    rowBass   = { modulePanel.getX(), modulePanel.getY() + 306, modulePanel.getWidth(), 148 };

    auto bigKnob = [] (juce::Rectangle<int> row) { return juce::Rectangle<int> (row.getX() + 12, row.getY() + 36, 124, 110); };
    treble.setBounds   (bigKnob (rowTreble));
    tilt.setBounds     (bigKnob (rowMid));
    presGain.setBounds (bigKnob (rowMid));
    bass.setBounds     (bigKnob (rowBass));

    trebleQ.setBounds  (rowTreble.getX() + 176, rowTreble.getY() + 46, 84, 96);
    bassQ.setBounds    (rowBass.getX()   + 176, rowBass.getY()   + 46, 84, 96);
    presFreq.setBounds (rowMid.getX() + 140, rowMid.getY() + 46, 78, 96);
    presQ.setBounds    (rowMid.getX() + 218, rowMid.getY() + 46, 78, 96);
}

void PillowRZAudioProcessorEditor::paint (juce::Graphics& g)
{
    // 배경
    g.setGradientFill (juce::ColourGradient (palette::bg, 0.0f, 0.0f,
                                             palette::bgDark, 0.0f, (float) getHeight(), false));
    g.fillAll();

    // 헤더
    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f, true, 0.3f));
    g.drawText ("PILLOW AUDIO", 24, 12, 220, 14, juce::Justification::centredLeft, false);
    g.setColour (palette::text);
    g.setFont (uiFont (24.0f, true));
    g.drawText ("RZ EQ", 22, 25, 110, 30, juce::Justification::centredLeft, false);
    g.setColour (palette::dim);
    g.setFont (uiFont (11.5f));
    g.drawText ("RZ062-style curves  /  variable Q", 108, 34, 300, 16, juce::Justification::centredLeft, false);

    // 하단 (튜브/출력)
    {
        const auto f = footer.toFloat();
        g.setColour (palette::bg.brighter (0.06f));
        g.fillRoundedRectangle (f, 10.0f);
        g.setColour (palette::panelEdge.withAlpha (0.5f));
        g.drawRoundedRectangle (f.reduced (0.5f), 10.0f, 1.0f);

        g.setColour (palette::text);
        g.setFont (uiFont (12.0f, true, 0.15f));
        g.drawText ("TUBE",   tube.getX(),   footer.getY() + 10, tube.getWidth(),   16, juce::Justification::centred, false);
        g.drawText ("OUTPUT", output.getX(), footer.getY() + 10, output.getWidth(), 16, juce::Justification::centred, false);

        g.setColour (palette::dim);
        g.setFont (uiFont (11.0f));
        const juce::String info =
            "Bass shelf: 40 Hz reference, +12 / -20 dB\n"
            "Treble shelf: 10 kHz reference, +12 / -18 dB\n"
            "A: tilt around 650 Hz, +/-4 dB\n"
            "B: presence 1.4 / 2 / 2.8 / 4 kHz, 0 - 4.8 dB\n"
            "Q 0.50 (shelves) / 1.00 (presence) = vintage\n"
            "Double-click a knob to reset";
        g.drawFittedText (info, footer.getX() + 240, footer.getY() + 14, footer.getWidth() - 256,
                          footer.getHeight() - 24, juce::Justification::topLeft, 6);
    }

    // 모듈 패널
    {
        const auto panel = modulePanel.toFloat();
        g.setColour (palette::panel);
        g.fillRoundedRectangle (panel, 12.0f);
        g.setColour (palette::panelEdge);
        g.drawRoundedRectangle (panel.reduced (0.5f), 12.0f, 1.0f);

        auto drawScrew = [&g] (juce::Point<float> c)
        {
            g.setColour (juce::Colour (0xff18202a));
            g.fillEllipse (c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f);
            g.setColour (juce::Colour (0xff70808f));
            g.drawLine (c.x - 3.0f, c.y - 1.2f, c.x + 3.0f, c.y + 1.2f, 1.3f);
        };
        drawScrew ({ panel.getX() + 12.0f,     panel.getY() + 12.0f });
        drawScrew ({ panel.getRight() - 12.0f, panel.getY() + 12.0f });
        drawScrew ({ panel.getX() + 12.0f,     panel.getBottom() - 12.0f });
        drawScrew ({ panel.getRight() - 12.0f, panel.getBottom() - 12.0f });

        g.setColour (palette::panelEdge.withAlpha (0.7f));
        g.drawHorizontalLine (rowMid.getY() - 2,  panel.getX() + 22.0f, panel.getRight() - 22.0f);
        g.drawHorizontalLine (rowBass.getY() - 2, panel.getX() + 22.0f, panel.getRight() - 22.0f);

        auto rowTitle = [&g] (juce::Rectangle<int> row, const juce::String& title, const juce::String& sub)
        {
            g.setColour (palette::text);
            g.setFont (uiFont (12.5f, true, 0.15f));
            g.drawText (title, row.getX() + 28, row.getY() + 8, 160, 16, juce::Justification::centredLeft, false);
            g.setColour (palette::dim);
            g.setFont (uiFont (10.5f));
            g.drawText (sub, row.getX() + 28, row.getY() + 23, 200, 14, juce::Justification::centredLeft, false);
        };

        auto smallLabel = [&g] (const juce::Component& c, const juce::String& text)
        {
            g.setColour (palette::text.withAlpha (0.85f));
            g.setFont (uiFont (11.0f, true, 0.12f));
            g.drawText (text, c.getX(), c.getY() - 14, c.getWidth(), 14, juce::Justification::centred, false);
        };

        rowTitle (rowTreble, "TREBLE", "shelf  /  10 kHz ref");
        smallLabel (trebleQ, "Q");

        if (lastModeB)
        {
            rowTitle (rowMid, "PRESENCE", "module B  /  boost");
            smallLabel (presFreq, "FREQ");
            smallLabel (presQ, "Q");
        }
        else
        {
            rowTitle (rowMid, "TILT", "module A  /  650 Hz pivot");
            g.setColour (palette::dim);
            g.setFont (uiFont (11.0f));
            g.drawFittedText ("left = darker\nright = brighter\nfixed 1st-order slope",
                              rowMid.getX() + 150, rowMid.getY() + 58, 140, 60,
                              juce::Justification::centredLeft, 3);
        }

        rowTitle (rowBass, "BASS", "shelf  /  40 Hz ref");
        smallLabel (bassQ, "Q");
    }
}
