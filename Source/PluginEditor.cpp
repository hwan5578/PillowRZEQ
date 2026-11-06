#include "PluginEditor.h"

namespace rz = pillow::rz;
using namespace pillow::ui;

namespace
{
    constexpr int kWidth  = 1080;
    constexpr int kHeight = 700;

    juce::Font uiFont (float height, bool bold = false, float kerning = 0.0f)
    {
        juce::Font f (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
        return kerning > 0.0f ? f.withExtraKerningFactor (kerning) : f;
    }

    void drawScrew (juce::Graphics& g, juce::Point<float> c)
    {
        g.setColour (juce::Colour (0xff18202a));
        g.fillEllipse (c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f);
        g.setColour (juce::Colour (0xff70808f));
        g.drawLine (c.x - 3.0f, c.y - 1.2f, c.x + 3.0f, c.y + 1.2f, 1.3f);
    }

    void drawPanelBackground (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (palette::panel);
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (palette::panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
        drawScrew (g, { r.getX() + 12.0f,     r.getY() + 12.0f });
        drawScrew (g, { r.getRight() - 12.0f, r.getY() + 12.0f });
        drawScrew (g, { r.getX() + 12.0f,     r.getBottom() - 12.0f });
        drawScrew (g, { r.getRight() - 12.0f, r.getBottom() - 12.0f });
    }

    void drawSmallLabel (juce::Graphics& g, const juce::Component& c, const juce::String& text)
    {
        g.setColour (palette::text.withAlpha (0.85f));
        g.setFont (uiFont (11.0f, true, 0.12f));
        g.drawText (text, c.getX() - 10, c.getY() - 14, c.getWidth() + 20, 14, juce::Justification::centred, false);
    }

    void drawRowTitle (juce::Graphics& g, juce::Rectangle<int> row, const juce::String& title, const juce::String& sub)
    {
        g.setColour (palette::text);
        g.setFont (uiFont (12.5f, true, 0.15f));
        g.drawText (title, row.getX() + 24, row.getY() + 6, 200, 16, juce::Justification::centredLeft, false);
        g.setColour (palette::dim);
        g.setFont (uiFont (10.5f));
        g.drawText (sub, row.getX() + 24, row.getY() + 21, 220, 14, juce::Justification::centredLeft, false);
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
    const bool  active = slider.isEnabled() && ! (knob != nullptr && knob->dimmed);
    const float alpha  = active ? 1.0f : 0.38f;
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
    if (stepped && ! knob->detents.empty())
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
    const float alpha = button.isEnabled() ? 1.0f : 0.4f;

    if (button.getProperties()["rzStyle"].toString() == "segmented")
    {
        // 두 칸 세그먼트 스위치 (off = 왼쪽, on = 오른쪽)
        const float radius = r.getHeight() * 0.5f;
        g.setColour (palette::bgDark.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r, radius);
        g.setColour (palette::panelEdge.withAlpha ((highlighted ? 1.0f : 0.7f) * alpha));
        g.drawRoundedRectangle (r, radius, 1.0f);

        const auto left   = r.withWidth (r.getWidth() * 0.5f);
        const auto right  = r.withTrimmedLeft (r.getWidth() * 0.5f);
        const auto active = (on ? right : left).reduced (2.5f);
        g.setColour (palette::accent.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (active, active.getHeight() * 0.5f);

        const auto leftText  = button.getProperties().getWithDefault ("rzLeft",  "A").toString();
        const auto rightText = button.getProperties().getWithDefault ("rzRight", "B").toString();
        g.setFont (uiFont (10.5f, true, 0.06f));
        g.setColour ((on ? palette::dim : palette::bgDark).withMultipliedAlpha (alpha));
        g.drawText (leftText, left, juce::Justification::centred, false);
        g.setColour ((on ? palette::bgDark : palette::dim).withMultipliedAlpha (alpha));
        g.drawText (rightText, right, juce::Justification::centred, false);
        return;
    }

    // LED 토글
    g.setColour (palette::bgDark.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (palette::panelEdge.withAlpha ((highlighted ? 1.0f : 0.7f) * alpha));
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);

    auto ledArea = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.32f);
    if (on)
    {
        g.setColour (palette::accent.withAlpha (0.35f * alpha));
        g.fillEllipse (ledArea.expanded (3.0f));
    }
    g.setColour ((on ? palette::accent : juce::Colour (0xff3a434d)).withMultipliedAlpha (alpha));
    g.fillEllipse (ledArea);

    g.setFont (uiFont (11.0f, true, 0.08f));
    g.setColour ((on ? palette::text : palette::dim).withMultipliedAlpha (alpha));
    g.drawText (button.getButtonText(), r.withTrimmedRight (8.0f), juce::Justification::centredLeft, false);
}

//==============================================================================
//  ModulePanel
//==============================================================================
ModulePanel::ModulePanel (PillowRZAudioProcessor& p, int moduleIndex)
    : proc (p), index (moduleIndex)
{
    setupKnob (treble,   pillow::ids::treble,   true);
    setupKnob (trebleQ,  pillow::ids::trebleQ,  false);
    setupKnob (bass,     pillow::ids::bass,     true);
    setupKnob (bassQ,    pillow::ids::bassQ,    false);
    setupKnob (tilt,     pillow::ids::tilt,     true);
    setupKnob (presGain, pillow::ids::presGain, true);
    setupKnob (presFreq, pillow::ids::presFreq, false);
    setupKnob (presQ,    pillow::ids::presQ,    false);
    setupKnob (drive,    pillow::ids::drive,    false);
    setupKnob (level,    pillow::ids::output,   false);

    // 원본 스텝(디텐트) 동작
    const std::function<bool()> stepped = [this] { return proc.isSteppedMode(); };

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
            const double shown = (proc.isSteppedMode() && k->quantize) ? k->quantize (v) : v;
            return pillow::formatDb ((float) shown);
        };
        k->updateText();
    }

    typeSwitch.getProperties().set ("rzStyle", "segmented");
    typeSwitch.getProperties().set ("rzLeft",  "A PIVOT");
    typeSwitch.getProperties().set ("rzRight", "B PRESENCE");
    typeSwitch.setButtonText ("Type");
    addAndMakeVisible (typeSwitch);
    typeAttachment = std::make_unique<ButtonAttachment> (proc.apvts, pillow::ids::forModule (pillow::ids::mode, index), typeSwitch);
}

void ModulePanel::setupKnob (RZKnob& knob, const char* baseId, bool large)
{
    knob.large = large;
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, large ? 86 : 74, 18);
    addAndMakeVisible (knob);
    attachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, pillow::ids::forModule (baseId, index), knob));
}

juce::String ModulePanel::title() const
{
    if (index == 0)
    {
        if (state.mono)    return "1  /  MONO";
        if (state.midSide) return state.linked ? "1  /  M + S" : "1  /  MID";
        return state.linked ? "1  /  L + R" : "1  /  LEFT";
    }
    return state.midSide ? "2  /  SIDE" : "2  /  RIGHT";
}

void ModulePanel::setState (const State& s)
{
    if (hasState && ! (s != state))
        return;

    const bool steppedChanged = ! hasState || s.stepped != state.stepped;
    state = s;
    hasState = true;

    tilt.setVisible (! state.modeB);
    presGain.setVisible (state.modeB);
    presFreq.setVisible (state.modeB);
    presQ.setVisible (state.modeB);

    for (auto* k : { &treble, &trebleQ, &bass, &bassQ, &tilt, &presGain, &presFreq, &presQ })
        k->dimmed = ! state.eqOn;
    drive.dimmed = ! state.driveOn;

    setEnabled (! isFollower());

    if (steppedChanged)
        for (auto* k : { &treble, &bass, &tilt, &presGain })
            k->updateText();

    repaint();
}

void ModulePanel::resized()
{
    const int w = getWidth();
    rowTreble = { 0, 46,  w, 146 };
    rowMid    = { 0, 192, w, 146 };
    rowBass   = { 0, 338, w, 146 };
    rowTube   = { 0, 484, w, getHeight() - 484 };

    typeSwitch.setBounds (w - 14 - 166, 12, 166, 26);

    auto bigKnob = [] (juce::Rectangle<int> row) { return juce::Rectangle<int> (row.getX() + 14, row.getY() + 34, 120, 110); };
    treble.setBounds   (bigKnob (rowTreble));
    tilt.setBounds     (bigKnob (rowMid));
    presGain.setBounds (bigKnob (rowMid));
    bass.setBounds     (bigKnob (rowBass));

    trebleQ.setBounds  (rowTreble.getX() + 172, rowTreble.getY() + 46, 82, 94);
    bassQ.setBounds    (rowBass.getX()   + 172, rowBass.getY()   + 46, 82, 94);
    presFreq.setBounds (rowMid.getX() + 140, rowMid.getY() + 46, 74, 94);
    presQ.setBounds    (rowMid.getX() + 214, rowMid.getY() + 46, 74, 94);

    drive.setBounds (rowTube.getX() + 34,  rowTube.getY() + 40, 96, 100);
    level.setBounds (rowTube.getX() + 166, rowTube.getY() + 40, 96, 100);
}

void ModulePanel::paint (juce::Graphics& g)
{
    drawPanelBackground (g, getLocalBounds().toFloat());

    g.setColour (palette::text);
    g.setFont (uiFont (12.0f, true, 0.15f));
    g.drawFittedText (title(), 26, 14, typeSwitch.getX() - 32, 22, juce::Justification::centredLeft, 1, 0.8f);

    g.setColour (palette::panelEdge.withAlpha (0.7f));
    for (auto row : { rowTreble, rowMid, rowBass, rowTube })
        g.drawHorizontalLine (row.getY() - 1, 20.0f, (float) getWidth() - 20.0f);

    drawRowTitle (g, rowTreble, "TREBLE", "shelf  /  10 kHz ref");
    drawSmallLabel (g, trebleQ, "Q");

    if (state.modeB)
    {
        drawRowTitle (g, rowMid, "PRESENCE", "module B  /  boost");
        drawSmallLabel (g, presFreq, "FREQ");
        drawSmallLabel (g, presQ, "Q");
    }
    else
    {
        drawRowTitle (g, rowMid, "PIVOT", "module A  /  650 Hz tilt");
        g.setColour (palette::dim);
        g.setFont (uiFont (11.0f));
        g.drawFittedText ("left = darker\nright = brighter\nfixed 1st-order slope",
                          rowMid.getX() + 148, rowMid.getY() + 56, 136, 60, juce::Justification::centredLeft, 3);
    }

    drawRowTitle (g, rowBass, "BASS", "shelf  /  40 Hz ref");

    drawRowTitle (g, rowTube, "TUBE  /  EF804S", {});
    drawSmallLabel (g, drive, "DRIVE");
    drawSmallLabel (g, level, "LEVEL");
}

void ModulePanel::paintOverChildren (juce::Graphics& g)
{
    if (! isFollower())
        return;

    const auto r = getLocalBounds().toFloat().reduced (6.0f);
    g.setColour (palette::bgDark.withAlpha (0.55f));
    g.fillRoundedRectangle (r, 10.0f);

    g.setColour (palette::text);
    g.setFont (uiFont (16.0f, true, 0.3f));
    g.drawText (state.mono ? "MONO" : "LINKED", r.withHeight (r.getHeight() * 0.5f).translated (0.0f, r.getHeight() * 0.25f - 14.0f),
                juce::Justification::centred, false);
    g.setColour (palette::dim);
    g.setFont (uiFont (11.5f));
    g.drawText (state.mono ? "mono track: module 1 only" : "follows module 1  -  turn LINK off to edit",
                r.withHeight (r.getHeight() * 0.5f).translated (0.0f, r.getHeight() * 0.25f + 12.0f),
                juce::Justification::centred, false);
}

//==============================================================================
//  GlobalPanel
//==============================================================================
GlobalPanel::GlobalPanel (PillowRZAudioProcessor& p) : proc (p)
{
    setupKnob (focus,  pillow::ids::focus);
    setupKnob (mix,    pillow::ids::mix);
    setupKnob (xfLow,  pillow::ids::xfLow);
    setupKnob (xfMid,  pillow::ids::xfMid);
    setupKnob (xfHigh, pillow::ids::xfHigh);

    eqSwitch.setButtonText ("EQ");
    addAndMakeVisible (eqSwitch);
    eqAttachment = std::make_unique<ButtonAttachment> (proc.apvts, pillow::ids::eqOn, eqSwitch);

    driveSwitch.setButtonText ("DRIVE");
    addAndMakeVisible (driveSwitch);
    driveAttachment = std::make_unique<ButtonAttachment> (proc.apvts, pillow::ids::driveOn, driveSwitch);
}

void GlobalPanel::setupKnob (RZKnob& knob, const char* paramId)
{
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 92, 18);
    addAndMakeVisible (knob);
    attachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, paramId, knob));
}

void GlobalPanel::setDriveOn (bool on)
{
    if (on == driveOn)
        return;
    driveOn = on;
    focus.dimmed = ! on;
    repaint();
}

void GlobalPanel::resized()
{
    const int w = getWidth();
    driveSwitch.setBounds (w - 20 - 92, 12, 92, 26);
    eqSwitch.setBounds    (w - 20 - 92 - 8 - 70, 12, 70, 26);

    focus.setBounds (22,  62, 100, 104);
    mix.setBounds   (140, 62, 100, 104);

    xfLow.setBounds  (22,  224, 100, 104);
    xfMid.setBounds  (156, 224, 100, 104);
    xfHigh.setBounds (290, 224, 100, 104);
}

void GlobalPanel::paint (juce::Graphics& g)
{
    drawPanelBackground (g, getLocalBounds().toFloat());

    g.setColour (palette::text);
    g.setFont (uiFont (12.5f, true, 0.15f));
    g.drawText ("TUBE", 26, 12, 120, 16, juce::Justification::centredLeft, false);
    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f));
    g.drawText ("EF804S  /  shared by both modules", 26, 27, 220, 14, juce::Justification::centredLeft, false);

    drawSmallLabel (g, focus, "FOCUS");
    drawSmallLabel (g, mix, "MIX");

    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f));
    g.drawText ("ODD", focus.getX() + 6, focus.getBottom() - 34, 30, 12, juce::Justification::centredLeft, false);
    g.drawText ("EVEN", focus.getRight() - 36, focus.getBottom() - 34, 30, 12, juce::Justification::centredRight, false);

    g.setFont (uiFont (11.0f));
    g.drawFittedText ("Drive per module (bottom row).\n"
                      "Focus: odd <-> even harmonics.\n"
                      "Mix: blend the dry signal back\n(parallel EQ + tube).",
                      258, 64, getWidth() - 274, 90, juce::Justification::topLeft, 5);

    g.setColour (palette::panelEdge.withAlpha (0.7f));
    g.drawHorizontalLine (176, 20.0f, (float) getWidth() - 20.0f);

    g.setColour (palette::text);
    g.setFont (uiFont (12.5f, true, 0.15f));
    g.drawText ("OUTPUT TRANSFORMER", 26, 184, 220, 16, juce::Justification::centredLeft, false);

    drawSmallLabel (g, xfLow,  "LOW  < 200 Hz");
    drawSmallLabel (g, xfMid,  "MID");
    drawSmallLabel (g, xfHigh, "HIGH  > 4 kHz");
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
                                     .withTrimmedTop (30.0f).withTrimmedBottom (24.0f);
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

int ResponseView::numCurves() const noexcept
{
    return (params.link || channels < 2) ? 1 : 2;
}

void ResponseView::timerCallback()
{
    const auto p = proc.readParams();
    const double r = proc.getProcessingRate();
    const int ch = proc.getNumChannelsInUse();
    if (! hasParams || p != params || ! juce::exactlyEqual (r, rate) || ch != channels)
    {
        params = p;
        rate = r;
        channels = ch;
        hasParams = true;
        rebuildPaths();
        repaint();
    }
}

void ResponseView::resized()
{
    rebuildPaths();
}

void ResponseView::rebuildPaths()
{
    for (auto& c : curves) c.clear();
    for (auto& f : fills)  f.clear();
    if (! hasParams || getWidth() <= 0)
        return;

    const auto plot = plotArea();
    const float y0 = yForDb (0.0, plot);

    for (int m = 0; m < numCurves(); ++m)
    {
        const rz::ResponseModel model (params, m, rate);
        auto& curve = curves[(size_t) m];

        constexpr int numPoints = 256;
        for (int i = 0; i < numPoints; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, (double) i / (double) (numPoints - 1));
            const float px = xForFreq (f, plot);
            const float py = yForDb (model.magnitudeDb (f), plot);
            if (i == 0) curve.startNewSubPath (px, py);
            else        curve.lineTo (px, py);
        }

        auto& fill = fills[(size_t) m];
        fill = curve;
        fill.lineTo (plot.getRight(), y0);
        fill.lineTo (plot.getX(), y0);
        fill.closeSubPath();

        // B 모드: 프레즌스 중심, A 모드: Pivot 회전축 표시
        const auto& mp = params.effective (m);
        const bool isB = mp.mode == rz::Mode::B_Presence;
        const double mf = isB ? rz::kPresenceHz[(size_t) juce::jlimit (0, 3, mp.presIndex)] : rz::kTiltPivotHz;
        markers[(size_t) m] = { xForFreq (mf, plot), yForDb (model.magnitudeDb (mf), plot) };
    }
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

    const int n = numCurves();
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (plot.toNearestInt().expanded (0, 2));

        for (int m = n - 1; m >= 0; --m)   // 모듈 1을 맨 위에
        {
            const auto colour = (m == 0) ? palette::accent : palette::accent2;
            g.setColour (colour.withAlpha (m == 0 ? 0.14f : 0.08f));
            g.fillPath (fills[(size_t) m]);
            g.setColour (colour);
            g.strokePath (curves[(size_t) m], juce::PathStrokeType (2.2f, juce::PathStrokeType::curved,
                                                                     juce::PathStrokeType::rounded));
            const auto mk = markers[(size_t) m];
            g.setColour (palette::bgDark);
            g.fillEllipse (mk.x - 5.0f, mk.y - 5.0f, 10.0f, 10.0f);
            g.setColour (colour);
            g.drawEllipse (mk.x - 4.0f, mk.y - 4.0f, 8.0f, 8.0f, 2.0f);
        }
    }

    // 범례
    const bool ms = params.routing == rz::Routing::MidSide;
    juce::String names[2];
    if (channels < 2)      names[0] = "MONO";
    else if (params.link)  names[0] = ms ? "MID + SIDE" : "LEFT + RIGHT";
    else                 { names[0] = ms ? "MID" : "LEFT"; names[1] = ms ? "SIDE" : "RIGHT"; }

    float lx = plot.getX() + 4.0f;
    g.setFont (uiFont (10.5f, true, 0.1f));
    for (int m = 0; m < n; ++m)
    {
        g.setColour (m == 0 ? palette::accent : palette::accent2);
        g.fillRoundedRectangle (lx, 11.0f, 14.0f, 4.0f, 2.0f);
        g.setColour (palette::dim);
        g.drawText (names[m], juce::Rectangle<float> (lx + 20.0f, 6.0f, 110.0f, 14.0f), juce::Justification::centredLeft, false);
        lx += 120.0f;
    }
    if (! params.eqOn)
    {
        g.setColour (palette::accent);
        g.drawText ("EQ OFF", juce::Rectangle<float> (plot.getRight() - 90.0f, 6.0f, 90.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }
}

//==============================================================================
//  Editor
//==============================================================================
PillowRZAudioProcessorEditor::PillowRZAudioProcessorEditor (PillowRZAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), response (p),
      module1 (p, 0), module2 (p, 1), global (p)
{
    addAndMakeVisible (response);
    addAndMakeVisible (module1);
    addAndMakeVisible (module2);
    addAndMakeVisible (global);

    routingSwitch.getProperties().set ("rzStyle", "segmented");
    routingSwitch.getProperties().set ("rzLeft",  "STEREO  L / R");
    routingSwitch.getProperties().set ("rzRight", "MID / SIDE");
    routingSwitch.setButtonText ("Routing");
    addAndMakeVisible (routingSwitch);
    routingAttachment = std::make_unique<pillow::ui::ButtonAttachment> (processorRef.apvts, pillow::ids::routing, routingSwitch);

    linkSwitch.setButtonText ("LINK");
    addAndMakeVisible (linkSwitch);
    linkAttachment = std::make_unique<pillow::ui::ButtonAttachment> (processorRef.apvts, pillow::ids::link, linkSwitch);

    stepsSwitch.setButtonText ("VINTAGE STEPS");
    addAndMakeVisible (stepsSwitch);
    stepsAttachment = std::make_unique<pillow::ui::ButtonAttachment> (processorRef.apvts, pillow::ids::steps, stepsSwitch);

    refreshState();

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

void PillowRZAudioProcessorEditor::refreshState()
{
    const auto p = processorRef.readParams();
    const bool mono = processorRef.getNumChannelsInUse() < 2;

    for (int m = 0; m < 2; ++m)
    {
        pillow::ui::ModulePanel::State s;
        s.modeB   = p.mod[(size_t) m].mode == rz::Mode::B_Presence;
        s.linked  = p.link;
        s.mono    = mono;
        s.midSide = p.routing == rz::Routing::MidSide;
        s.stepped = p.stepped;
        s.eqOn    = p.eqOn;
        s.driveOn = p.driveOn;
        (m == 0 ? module1 : module2).setState (s);
    }

    global.setDriveOn (p.driveOn);
    routingSwitch.setEnabled (! mono);
    linkSwitch.setEnabled (! mono);
}

void PillowRZAudioProcessorEditor::timerCallback()
{
    refreshState();
}

void PillowRZAudioProcessorEditor::resized()
{
    stepsSwitch.setBounds   (kWidth - 16 - 150, 14, 150, 28);
    linkSwitch.setBounds    (kWidth - 16 - 150 - 10 - 88, 14, 88, 28);
    routingSwitch.setBounds (kWidth - 16 - 150 - 10 - 88 - 10 - 236, 14, 236, 28);

    module1.setBounds  (16,  60, 296, 628);
    module2.setBounds  (324, 60, 296, 628);
    response.setBounds (636, 60, 428, 280);
    global.setBounds   (636, 352, 428, 336);
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
    g.drawText ("RZ062-style curves  /  variable Q  /  EF804S", 106, 32, 330, 16, juce::Justification::centredLeft, false);
}
