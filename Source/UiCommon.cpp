#include "UiCommon.h"

namespace rz = pillow::rz;

namespace pillow::ui
{
//==============================================================================
juce::Font uiFont (float height, bool bold, float kerning)
{
    juce::Font f (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
    return kerning > 0.0f ? f.withExtraKerningFactor (kerning) : f;
}

juce::Colour bandColour (int index)
{
    static const juce::uint32 colours[] = {
        0xfff2a65a, 0xff6cc4bd, 0xffe8d36b, 0xffa98be0, 0xffe9797b, 0xff7fb3e8, 0xff9fd27a, 0xffe58fc8,
        0xffd9b48c, 0xff5fb9b0, 0xffc9c26a, 0xff8c9cf0, 0xfff0b98c, 0xff7fd1e0, 0xffc08ad8, 0xffb8d89a
    };
    return juce::Colour (colours[(size_t) juce::jlimit (0, 15, index)]);
}

static void drawScrew (juce::Graphics& g, juce::Point<float> c)
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
    g.setColour (palette::text.withAlpha (c.isEnabled() ? 0.85f : 0.35f));
    g.setFont (uiFont (11.0f, true, 0.12f));
    g.drawText (text, c.getX() - 10, c.getY() - 14, c.getWidth() + 20, 14, juce::Justification::centred, false);
}

void drawSectionTitle (juce::Graphics& g, int x, int y, const juce::String& title, const juce::String& sub)
{
    g.setColour (palette::text);
    g.setFont (uiFont (12.5f, true, 0.15f));
    g.drawText (title, x, y, 260, 16, juce::Justification::centredLeft, false);
    if (sub.isNotEmpty())
    {
        g.setColour (palette::dim);
        g.setFont (uiFont (10.5f));
        g.drawText (sub, x, y + 15, 260, 14, juce::Justification::centredLeft, false);
    }
}

void drawShapeIcon (juce::Graphics& g, rz::BandType type, juce::Rectangle<float> r, float thickness)
{
    // u: 0..1 (왼쪽 -> 오른쪽), 반환값 v: -1..1 (아래 -> 위)
    auto curve = [type] (float u) -> float
    {
        auto sig = [] (float x) { return 1.0f / (1.0f + std::exp (-x)); };
        switch (type)
        {
            case rz::BandType::Bell:      return -0.45f + 1.25f * std::exp (-std::pow ((u - 0.5f) / 0.16f, 2.0f));
            case rz::BandType::LowShelf:  return -0.45f + 1.1f * (1.0f - sig ((u - 0.5f) / 0.08f));
            case rz::BandType::HighShelf: return -0.45f + 1.1f * sig ((u - 0.5f) / 0.08f);
            case rz::BandType::LowCut:    return 0.35f - 1.3f * std::pow (std::max (0.0f, 1.0f - u / 0.5f), 1.6f);
            case rz::BandType::HighCut:   return 0.35f - 1.3f * std::pow (std::max (0.0f, (u - 0.5f) / 0.5f), 1.6f);
            case rz::BandType::Notch:     return 0.3f - 1.25f * std::exp (-std::pow ((u - 0.5f) / 0.05f, 2.0f));
            case rz::BandType::Tilt:      return -0.6f + 1.2f * u;
        }
        return 0.0f;
    };

    juce::Path p;
    constexpr int n = 48;
    for (int i = 0; i <= n; ++i)
    {
        const float u = (float) i / (float) n;
        const float x = r.getX() + u * r.getWidth();
        const float y = r.getCentreY() - curve (u) * r.getHeight() * 0.5f;
        if (i == 0) p.startNewSubPath (x, y);
        else        p.lineTo (x, y);
    }
    g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (type == rz::BandType::Tilt)
        g.fillEllipse (juce::Rectangle<float> (thickness * 2.6f, thickness * 2.6f).withCentre (r.getCentre()));
}

//==============================================================================
juce::RangedAudioParameter* bandParam (PillowRZAudioProcessor& proc, int band, const char* field)
{
    return proc.apvts.getParameter (pillow::ids::band (band, field));
}

void setParameter (juce::RangedAudioParameter* p, float value)
{
    if (p == nullptr)
        return;
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (value));
    p->endChangeGesture();
}

int addBand (PillowRZAudioProcessor& proc, rz::BandType type, float freq, float gain, rz::Placement place)
{
    for (int i = 0; i < rz::kMaxBands; ++i)
    {
        auto* used = bandParam (proc, i, pillow::ids::used);
        if (used == nullptr || used->getValue() >= 0.5f)
            continue;

        setParameter (bandParam (proc, i, pillow::ids::on),    1.0f);
        setParameter (bandParam (proc, i, pillow::ids::type),  (float) (int) type);
        setParameter (bandParam (proc, i, pillow::ids::freq),  juce::jlimit (rz::kFreqMin, rz::kFreqMax, freq));
        setParameter (bandParam (proc, i, pillow::ids::gain),  rz::typeHasGain (type) ? juce::jlimit (-rz::kGainMax, rz::kGainMax, gain) : 0.0f);
        setParameter (bandParam (proc, i, pillow::ids::q),     1.0f);
        setParameter (bandParam (proc, i, pillow::ids::slope), (float) rz::kDefaultSlope);
        setParameter (bandParam (proc, i, pillow::ids::place), (float) (int) place);
        setParameter (bandParam (proc, i, pillow::ids::tube),  0.0f);   // 예전에 쓰던 칸의 설정이 남지 않게
        setParameter (bandParam (proc, i, pillow::ids::xfmr),  0.0f);
        setParameter (used, 1.0f);
        return i;
    }
    return -1;
}

void removeBand (PillowRZAudioProcessor& proc, int band)
{
    if (band >= 0 && band < rz::kMaxBands)
        setParameter (bandParam (proc, band, pillow::ids::used), 0.0f);
}

//==============================================================================
//  RZKnob
//==============================================================================
RZKnob::RZKnob()
    : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow)
{
    setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                         juce::MathConstants<float>::pi * 2.75f, true);
    setMouseDragSensitivity (240);
    setMouseClickGrabsKeyboardFocus (false);   // 돌려도 그래프의 키보드 포커스(Delete 등)를 뺏지 않음
}

void RZKnob::mouseDown (const juce::MouseEvent& e)
{
    lastDragPos = virtualDragPos = e.position;
    juce::Slider::mouseDown (e);
}

void RZKnob::mouseDrag (const juce::MouseEvent& e)
{
    // Shift: 10배 미세 조정. 움직인 양을 누적한 "가상 위치"로 넘기므로 드래그 도중 Shift를 눌러도 값이 튀지 않음
    const float k = e.mods.isShiftDown() ? 0.1f : 1.0f;
    virtualDragPos += (e.position - lastDragPos) * k;
    lastDragPos = e.position;
    juce::Slider::mouseDrag (e.withNewPosition (virtualDragPos));
}

void RZKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! e.mods.isShiftDown())
    {
        juce::Slider::mouseWheelMove (e, wheel);
        return;
    }
    auto fine = wheel;
    fine.deltaX *= 0.1f;
    fine.deltaY *= 0.1f;
    juce::Slider::mouseWheelMove (e, fine);
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
    setColour (juce::TextButton::textColourOffId,       palette::text);
    setColour (juce::TextButton::textColourOnId,        palette::text);
    setColour (juce::PopupMenu::backgroundColourId,            palette::bgDark);
    setColour (juce::PopupMenu::textColourId,                  palette::text);
    setColour (juce::PopupMenu::headerTextColourId,            palette::dim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, palette::accent.withAlpha (0.85f));
    setColour (juce::PopupMenu::highlightedTextColourId,       palette::bgDark);
}

juce::Font RZLookAndFeel::getLabelFont (juce::Label&)          { return uiFont (12.5f); }
juce::Font RZLookAndFeel::getPopupMenuFont()                   { return uiFont (14.0f); }
juce::Font RZLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return uiFont (11.0f, true, 0.08f); }

void RZLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPos, float startAngle, float endAngle,
                                      juce::Slider& slider)
{
    auto* knob = dynamic_cast<RZKnob*> (&slider);

    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (2.0f);
    const float size   = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto  area   = bounds.withSizeKeepingCentre (size, size);
    const auto  centre = area.getCentre();
    const float radius = size * 0.5f;
    const bool  active = slider.isEnabled() && ! (knob != nullptr && knob->dimmed);
    const float alpha  = active ? 1.0f : 0.38f;
    const bool  large  = knob != nullptr && knob->large;

    auto tick = [&] (float proportion, float len, float thick)
    {
        const float a  = startAngle + proportion * (endAngle - startAngle);
        const auto  p1 = centre.getPointOnCircumference (radius - 1.0f, a);
        const auto  p2 = centre.getPointOnCircumference (radius - 1.0f - len, a);
        g.drawLine (juce::Line<float> (p1, p2), thick);
    };

    g.setColour (palette::scale.withMultipliedAlpha (0.75f * alpha));
    if (knob != nullptr && ! knob->detents.empty())
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
    const auto ledColour = button.getProperties().contains ("rzLed")
                               ? juce::Colour ((juce::uint32) (juce::int64) button.getProperties()["rzLed"])
                               : palette::accent;

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
        g.setColour (ledColour.withAlpha (0.35f * alpha));
        g.fillEllipse (ledArea.expanded (3.0f));
    }
    g.setColour ((on ? ledColour : juce::Colour (0xff3a434d)).withMultipliedAlpha (alpha));
    g.fillEllipse (ledArea);

    g.setFont (uiFont (11.0f, true, 0.08f));
    g.setColour ((on ? palette::text : palette::dim).withMultipliedAlpha (alpha));
    g.drawText (button.getButtonText(), r.withTrimmedRight (8.0f), juce::Justification::centredLeft, false);
}

void RZLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                          bool highlighted, bool down)
{
    const auto r = button.getLocalBounds().toFloat().reduced (1.0f);
    const float alpha = button.isEnabled() ? 1.0f : 0.4f;
    g.setColour ((down ? palette::panelEdge : palette::bgDark).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (palette::panelEdge.withAlpha ((highlighted ? 1.0f : 0.7f) * alpha));
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
}

//==============================================================================
//  SegmentSelector
//==============================================================================
SegmentSelector::SegmentSelector (Style s, juce::StringArray l)
    : style (s), labels (std::move (l)), enabled ((size_t) labels.size(), true)
{
    setMouseClickGrabsKeyboardFocus (false);
}

void SegmentSelector::setIndex (int i)
{
    i = juce::jlimit (0, juce::jmax (0, labels.size() - 1), i);
    if (i != index)
    {
        index = i;
        repaint();
    }
}

void SegmentSelector::setItemEnabled (int item, bool e)
{
    if (item >= 0 && item < (int) enabled.size() && enabled[(size_t) item] != e)
    {
        enabled[(size_t) item] = e;
        repaint();
    }
}

juce::Rectangle<float> SegmentSelector::itemBounds (int i) const noexcept
{
    const auto r = getLocalBounds().toFloat();
    const float w = r.getWidth() / (float) juce::jmax (1, labels.size());
    return { r.getX() + w * (float) i, r.getY(), w, r.getHeight() };
}

int SegmentSelector::itemAt (juce::Point<float> p) const noexcept
{
    for (int i = 0; i < labels.size(); ++i)
        if (itemBounds (i).contains (p))
            return i;
    return -1;
}

void SegmentSelector::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float alpha = isEnabled() ? 1.0f : 0.4f;
    const float radius = juce::jmin (8.0f, r.getHeight() * 0.5f);

    g.setColour (palette::bgDark.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, radius);
    g.setColour (palette::panelEdge.withMultipliedAlpha (0.8f * alpha));
    g.drawRoundedRectangle (r, radius, 1.0f);

    for (int i = 0; i < labels.size(); ++i)
    {
        const auto ib = itemBounds (i);
        const bool sel = i == index;
        const bool en  = enabled[(size_t) i];
        const float a  = alpha * (en ? 1.0f : 0.3f);

        if (i > 0 && ! sel && i - 1 != index)
        {
            g.setColour (palette::panelEdge.withMultipliedAlpha (0.5f * alpha));
            g.drawVerticalLine (juce::roundToInt (ib.getX()), ib.getY() + 6.0f, ib.getBottom() - 6.0f);
        }

        if (sel)
        {
            g.setColour (accent.withMultipliedAlpha (a));
            g.fillRoundedRectangle (ib.reduced (2.5f), juce::jmin (6.0f, ib.getHeight() * 0.5f - 2.5f));
        }
        else if (i == hover && en)
        {
            g.setColour (palette::panel.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (ib.reduced (2.5f), juce::jmin (6.0f, ib.getHeight() * 0.5f - 2.5f));
        }

        const auto fg = (sel ? palette::bgDark : (i == hover ? palette::text : palette::dim)).withMultipliedAlpha (a);
        g.setColour (fg);
        if (style == Style::ShapeIcons)
        {
            const auto iconArea = ib.reduced (ib.getWidth() * 0.2f, ib.getHeight() * 0.28f);
            drawShapeIcon (g, (rz::BandType) i, iconArea, 1.8f);
        }
        else
        {
            g.setFont (uiFont (10.5f, true, 0.06f));
            g.drawText (labels[i], ib, juce::Justification::centred, false);
        }
    }
}

void SegmentSelector::mouseDown (const juce::MouseEvent& e)
{
    const int i = itemAt (e.position);
    if (i < 0 || ! enabled[(size_t) i])
        return;
    setIndex (i);
    if (onChange)
        onChange (i);
}

void SegmentSelector::mouseMove (const juce::MouseEvent& e)
{
    const int h = itemAt (e.position);
    if (h != hover)
    {
        hover = h;
        repaint();
    }
    if (h >= 0 && h < labels.size())
        setTooltip (labels[h]);
}

void SegmentSelector::mouseExit (const juce::MouseEvent&)
{
    hover = -1;
    repaint();
}
} // namespace pillow::ui
