#include "Panels.h"

namespace rz = pillow::rz;

namespace pillow::ui
{
//==============================================================================
//  BandPanel
//==============================================================================
namespace
{
    const juce::Colour kDynColour { 0xffffd166 };     // 다이내믹 · 듣기 버튼의 켜짐 표시
}

BandPanel::BandPanel (PillowRZAudioProcessor& p) : proc (p)
{
    for (auto* k : { &freqKnob, &gainKnob, &qKnob })
    {
        k->large = true;
        k->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 88, 18);
        addChildComponent (*k);
    }
    for (auto* k : { &driveKnob, &focusKnob, &mixKnob, &xfAmtKnob })
    {
        k->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 16);
        addChildComponent (*k);
    }
    for (auto* k : { &rangeKnob, &threshKnob, &attackKnob, &releaseKnob, &densityKnob })
    {
        k->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 16);
        addChildComponent (*k);
    }
    for (auto* k : allKnobs())
    {
        // 어태치먼트가 제스처를 닫은 "뒤"에 불림 -> 미뤄 둔 다시 연결을 안전하게 할 수 있음
        k->onDragStart = [this, k] { draggingKnob = k; };
        k->onDragEnd   = [this, k] { if (draggingKnob == k) draggingKnob = nullptr; };
    }
    freqKnob.setMouseDragSensitivity (360);   // 10 Hz ~ 30 kHz (11.5 옥타브)
    qKnob.setMouseDragSensitivity (300);

    slopeKnob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 96, 18);
    for (int i = 0; i < (int) rz::kCutSlopes.size(); ++i)
        slopeKnob.detents.push_back ((double) i);
    addChildComponent (slopeKnob);

    shapes.onChange    = [this] (int i) { if (shapeAtt != nullptr) shapeAtt->setValueAsCompleteGesture ((float) i); };
    placement.onChange = [this] (int i) { if (placeAtt != nullptr) placeAtt->setValueAsCompleteGesture ((float) i); };
    tabs.onChange      = [this] (int i) { showTab (i); };
    addChildComponent (shapes);
    addChildComponent (placement);
    addChildComponent (tabs);

    onButton.setButtonText ("ON");
    onButton.setTooltip ("Band on / off (Alt + click on the graph point)");
    listenButton.setButtonText ("LISTEN");
    listenButton.setTooltip ("Hear only what this band looks at (its frequency range of the input)");
    listenButton.getProperties().set ("rzLed", (juce::int64) kDynColour.getARGB());
    listenButton.onClick = [this] { proc.setAudition (listenButton.getToggleState() ? selected : -1); };
    tubeButton.setButtonText ("TUBE");
    tubeButton.setTooltip ("This band's own EF804S tube, on this band's frequency range only\n"
                           "(its own DRIVE / FOCUS / MIX; ALL in the main panel is a separate whole-signal stage)");
    tubeButton.getProperties().set ("rzLed", (juce::int64) juce::Colour (0xffff8f4f).getARGB());
    xfmrButton.setButtonText ("XFMR");
    xfmrButton.setTooltip ("This band's own output transformer, on this band's frequency range only\n"
                           "(its own AMOUNT; the character follows the band frequency)");
    xfmrButton.getProperties().set ("rzLed", (juce::int64) palette::accent2.getARGB());
    dynButton.setButtonText ("DYN");
    dynButton.setTooltip ("Dynamic EQ: this band's gain moves with the level in its frequency range\n"
                          "(Alt + mouse wheel on the graph point sets the RANGE)");
    dynButton.getProperties().set ("rzLed", (juce::int64) kDynColour.getARGB());
    autoButton.setButtonText ("AUTO");
    autoButton.setTooltip ("Automatic threshold: follows the band's own recent level, so only what pokes out moves");
    autoButton.getProperties().set ("rzLed", (juce::int64) kDynColour.getARGB());
    specButton.setButtonText ("SPECTRAL");
    specButton.setTooltip ("Spectral dynamics: only the frequencies inside the band that go over the threshold move\n"
                           "ZERO LATENCY: zero-latency filter bank   /   HQ: linear phase FFT (about 43 ms latency)");
    specButton.getProperties().set ("rzLed", (juce::int64) kDynColour.getARGB());
    specButton.onClick = [this]
    {
        // 스펙트럴은 다이내믹 안의 방식: 켜면 DYN도 같이 켬
        if (selected < 0 || ! specButton.getToggleState())
            return;
        if (auto* d = bandParam (proc, selected, pillow::ids::dyn); d != nullptr && d->getValue() < 0.5f)
            setParameter (d, 1.0f);
    };
    driveKnob.setTooltip ("Tube drive for this band (0 = almost clean, 30+ = heavy)");
    focusKnob.setTooltip ("Odd (left) / even (right) harmonics of this band's tube");
    mixKnob.setTooltip ("How much of this band's tube colour is blended in");
    xfAmtKnob.setTooltip ("Transformer amount for this band. Character follows the band frequency:\n"
                          "below 200 Hz core saturation, 200 Hz - 4 kHz density, above 4 kHz high saturation");
    rangeKnob.setTooltip ("How far the band may move: minus = cut when loud (compress), plus = lift when loud (expand)");
    threshKnob.setTooltip ("Level where the band starts to move (sine dBFS). Used when AUTO is off");
    attackKnob.setTooltip ("Auto = matched to the band frequency. Left = faster (down to 1/8), right = slower (up to 8x)");
    releaseKnob.setTooltip ("Auto = matched to the band frequency. Left = faster (down to 1/8), right = slower (up to 8x)");
    densityKnob.setTooltip ("Spectral selectivity: low = wider areas around a peak move, high = only the exact peaks");
    for (auto* b : { &onButton, &listenButton, &tubeButton, &xfmrButton, &dynButton, &autoButton, &specButton })
    {
        b->setWantsKeyboardFocus (false);         // Return 키가 버튼을 누르지 않게
        addChildComponent (*b);
    }
    removeButton.setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    removeButton.setTooltip ("Delete this band (Delete key on the graph)");
    removeButton.onClick = [this]
    {
        if (selected < 0)
            return;
        const int b = selected;
        setSelectedBand (-1);
        removeBand (proc, b);
        if (onSelect)
            onSelect (-1);
    };
    addChildComponent (removeButton);

    tab = juce::jlimit (0, 1, (int) proc.apvts.state.getProperty (pillow::ids::uiBandTab, 0));
    tabs.setIndex (tab);
    params = proc.readParams();
    startTimerHz (20);
}

BandPanel::~BandPanel()
{
    stopTimer();
    proc.setAudition (-1);
}

void BandPanel::stopListening()
{
    listenButton.setToggleState (false, juce::dontSendNotification);
    proc.setAudition (-1);
}

void BandPanel::showTab (int t)
{
    t = juce::jlimit (0, 1, t);
    tabs.setIndex (t);
    if (t == tab)
        return;
    tab = t;
    proc.apvts.state.setProperty (pillow::ids::uiBandTab, tab, nullptr);
    updateVisibility();
    repaint();
}

void BandPanel::setSelectedBand (int band)
{
    if (band >= 0 && ! params.bands[(size_t) band].used)
        params = proc.readParams();   // 방금 추가된 밴드일 수 있음
    if (band == selected)
        return;
    selected = band;
    stopListening();
    attach (band);
    repaint();
}

bool BandPanel::knobBeingDragged() const
{
    if (draggingKnob != nullptr)
        return true;
    for (auto* k : { &freqKnob, &gainKnob, &qKnob, &slopeKnob, &driveKnob, &focusKnob, &mixKnob, &xfAmtKnob,
                     &rangeKnob, &threshKnob, &attackKnob, &releaseKnob, &densityKnob })
        if (k->isMouseButtonDown (true))
            return true;
    return false;
}

void BandPanel::updateVisibility()
{
    const bool show = attachedBand >= 0;
    for (auto* c : std::initializer_list<juce::Component*> { &freqKnob, &gainKnob, &qKnob, &shapes, &placement,
                                                             &onButton, &listenButton, &removeButton, &tabs })
        c->setVisible (show);
    for (auto* c : std::initializer_list<juce::Component*> { &driveKnob, &focusKnob, &mixKnob, &xfAmtKnob, &tubeButton, &xfmrButton })
        c->setVisible (show && tab == 0);
    for (auto* c : std::initializer_list<juce::Component*> { &rangeKnob, &threshKnob, &attackKnob, &releaseKnob, &densityKnob,
                                                             &dynButton, &autoButton, &specButton })
        c->setVisible (show && tab == 1);
    slopeKnob.setVisible (show && selected >= 0 && rz::typeHasSlope (params.bands[(size_t) selected].type));
}

void BandPanel::attach (int band)
{
    // 노브를 잡고 있는 동안 바꾸면 그 파라미터의 제스처 끝이 사라짐 -> 손을 뗀 뒤에 (타이머)
    if (knobBeingDragged())
    {
        pendingAttach = band;
        return;
    }
    pendingAttach = -2;
    attachedBand = band;

    freqAtt.reset(); gainAtt.reset(); qAtt.reset(); slopeAtt.reset();
    driveAtt.reset(); focusAtt.reset(); mixAtt.reset(); xfAmtAtt.reset();
    rangeAtt.reset(); threshAtt.reset(); attackAtt.reset(); releaseAtt.reset(); densityAtt.reset();
    onAtt.reset(); tubeAtt.reset(); xfmrAtt.reset(); dynAtt.reset(); autoAtt.reset(); specAtt.reset();
    shapeAtt.reset(); placeAtt.reset();

    updateVisibility();
    if (band < 0)
        return;

    auto slider = [this, band] (std::unique_ptr<SliderAttachment>& att, const char* field, RZKnob& knob, bool resetToDefault = true)
    {
        att = std::make_unique<SliderAttachment> (proc.apvts, pillow::ids::band (band, field), knob);
        if (auto* prm = bandParam (proc, band, field))
            knob.setDoubleClickReturnValue (resetToDefault, (double) prm->convertFrom0to1 (prm->getDefaultValue()));
    };
    auto button = [this, band] (std::unique_ptr<ButtonAttachment>& att, const char* field, juce::ToggleButton& b)
    {
        att = std::make_unique<ButtonAttachment> (proc.apvts, pillow::ids::band (band, field), b);
    };
    slider (freqAtt,  pillow::ids::freq,  freqKnob, false);
    slider (gainAtt,  pillow::ids::gain,  gainKnob);
    slider (qAtt,     pillow::ids::q,     qKnob);
    slider (slopeAtt, pillow::ids::slope, slopeKnob);
    slider (driveAtt, pillow::ids::drive, driveKnob);
    slider (focusAtt, pillow::ids::focus, focusKnob);
    slider (mixAtt,   pillow::ids::mix,   mixKnob);
    slider (xfAmtAtt, pillow::ids::xfAmt, xfAmtKnob);
    slider (rangeAtt,   pillow::ids::range,   rangeKnob);
    slider (threshAtt,  pillow::ids::thresh,  threshKnob);
    slider (attackAtt,  pillow::ids::attack,  attackKnob);
    slider (releaseAtt, pillow::ids::release, releaseKnob);
    slider (densityAtt, pillow::ids::density, densityKnob);
    button (onAtt,   pillow::ids::on,     onButton);
    button (tubeAtt, pillow::ids::tube,   tubeButton);
    button (xfmrAtt, pillow::ids::xfmr,   xfmrButton);
    button (dynAtt,  pillow::ids::dyn,    dynButton);
    button (autoAtt, pillow::ids::autoTh, autoButton);
    button (specAtt, pillow::ids::spec,   specButton);

    if (auto* typeParam = bandParam (proc, band, pillow::ids::type))
    {
        shapeAtt = std::make_unique<juce::ParameterAttachment> (*typeParam, [this] (float v)
        {
            shapes.setIndex (juce::roundToInt (v));
            params = proc.readParams();          // 모양에 따라 GAIN · Q · SLOPE 표시가 바로 바뀌게
            updateTypeDependent();
            repaint();
        });
        shapeAtt->sendInitialUpdate();
    }
    if (auto* placeParam = bandParam (proc, band, pillow::ids::place))
    {
        placeAtt = std::make_unique<juce::ParameterAttachment> (*placeParam, [this] (float v)
        {
            placement.setIndex (juce::roundToInt (v));
            params = proc.readParams();
            updateTypeDependent();
            repaint();
        });
        placeAtt->sendInitialUpdate();
    }

    const auto colour = bandColour (band);
    shapes.setAccent (colour);
    placement.setAccent (colour);
    onButton.getProperties().set ("rzLed", (juce::int64) colour.getARGB());
    onButton.repaint();
    updateTypeDependent();
}

void BandPanel::updateTypeDependent()
{
    if (selected < 0 || attachedBand != selected)
        return;
    const auto& b = params.bands[(size_t) selected];
    gainKnob.setEnabled (rz::typeHasGain (b.type));
    qKnob.setEnabled (rz::typeHasQ (b.type));
    slopeKnob.setVisible (rz::typeHasSlope (b.type));

    const bool mono = proc.getNumChannelsInUse() < 2;
    const bool live = params.eqOn && b.on && ! (mono && b.place == rz::Placement::Side);
    auto setDimmed = [] (RZKnob& k, bool dim)
    {
        if (k.dimmed != dim)
        {
            k.dimmed = dim;
            k.repaint();
        }
    };
    for (auto* k : { &freqKnob, &gainKnob, &qKnob, &slopeKnob })
        setDimmed (*k, ! live);

    // 튜브 · 트랜스포머 노브는 그 스위치를 켰을 때만 쓰임 (꺼져 있어도 미리 맞춰 둘 수는 있음)
    for (auto* k : { &driveKnob, &focusKnob, &mixKnob })
        setDimmed (*k, ! (live && b.tube));
    setDimmed (xfAmtKnob, ! (live && b.xfmr));

    // 다이내믹: Bell · 셸프 · 틸트만, 스펙트럴은 Bell · 셸프만
    const bool canDyn = rz::typeHasDynamics (b.type), canSpec = rz::typeHasSpectral (b.type);
    for (juce::Component* c : { (juce::Component*) &dynButton, (juce::Component*) &autoButton, (juce::Component*) &rangeKnob,
                                (juce::Component*) &threshKnob, (juce::Component*) &attackKnob, (juce::Component*) &releaseKnob })
        c->setEnabled (canDyn);
    specButton.setEnabled (canSpec);
    densityKnob.setEnabled (canSpec);
    const bool dynLive = live && canDyn && b.dyn;
    for (auto* k : { &rangeKnob, &attackKnob, &releaseKnob })
        setDimmed (*k, ! dynLive);
    setDimmed (threshKnob, ! dynLive || b.autoTh);
    setDimmed (densityKnob, ! (dynLive && canSpec && b.spectral));
}

void BandPanel::timerCallback()
{
    if (pendingAttach != -2 && ! knobBeingDragged())
        attach (pendingAttach);

    if (listenButton.getToggleState() && proc.getAudition() != selected)
        listenButton.setToggleState (false, juce::dontSendNotification);   // 다른 곳에서 꺼짐

    // 지금 게인 변화 (다이내믹 탭)
    if (selected >= 0 && tab == 1)
    {
        const float v = params.bands[(size_t) selected].dyn ? proc.getDynamicGainDb (selected) : 0.0f;
        if (std::abs (v - dynShown) > 0.05f)
        {
            dynShown = v;
            repaint (0, 280, 110, 40);
        }
    }

    const auto p = proc.readParams();
    if (p == params)
        return;
    params = p;

    if (selected >= 0 && ! params.bands[(size_t) selected].used)
    {
        setSelectedBand (-1);
        if (onSelect)
            onSelect (-1);
    }
    updateTypeDependent();
    repaint();
}

juce::Rectangle<float> BandPanel::chipBounds (int slot) const noexcept
{
    return { 342.0f + 23.0f * (float) slot, 12.0f, 20.0f, 20.0f };
}

void BandPanel::resized()
{
    shapes.setBounds    (22, 70, 300, 36);
    placement.setBounds (22, 128, 300, 28);
    onButton.setBounds     (22, 166, 72, 26);
    listenButton.setBounds (100, 166, 92, 26);
    removeButton.setBounds (252, 166, 70, 26);

    freqKnob.setBounds  (352, 58, 92, 134);
    gainKnob.setBounds  (448, 58, 92, 134);
    qKnob.setBounds     (544, 58, 92, 134);
    slopeKnob.setBounds (634, 68, 84, 116);

    tabs.setBounds (22, 202, 220, 22);

    // ANALOG: 튜브 | 트랜스포머
    tubeButton.setBounds (22, 252, 84, 28);
    driveKnob.setBounds  (118, 246, 84, 70);
    focusKnob.setBounds  (206, 246, 84, 70);
    mixKnob.setBounds    (294, 246, 84, 70);
    xfmrButton.setBounds (412, 252, 84, 28);
    xfAmtKnob.setBounds  (508, 246, 84, 70);

    // DYNAMICS: DYN · RANGE · THRESHOLD(+AUTO) · ATTACK · RELEASE | SPECTRAL · DENSITY
    dynButton.setBounds   (22, 252, 78, 28);
    rangeKnob.setBounds   (106, 246, 76, 70);
    threshKnob.setBounds  (184, 246, 76, 70);
    autoButton.setBounds  (262, 256, 72, 24);
    attackKnob.setBounds  (338, 246, 76, 70);
    releaseKnob.setBounds (416, 246, 76, 70);
    specButton.setBounds  (508, 252, 116, 28);
    densityKnob.setBounds (630, 246, 78, 70);
}

juce::String BandPanel::transformerCharacter (const pillow::rz::BandParams& b)
{
    // 코어의 TransformerBand와 같은 규칙 (200 Hz · 4 kHz 경계, 1옥타브에 걸쳐 섞음)
    const double hz = rz::transformerCharacterHz (b.type, b.freq);
    const double a = rz::clamp01 (0.5 + std::log2 (hz / rz::TransformerStage::kXoverLowHz));
    const double h = rz::clamp01 (0.5 + std::log2 (hz / rz::TransformerStage::kXoverHighHz));
    if (a <= 0.0) return "LOW  /  core";
    if (h >= 1.0) return "HIGH  /  air";
    if (a < 1.0)  return "LOW - MID";
    if (h > 0.0)  return "MID - HIGH";
    return "MID  /  density";
}

void BandPanel::paint (juce::Graphics& g)
{
    drawPanelBackground (g, getLocalBounds().toFloat());
    const bool has = selected >= 0 && params.bands[(size_t) selected].used;

    // 머리: 고른 밴드 + 번호 칩
    if (has)
    {
        const auto& b = params.bands[(size_t) selected];
        g.setColour (b.on ? bandColour (selected) : juce::Colour (0xff5b6672));
        g.fillEllipse (26.0f, 15.0f, 14.0f, 14.0f);
        g.setColour (palette::text);
        g.setFont (uiFont (14.0f, true, 0.12f));
        g.drawText ("BAND " + juce::String (selected + 1), 48, 10, 90, 24, juce::Justification::centredLeft, false);
        g.setColour (palette::dim);
        g.setFont (uiFont (11.0f, true, 0.08f));
        juce::String info = pillow::bandTypeNames()[(int) b.type].toUpperCase() + "   /   "
                          + pillow::placementNames()[(int) b.place].toUpperCase();
        if (b.tube) info += "   /   TUBE";
        if (b.xfmr) info += "   /   XFMR";
        if (b.dyn && rz::typeHasDynamics (b.type)) info += b.spectral && rz::typeHasSpectral (b.type) ? "   /   SPECTRAL" : "   /   DYN";
        g.drawFittedText (info, 136, 10, 200, 24, juce::Justification::centredLeft, 1, 0.7f);
    }
    else
    {
        g.setColour (palette::text);
        g.setFont (uiFont (14.0f, true, 0.12f));
        g.drawText ("BANDS", 28, 10, 90, 24, juce::Justification::centredLeft, false);
        g.setColour (palette::dim);
        g.setFont (uiFont (11.0f));
        g.drawText ("up to 16, each with its own settings", 96, 10, 240, 24, juce::Justification::centredLeft, false);
    }

    numChips = 0;
    for (int i = 0; i < pillow::rz::kMaxBands; ++i)
        if (params.bands[(size_t) i].used)
            chipBand[(size_t) numChips++] = i;

    for (int s = 0; s < numChips; ++s)
    {
        const int band = chipBand[(size_t) s];
        const auto& b = params.bands[(size_t) band];
        const auto r = chipBounds (s);
        const auto col = b.on ? bandColour (band) : juce::Colour (0xff5b6672);
        if (band == selected)
        {
            g.setColour (col.withAlpha (0.3f));
            g.fillEllipse (r.expanded (3.0f));
        }
        g.setColour (col);
        g.fillEllipse (r);
        g.setColour (band == selected ? palette::text : palette::panel);
        g.drawEllipse (r, band == selected ? 1.8f : 1.0f);
        g.setColour (palette::bgDark);
        g.setFont (uiFont (10.0f, true));
        g.drawText (juce::String (band + 1), r, juce::Justification::centred, false);
    }

    g.setColour (palette::panelEdge.withAlpha (0.7f));
    g.drawHorizontalLine (43, 20.0f, (float) getWidth() - 20.0f);

    if (! has)
    {
        g.setColour (palette::dim);
        g.setFont (uiFont (12.5f));
        g.drawFittedText ("Double-click the graph to add a band, or right-click it to pick a shape.\n"
                          "Drag a point: frequency and gain   /   mouse wheel: Q   /   Alt + wheel: dynamic range\n"
                          "Ctrl (Cmd) + drag: Q   /   Alt + click: band on/off   /   Delete: remove band\n"
                          "Click a point (or a number above) to open that band's settings here.\n\n"
                          "Every band has its own EF804S tube and output transformer (ANALOG)\n"
                          "and its own dynamic / spectral EQ (DYNAMICS), on that band's frequency range only.",
                          juce::Rectangle<int> (30, 56, getWidth() - 60, 240), juce::Justification::centred, 8);
        return;
    }

    const auto& b = params.bands[(size_t) selected];
    g.setColour (palette::text.withAlpha (0.85f));
    g.setFont (uiFont (11.0f, true, 0.12f));
    g.drawText ("SHAPE",     22, 52,  200, 14, juce::Justification::centredLeft, false);
    g.drawText ("PLACEMENT", 22, 110, 200, 14, juce::Justification::centredLeft, false);

    drawSmallLabel (g, freqKnob, "FREQ");
    drawSmallLabel (g, gainKnob, "GAIN");
    drawSmallLabel (g, qKnob, "Q");
    if (slopeKnob.isVisible())
        drawSmallLabel (g, slopeKnob, "SLOPE");

    g.setColour (palette::panelEdge.withAlpha (0.6f));
    g.drawVerticalLine (338, 52.0f, 196.0f);
    g.setColour (palette::panelEdge.withAlpha (0.7f));
    g.drawHorizontalLine (213, 250.0f, (float) getWidth() - 20.0f);

    if (tab == 0)
    {
        // ANALOG: 이 밴드만의 튜브 · 트랜스포머
        g.setColour (palette::panelEdge.withAlpha (0.6f));
        g.drawVerticalLine (396, 236.0f, 314.0f);

        drawSmallLabel (g, driveKnob, "DRIVE");
        drawSmallLabel (g, focusKnob, "FOCUS");
        drawSmallLabel (g, mixKnob,   "MIX");
        drawSmallLabel (g, xfAmtKnob, "AMOUNT");

        g.setColour (palette::dim);
        g.setFont (uiFont (10.5f));
        g.drawText ("EF804S", 22, 234, 84, 14, juce::Justification::centred, false);
        g.drawText ("this band only", 16, 286, 96, 14, juce::Justification::centred, false);
        g.drawText ("OUTPUT XFMR", 406, 234, 96, 14, juce::Justification::centred, false);
        g.drawText ("this band only", 406, 286, 96, 14, juce::Justification::centred, false);

        // 트랜스포머 성격: 밴드 주파수로 자동 (코어와 같은 규칙)
        const auto lit = b.xfmr && params.eqOn && b.on;
        g.setColour (palette::text.withAlpha (lit ? 0.85f : 0.4f));
        g.setFont (uiFont (11.0f, true, 0.12f));
        g.drawText ("CHARACTER", 610, 250, 110, 14, juce::Justification::centredLeft, false);
        g.setColour (lit ? palette::accent2 : palette::dim);
        g.setFont (uiFont (12.0f, true, 0.06f));
        g.drawFittedText (transformerCharacter (b), 610, 267, 106, 16, juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (palette::dim);
        g.setFont (uiFont (10.5f));
        g.drawText ("follows the band freq", 610, 285, 112, 14, juce::Justification::centredLeft, false);
        return;
    }

    // DYNAMICS
    const bool canDyn = rz::typeHasDynamics (b.type), canSpec = rz::typeHasSpectral (b.type);
    g.setColour (palette::panelEdge.withAlpha (0.6f));
    g.drawVerticalLine (500, 236.0f, 314.0f);
    drawSmallLabel (g, rangeKnob,   "RANGE");
    drawSmallLabel (g, threshKnob,  "THRESHOLD");
    drawSmallLabel (g, attackKnob,  "ATTACK");
    drawSmallLabel (g, releaseKnob, "RELEASE");
    drawSmallLabel (g, densityKnob, "DENSITY");

    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f));
    g.drawText ("DYNAMIC EQ", 14, 234, 94, 14, juce::Justification::centred, false);
    if (! canDyn)
    {
        g.drawFittedText ("Bell, shelf and tilt bands", 14, 284, 94, 28, juce::Justification::centred, 2, 0.8f);
    }
    else
    {
        // 지금 게인 변화 (스펙트럴이면 밴드 안에서 가장 크게 움직인 곳)
        const float range = b.dynRange;
        const float v = b.dyn ? dynShown : 0.0f;
        g.setColour (b.dyn ? palette::text : palette::dim);
        g.setFont (uiFont (11.5f, true, 0.04f));
        g.drawText (pillow::formatDb (v), 14, 284, 94, 14, juce::Justification::centred, false);
        const auto bar = juce::Rectangle<float> (25.0f, 302.0f, 72.0f, 5.0f);
        g.setColour (palette::bgDark);
        g.fillRoundedRectangle (bar, 2.5f);
        if (std::abs (range) > 0.01f && b.dyn)
        {
            const float frac = juce::jlimit (0.0f, 1.0f, std::abs (v) / std::abs (range));
            g.setColour (bandColour (selected));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * frac), 2.5f);
        }
    }
    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f));
    g.drawText ("ONLY THE PEAKS", 508, 234, 116, 14, juce::Justification::centred, false);
    juce::String specInfo = ! canSpec ? juce::String ("Bell and shelf bands")
                          : proc.isSpectralLinearPhase() ? juce::String ("HQ: linear phase")
                                                         : juce::String ("zero latency");
    g.drawFittedText (specInfo, 508, 286, 116, 14, juce::Justification::centred, 1, 0.8f);
}

void BandPanel::mouseDown (const juce::MouseEvent& e)
{
    for (int s = 0; s < numChips; ++s)
        if (chipBounds (s).expanded (2.0f).contains (e.position))
        {
            const int band = chipBand[(size_t) s];
            setSelectedBand (band);
            if (onSelect)
                onSelect (band);
            return;
        }
}

//==============================================================================
//  MainPanel
//==============================================================================
MainPanel::MainPanel (PillowRZAudioProcessor& p) : proc (p)
{
    setupKnob (drive,    pillow::ids::drive,    false);
    setupKnob (focus,    pillow::ids::focus,    false);
    setupKnob (xfAmount, pillow::ids::xfAmount, false);
    setupKnob (mix,      pillow::ids::mix,      false);
    setupKnob (output,   pillow::ids::output,   true);

    drive.setTooltip ("Whole-signal tube drive (used when ALL is on)");
    focus.setTooltip ("Odd (left) / even (right) harmonics of the whole-signal tube");
    xfAmount.setTooltip ("Whole-signal transformer amount (used when ALL is on)\n"
                         "lows: core saturation, mids: density, highs: saturation + gentle air roll-off");
    mix.setTooltip ("Dry / wet of the whole plugin (EQ, tubes and transformers)");

    eqSwitch.setButtonText ("EQ");
    eqSwitch.setTooltip ("All bands on / off");
    addAndMakeVisible (eqSwitch);
    eqAttachment = std::make_unique<ButtonAttachment> (proc.apvts, pillow::ids::eqOn, eqSwitch);

    tubeAllSwitch.setButtonText ("ALL");
    tubeAllSwitch.setTooltip ("Whole signal through the EF804S after the EQ (classic RZ062)\n"
                              "Bands with TUBE on have their own tube in addition");
    tubeAllSwitch.getProperties().set ("rzLed", (juce::int64) juce::Colour (0xffff8f4f).getARGB());
    xfAllSwitch.setButtonText ("ALL");
    xfAllSwitch.setTooltip ("Whole signal through the output transformer (classic RZ062)\n"
                            "Bands with XFMR on have their own transformer in addition");
    xfAllSwitch.getProperties().set ("rzLed", (juce::int64) palette::accent2.getARGB());
    for (auto* b : { &eqSwitch, &tubeAllSwitch, &xfAllSwitch })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    setMouseClickGrabsKeyboardFocus (false);
    tubeAllAttachment = std::make_unique<ButtonAttachment> (proc.apvts, pillow::ids::driveOn, tubeAllSwitch);
    xfAllAttachment   = std::make_unique<ButtonAttachment> (proc.apvts, pillow::ids::xfAll, xfAllSwitch);

    timerCallback();
    startTimerHz (10);
}

MainPanel::~MainPanel()
{
    stopTimer();
}

void MainPanel::setupKnob (RZKnob& knob, const char* paramId, bool large)
{
    knob.large = large;
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 92, 18);
    addAndMakeVisible (knob);
    attachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, paramId, knob));

    if (auto* prm = proc.apvts.getParameter (paramId))
        knob.setDoubleClickReturnValue (true, (double) prm->convertFrom0to1 (prm->getDefaultValue()));
}

void MainPanel::timerCallback()
{
    const auto p = proc.readParams();

    // 어디에 걸리고 있는지: 신호 전체(ALL) + 따로 가진 밴드 목록
    auto usage = [&p] (bool all, bool tube)
    {
        juce::StringArray list;
        for (int i = 0; i < pillow::rz::kMaxBands; ++i)
        {
            const auto& b = p.bands[(size_t) i];
            if (p.eqOn && b.isActive() && (tube ? b.tube : b.xfmr))
                list.add (juce::String (i + 1));
        }
        const juce::String bands = (list.size() == 1 ? "band " : "bands ") + list.joinIntoString (", ");
        if (all)
            return list.isEmpty() ? juce::String ("whole signal") : "whole signal  +  " + bands;
        if (list.isEmpty())
            return juce::String (tube ? "off   /   or TUBE on a band" : "off   /   or XFMR on a band");
        return "off   /   " + bands + " (own)";
    };

    const auto t = usage (p.driveOn, true);
    const auto x = usage (p.xfAll, false);
    if (t != tubeUsage || x != xfUsage)
    {
        tubeUsage = t;
        xfUsage = x;
        repaint();
    }

    auto setDimmed = [] (RZKnob& k, bool dim)
    {
        if (k.dimmed != dim)
        {
            k.dimmed = dim;
            k.repaint();
        }
    };
    setDimmed (drive, ! p.driveOn);
    setDimmed (focus, ! p.driveOn);
    setDimmed (xfAmount, ! p.xfAll);
}

void MainPanel::resized()
{
    eqSwitch.setBounds      (24, 54, 74, 28);
    tubeAllSwitch.setBounds (214, 100, 70, 26);
    xfAllSwitch.setBounds   (214, 282, 70, 26);

    drive.setBounds (34,  150, 100, 112);
    focus.setBounds (170, 150, 100, 112);

    xfAmount.setBounds (34, 332, 100, 112);

    mix.setBounds    (34, 512, 100, 112);
    output.setBounds (160, 504, 120, 124);
}

void MainPanel::paint (juce::Graphics& g)
{
    drawPanelBackground (g, getLocalBounds().toFloat());
    const float w = (float) getWidth();

    drawSectionTitle (g, 26, 12, "MAIN", "whole-signal tube & transformer, output");

    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f));
    g.drawText ("all bands on / off", 106, 54, 180, 28, juce::Justification::centredLeft, false);

    auto drawUsage = [&g] (const juce::String& text, int y)
    {
        g.setColour (palette::dim);
        g.setFont (uiFont (10.5f));
        g.drawFittedText (text, 26, y, 182, 14, juce::Justification::centredLeft, 1, 0.75f);   // ALL 스위치와 겹치지 않게
    };

    g.setColour (palette::panelEdge.withAlpha (0.7f));
    g.drawHorizontalLine (94, 20.0f, w - 20.0f);
    drawSectionTitle (g, 26, 100, "EF804S TUBE", {});
    drawUsage (tubeUsage, 115);
    drawSmallLabel (g, drive, "DRIVE");
    drawSmallLabel (g, focus, "FOCUS");

    g.setColour (palette::dim);
    g.setFont (uiFont (10.0f));
    g.drawText ("ODD", focus.getX() + 8, focus.getBottom() - 32, 30, 12, juce::Justification::centredLeft, false);
    g.drawText ("EVEN", focus.getRight() - 38, focus.getBottom() - 32, 30, 12, juce::Justification::centredRight, false);

    g.setColour (palette::panelEdge.withAlpha (0.7f));
    g.drawHorizontalLine (276, 20.0f, w - 20.0f);
    drawSectionTitle (g, 26, 282, "TRANSFORMER", {});
    drawUsage (xfUsage, 297);
    drawSmallLabel (g, xfAmount, "AMOUNT");

    g.setColour (palette::dim);
    g.setFont (uiFont (10.5f));
    g.drawFittedText ("lows < 200 Hz: core saturation\nmids: density (2nd + 3rd)\nhighs > 4 kHz: saturation\n   + gentle air roll-off",
                      150, 346, (int) w - 166, 70, juce::Justification::centredLeft, 4, 0.85f);

    g.setColour (palette::panelEdge.withAlpha (0.7f));
    g.drawHorizontalLine (458, 20.0f, w - 20.0f);
    drawSectionTitle (g, 26, 464, "OUTPUT", {});
    drawSmallLabel (g, mix, "MIX");
    drawSmallLabel (g, output, "LEVEL");

    g.setColour (palette::panelEdge.withAlpha (0.7f));
    g.drawHorizontalLine (644, 20.0f, w - 20.0f);
    g.setColour (palette::dim);
    g.setFont (uiFont (11.0f));
    g.drawFittedText ("Shift + drag: fine   /   double-click: reset\nClick a value: type it",
                      26, 652, (int) w - 52, 40, juce::Justification::centredLeft, 2);
}
} // namespace pillow::ui
