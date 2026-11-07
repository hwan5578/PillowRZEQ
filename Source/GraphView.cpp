#include "GraphView.h"

namespace rz = pillow::rz;

namespace pillow::ui
{
namespace
{
    juce::String shortHz (double f)
    {
        if (f >= 1000.0)
        {
            const double k = f / 1000.0;
            return (std::abs (k - std::round (k)) < 0.01 ? juce::String ((int) std::round (k)) : juce::String (k, 1)) + "k";
        }
        return juce::String ((int) std::round (f));
    }

    const char* placeLetter (rz::Placement p) noexcept
    {
        switch (p)
        {
            case rz::Placement::Left:   return "L";
            case rz::Placement::Right:  return "R";
            case rz::Placement::Mid:    return "M";
            case rz::Placement::Side:   return "S";
            case rz::Placement::Stereo: break;
        }
        return "";
    }

    // 전체 곡선 색: 밴드 색과 겹치지 않게 무채색 계열 (두 번째 채널은 회청색)
    juce::Colour viewColour (rz::Placement p) noexcept
    {
        return (p == rz::Placement::Right || p == rz::Placement::Side) ? juce::Colour (0xffa9b8c9) : palette::text;
    }
}

//==============================================================================
GraphView::GraphView (PillowRZAudioProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);

    const auto& st = proc.apvts.state;
    rangeDb = (int) st.getProperty (pillow::ids::uiRange, 12);
    if (rangeDb != 6 && rangeDb != 12 && rangeDb != 24)
        rangeDb = 12;
    analyzerMode = juce::jlimit (0, 2, (int) st.getProperty (pillow::ids::uiAnalyzer, 2));

    pullBuffer.resize ((size_t) pillow::AnalyzerTap::kCapacity);
    proc.setAnalyzerActive (analyzerMode > 0);

    refreshParams();
    startTimerHz (30);
}

GraphView::~GraphView()
{
    juce::PopupMenu::dismissAllActiveMenus();   // 창을 닫을 때 열린 메뉴가 남지 않게
    stopTimer();
    endDrag();
    proc.setAnalyzerActive (false);
}

//==============================================================================
float GraphView::xForFreq (double hz) const noexcept
{
    const double t = std::log (juce::jmax (hz, 1.0) / kMinHz) / std::log (kMaxHz / kMinHz);
    return plot.getX() + (float) t * plot.getWidth();
}

double GraphView::freqForX (float x) const noexcept
{
    const double t = (double) ((x - plot.getX()) / juce::jmax (1.0f, plot.getWidth()));
    return kMinHz * std::pow (kMaxHz / kMinHz, t);
}

float GraphView::yForDb (double db) const noexcept
{
    const float mid = 0.5f * (eqTop + eqBottom), half = 0.5f * (eqBottom - eqTop);
    return mid - (float) (db / (double) rangeDb) * half;
}

double GraphView::dbForY (float y) const noexcept
{
    const float mid = 0.5f * (eqTop + eqBottom), half = juce::jmax (1.0f, 0.5f * (eqBottom - eqTop));
    return (double) ((mid - y) / half) * (double) rangeDb;
}

float GraphView::ySpec (double db) const noexcept
{
    const double t = (kSpecTopDb - db) / (kSpecTopDb - kSpecBottomDb);
    return plot.getY() + (float) t * plot.getHeight();
}

juce::Point<float> GraphView::nodePosition (int band) const noexcept
{
    const auto& b = params.bands[(size_t) band];
    const float x = juce::jlimit (plot.getX() + 6.0f, plot.getRight() - 6.0f, xForFreq (b.freq));
    const float y = rz::typeHasGain (b.type) ? yForDb (b.gain) : yForDb (0.0);
    return { x, juce::jlimit (plot.getY() + 9.0f, plot.getBottom() - 9.0f, y) };
}

//==============================================================================
void GraphView::resized()
{
    plot     = getLocalBounds().toFloat().reduced (1.0f);
    eqTop    = plot.getY() + 30.0f;
    eqBottom = plot.getBottom() - 30.0f;

    analyzerButton = { plot.getRight() - 262.0f, plot.getY() + 7.0f, 150.0f, 19.0f };
    rangeButton    = { plot.getRight() - 106.0f, plot.getY() + 7.0f,  96.0f, 19.0f };

    const int n = juce::jmax (2, (int) (plot.getWidth() / 3.0f) + 1);
    pointFreq.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        pointFreq[(size_t) i] = (float) freqForX (plot.getX() + plot.getWidth() * (float) i / (float) (n - 1));
    for (auto& s : spectra)
        s.level.assign ((size_t) n, (float) kSpecFloorDb);

    rebuildCurves();
    rebuildSpectrumPaths();
}

void GraphView::timerCallback()
{
    const auto& st = proc.apvts.state;   // 프리셋 · 세션을 불러오면 RANGE / ANALYZER 도 따라감
    applyUiSettings ((int) st.getProperty (pillow::ids::uiRange, rangeDb),
                     (int) st.getProperty (pillow::ids::uiAnalyzer, analyzerMode));

    bool dirty = refreshParams();
    if (analyzerMode > 0)
    {
        updateAnalyzer();
        dirty = true;
    }
    if (dirty)
        repaint();
}

void GraphView::refreshNow()
{
    if (refreshParams())
        repaint();
}

bool GraphView::refreshParams()
{
    const auto p = proc.readParams();
    const double r = proc.getProcessingRate();
    const int ch = proc.getNumChannelsInUse();
    if (hasParams && p == params && juce::exactlyEqual (r, rate) && ch == channels)
        return false;

    params = p;
    rate = r;
    channels = ch;
    hasParams = true;

    if (hovered >= 0 && ! params.bands[(size_t) hovered].used)
        hovered = -1;
    if (selected >= 0 && ! params.bands[(size_t) selected].used)
    {
        selected = -1;
        if (onSelect)
            onSelect (-1);
    }

    rebuildCurves();
    return true;
}

void GraphView::rebuildCurves()
{
    totals.clear();
    for (auto& c : bandCurves) c.clear();
    selectedFill.clear();
    if (! hasParams || plot.isEmpty())
        return;

    // 모노 트랙: Side 밴드는 할 일이 없고 나머지는 그 채널에 -> 곡선 하나
    auto shown = params;
    if (channels < 2)
        for (auto& b : shown.bands)
        {
            if (b.place == rz::Placement::Side) b.on = false;
            else                                b.place = rz::Placement::Stereo;
        }
    const rz::ResponseModel model (shown, rate);

    const int n = juce::jmax (2, (int) (plot.getWidth() / (float) kPointStep) + 1);
    std::vector<double> freqs ((size_t) n);
    std::vector<float> xs ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        xs[(size_t) i]    = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
        freqs[(size_t) i] = freqForX (xs[(size_t) i]);
    }

    const float yMin = plot.getY() - 4.0f, yMax = plot.getBottom() + 4.0f;
    auto makePath = [&] (auto&& dbAt)
    {
        juce::Path path;
        for (int i = 0; i < n; ++i)
        {
            const float y = juce::jlimit (yMin, yMax, yForDb (dbAt (freqs[(size_t) i])));
            if (i == 0) path.startNewSubPath (xs[(size_t) i], y);
            else        path.lineTo (xs[(size_t) i], y);
        }
        return path;
    };

    for (int b = 0; b < rz::kMaxBands; ++b)
        if (model.isActive (b))
            bandCurves[(size_t) b] = makePath ([&model, b] (double f)
            {
                return 20.0 * std::log10 (std::max (std::abs (model.bandResponse (b, f)), 1.0e-9));
            });

    if (selected >= 0 && model.isActive (selected))
    {
        selectedFill = bandCurves[(size_t) selected];
        const float y0 = yForDb (0.0);
        selectedFill.lineTo (xs.back(), y0);
        selectedFill.lineTo (xs.front(), y0);
        selectedFill.closeSubPath();
    }

    // 전체 곡선: 스테레오 밴드만 있으면 하나, L/R · M/S 밴드가 있으면 채널마다
    const bool lr = model.usesPlacement (rz::Placement::Left) || model.usesPlacement (rz::Placement::Right);
    const bool ms = model.usesPlacement (rz::Placement::Mid)  || model.usesPlacement (rz::Placement::Side);
    std::vector<std::pair<rz::Placement, juce::String>> views;
    if (! lr && ! ms) views.push_back ({ rz::Placement::Stereo, {} });
    if (lr) { views.push_back ({ rz::Placement::Left, "LEFT" }); views.push_back ({ rz::Placement::Right, "RIGHT" }); }
    if (ms) { views.push_back ({ rz::Placement::Mid, "MID" });   views.push_back ({ rz::Placement::Side, "SIDE" }); }

    for (const auto& v : views)
    {
        const auto view = v.first;
        const bool dashed = lr && ms && rz::isMsPlacement (view);   // L/R와 M/S가 같이 있으면 M/S는 점선
        totals.push_back ({ makePath ([&model, view] (double f) { return model.magnitudeDb (f, view); }),
                            viewColour (view), v.second, dashed });
    }
}

//==============================================================================
//  분석기
//==============================================================================
void GraphView::ensureFft (double sessionRate)
{
    const int order = sessionRate <= 50000.0 ? 13 : (sessionRate <= 100000.0 ? 14 : 15);
    fftRate = sessionRate;
    if (order == fftOrder && fft != nullptr)
        return;

    fftOrder = order;
    fftSize  = 1 << order;
    fft = std::make_unique<juce::dsp::FFT> (order);

    window.resize ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

    fftData.assign ((size_t) fftSize * 2, 0.0f);
    for (auto& s : spectra)
    {
        s.ring.assign ((size_t) fftSize, 0.0f);
        s.writePos = 0;
    }
}

void GraphView::updateAnalyzer()
{
    const double sr = proc.getSessionRate();
    ensureFft (sr);

    const int N = fftSize, numBins = N / 2;
    const float norm = 4.0f / (float) N;          // Hann: 풀스케일 사인 = 0 dB
    const double binHz = sr / (double) N;
    const int numPoints = (int) pointFreq.size();

    for (int s = 0; s < 2; ++s)
    {
        auto& sp = spectra[(size_t) s];
        const int got = proc.getAnalyzerTap (s).pull (pullBuffer.data(), (int) pullBuffer.size());
        if (got > 0)
        {
            for (int k = juce::jmax (0, got - N); k < got; ++k)
            {
                sp.ring[(size_t) sp.writePos] = pullBuffer[(size_t) k];
                if (++sp.writePos >= N) sp.writePos = 0;
            }
            sp.silentTicks = 0;
            sp.fresh = true;
        }
        else
        {
            ++sp.silentTicks;
        }

        const bool wanted = s == 1 || analyzerMode == 2;
        if (! wanted || (int) sp.level.size() != numPoints)
            continue;

        if (sp.fresh)
        {
            sp.fresh = false;
            for (int i = 0; i < N; ++i)
                fftData[(size_t) i] = sp.ring[(size_t) ((sp.writePos + i) % N)] * window[(size_t) i];
            std::fill (fftData.begin() + N, fftData.end(), 0.0f);
            fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

            for (int i = 0; i < numPoints; ++i)
            {
                const double f = pointFreq[(size_t) i];
                float target = (float) kSpecFloorDb;
                if (f < 0.5 * sr)
                {
                    const double lo = i > 0 ? std::sqrt (f * pointFreq[(size_t) i - 1]) : f;
                    const double hi = i + 1 < numPoints ? std::sqrt (f * pointFreq[(size_t) i + 1]) : f;
                    float mag = 0.0f;
                    if ((hi - lo) / binHz < 1.0)
                    {
                        // 화면 한 점이 빈 하나보다 좁은 저역: 빈 사이 보간
                        const double k = f / binHz;
                        const int k0 = juce::jlimit (0, numBins - 1, (int) k);
                        const float frac = (float) juce::jlimit (0.0, 1.0, k - (double) k0);
                        mag = fftData[(size_t) k0] + frac * (fftData[(size_t) k0 + 1] - fftData[(size_t) k0]);
                    }
                    else
                    {
                        // 넓은 고역: 그 구간에서 가장 큰 빈 (피크가 묻히지 않게)
                        const int a = juce::jlimit (0, numBins, (int) std::ceil (lo / binHz));
                        const int b = juce::jlimit (0, numBins, (int) std::floor (hi / binHz));
                        for (int k = a; k <= b; ++k)
                            mag = std::max (mag, fftData[(size_t) k]);
                    }
                    // 4.5 dB/oct 기울기 (1 kHz 기준): 보통 음악이 대략 평평하게 보이도록
                    target = 20.0f * std::log10 (std::max (mag * norm, 1.0e-12f)) + 4.5f * (float) std::log2 (f / 1000.0);
                }
                float& lv = sp.level[(size_t) i];
                if (! std::isfinite (target)) target = (float) kSpecFloorDb;
                if (! std::isfinite (lv))     lv = (float) kSpecFloorDb;
                lv += (target - lv) * (target > lv ? 0.55f : 0.16f);
            }
        }
        else if (sp.silentTicks > 6)
        {
            for (auto& lv : sp.level)        // 호스트가 처리를 멈추면 천천히 내려감
                lv = std::max ((float) kSpecFloorDb, lv - 1.5f);
        }
    }

    rebuildSpectrumPaths();
}

void GraphView::rebuildSpectrumPaths()
{
    for (int s = 0; s < 2; ++s)
    {
        auto& sp = spectra[(size_t) s];
        sp.line.clear();
        sp.fill.clear();
        if (analyzerMode == 0 || (s == 0 && analyzerMode < 2) || sp.level.size() < 2 || fftRate <= 0.0)
            continue;

        const int n = (int) sp.level.size();
        const float bottom = plot.getBottom();
        bool started = false;
        float lastX = plot.getX();
        for (int i = 0; i < n; ++i)
        {
            if (pointFreq[(size_t) i] >= 0.5 * fftRate)
                break;
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
            const float y = juce::jlimit (plot.getY(), bottom, ySpec (sp.level[(size_t) i]));
            if (! started) { sp.line.startNewSubPath (x, y); started = true; }
            else           sp.line.lineTo (x, y);
            lastX = x;
        }
        if (started)
        {
            sp.fill = sp.line;
            sp.fill.lineTo (lastX, bottom);
            sp.fill.lineTo (plot.getX(), bottom);
            sp.fill.closeSubPath();
        }
    }
}

//==============================================================================
//  그리기
//==============================================================================
void GraphView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (palette::graphBg);
    g.fillRoundedRectangle (bounds, 10.0f);

    const double gridStep  = rangeDb / 4.0;
    const int    labelStep = rangeDb / 2;
    int numUsed = 0;
    for (const auto& b : params.bands) numUsed += b.used ? 1 : 0;

    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (bounds.reduced (1.0f), 9.0f);
        g.reduceClipRegion (clip);

        // 눈금: 주파수 (10단위), dB
        for (double decade : { 10.0, 100.0, 1000.0, 10000.0 })
            for (int m = 1; m <= 9; ++m)
            {
                const double f = decade * m;
                if (f < kMinHz || f > kMaxHz) continue;
                g.setColour (m == 1 ? palette::gridMajor : palette::grid.withAlpha (0.55f));
                g.drawVerticalLine (juce::roundToInt (xForFreq (f)), plot.getY(), plot.getBottom());
            }
        for (double db = -rangeDb; db <= rangeDb + 1.0e-6; db += gridStep)
        {
            g.setColour (std::abs (db) < 1.0e-6 ? palette::dim.withAlpha (0.4f) : palette::grid);
            g.drawHorizontalLine (juce::roundToInt (yForDb (db)), plot.getX(), plot.getRight());
        }

        // 스펙트럼
        if (analyzerMode > 0)
        {
            const auto& post = spectra[1];
            if (! post.fill.isEmpty())
            {
                g.setGradientFill (juce::ColourGradient (palette::accent2.withAlpha (0.20f), 0.0f, plot.getY(),
                                                         palette::accent2.withAlpha (0.04f), 0.0f, plot.getBottom(), false));
                g.fillPath (post.fill);
                g.setColour (palette::accent2.withAlpha (0.30f));
                g.strokePath (post.line, juce::PathStrokeType (1.0f));
            }
            if (analyzerMode == 2 && ! spectra[0].line.isEmpty())
            {
                g.setColour (palette::scale.withAlpha (0.28f));
                g.strokePath (spectra[0].line, juce::PathStrokeType (1.0f));
            }
        }

        // 축 글자
        g.setFont (uiFont (10.5f));
        g.setColour (palette::dim.withAlpha (0.85f));
        for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
            g.drawText (shortHz (f), juce::Rectangle<float> (xForFreq (f) - 20.0f, plot.getBottom() - 19.0f, 40.0f, 14.0f),
                        juce::Justification::centred, false);
        for (int db = -rangeDb; db <= rangeDb; db += labelStep)
            g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                        juce::Rectangle<float> (plot.getRight() - 40.0f, yForDb (db) - 7.0f, 34.0f, 14.0f),
                        juce::Justification::centredRight, false);

        // 고른 밴드: 그 밴드만의 곡선을 면으로
        if (selected >= 0 && ! selectedFill.isEmpty())
        {
            g.setColour (bandColour (selected).withAlpha (0.16f));
            g.fillPath (selectedFill);
        }

        // 밴드별 곡선 (얇게)
        for (int b = 0; b < pillow::rz::kMaxBands; ++b)
            if (! bandCurves[(size_t) b].isEmpty())
            {
                const bool sel = b == selected;
                g.setColour (bandColour (b).withAlpha (sel ? 0.85f : 0.32f));
                g.strokePath (bandCurves[(size_t) b], juce::PathStrokeType (sel ? 1.5f : 1.1f));
            }

        // 전체 곡선
        const float dashes[] = { 7.0f, 4.0f };
        for (auto it = totals.rbegin(); it != totals.rend(); ++it)
        {
            g.setColour (it->colour.withAlpha (params.eqOn ? 1.0f : 0.5f));
            const juce::PathStrokeType stroke (2.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            if (it->dashed)
            {
                juce::Path d;
                stroke.createDashedStroke (d, it->path, dashes, 2);
                g.fillPath (d);
            }
            else
            {
                g.strokePath (it->path, stroke);
            }
        }

        // 범례 (채널별 곡선일 때)
        if (totals.size() > 1)
        {
            float lx = plot.getX() + 12.0f;
            const float ly = plot.getBottom() - 40.0f;
            g.setFont (uiFont (10.5f, true, 0.08f));
            for (const auto& c : totals)
            {
                g.setColour (c.colour);
                if (c.dashed)
                {
                    g.fillRoundedRectangle (lx, ly + 5.0f, 5.0f, 4.0f, 2.0f);
                    g.fillRoundedRectangle (lx + 8.0f, ly + 5.0f, 6.0f, 4.0f, 2.0f);
                }
                else
                {
                    g.fillRoundedRectangle (lx, ly + 5.0f, 14.0f, 4.0f, 2.0f);
                }
                g.setColour (palette::dim);
                g.drawText (c.name, juce::Rectangle<float> (lx + 19.0f, ly, 60.0f, 14.0f), juce::Justification::centredLeft, false);
                lx += 76.0f;
            }
        }

        // 밴드 점 (고른 것 · 마우스가 올라간 것을 맨 위에)
        auto drawNode = [&] (int i)
        {
            const auto& b = params.bands[(size_t) i];
            const auto c = nodePosition (i);
            const bool sel = i == selected;
            const bool hot = i == hovered || i == dragging;
            const bool inert = ! b.on || ! params.eqOn || (channels < 2 && b.place == rz::Placement::Side);
            const float r = sel ? 10.0f : (hot ? 9.5f : 8.5f);
            const auto col = inert ? juce::Colour (0xff5b6672) : bandColour (i);
            const auto circle = juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (c);

            if (sel || hot)
            {
                g.setColour (col.withAlpha (sel ? 0.28f : 0.18f));
                g.fillEllipse (circle.expanded (5.0f));
            }
            g.setColour (col);
            g.fillEllipse (circle);
            g.setColour (sel ? palette::text : palette::graphBg);
            g.drawEllipse (circle, sel ? 2.0f : 1.5f);

            g.setColour (palette::graphBg);
            g.setFont (uiFont (10.5f, true));
            g.drawText (juce::String (i + 1), circle, juce::Justification::centred, false);

            // 이 밴드 부분을 튜브 · 트랜스포머로 보냄 (ALL이 켜져 있으면 이미 전체가 지나가므로 흐리게)
            const float dotY = c.y + r + 5.0f;
            auto dot = [&] (float x, juce::Colour dc, bool redundant)
            {
                g.setColour (palette::graphBg);
                g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x, dotY }));
                g.setColour (dc.withAlpha (redundant || inert ? 0.4f : 1.0f));
                g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ x, dotY }));
            };
            if (b.tube && b.xfmr)
            {
                dot (c.x - 4.0f, juce::Colour (0xffff8f4f), params.driveOn);
                dot (c.x + 4.0f, palette::accent2, params.xfAll);
            }
            else if (b.tube) dot (c.x, juce::Colour (0xffff8f4f), params.driveOn);
            else if (b.xfmr) dot (c.x, palette::accent2, params.xfAll);

            if (b.place != rz::Placement::Stereo)
            {
                const auto badge = juce::Rectangle<float> (13.0f, 13.0f).withCentre (c + juce::Point<float> (r * 0.95f, -r * 0.95f));
                g.setColour (palette::graphBg);
                g.fillEllipse (badge);
                g.setColour (col);
                g.drawEllipse (badge.reduced (0.5f), 1.0f);
                g.setFont (uiFont (8.5f, true));
                g.drawText (placeLetter (b.place), badge, juce::Justification::centred, false);
            }
        };

        for (int i = 0; i < pillow::rz::kMaxBands; ++i)
            if (params.bands[(size_t) i].used && i != selected && i != hovered && i != dragging)
                drawNode (i);
        for (int i : { dragging, hovered })
            if (i >= 0 && i != selected && params.bands[(size_t) i].used)
                drawNode (i);
        if (selected >= 0 && params.bands[(size_t) selected].used)
            drawNode (selected);

        if (numUsed == 0)
        {
            g.setColour (palette::dim.withAlpha (0.9f));
            g.setFont (uiFont (13.0f));
            g.drawText ("Double-click to add a band   /   right-click for shapes",
                        plot.withSizeKeepingCentre (plot.getWidth(), 24.0f).translated (0.0f, -40.0f),
                        juce::Justification::centred, false);
        }

        if (! params.eqOn)
        {
            g.setColour (palette::accent.withAlpha (0.9f));
            g.setFont (uiFont (13.0f, true, 0.2f));
            g.drawText ("EQ OFF", plot.withHeight (24.0f).translated (0.0f, 34.0f), juce::Justification::centred, false);
        }
    }

    // 왼쪽 위: 값 표시
    {
        juce::String text;
        auto col = palette::dim;
        const int shownBand = dragging >= 0 ? dragging : hovered;
        if (shownBand >= 0 && params.bands[(size_t) shownBand].used)
        {
            const auto& b = params.bands[(size_t) shownBand];
            text = juce::String (shownBand + 1) + "   " + pillow::bandTypeNames()[(int) b.type] + "   " + pillow::formatHz (b.freq);
            if (rz::typeHasGain (b.type))  text += "   " + pillow::formatDb (b.gain);
            if (rz::typeHasSlope (b.type)) text += "   " + pillow::slopeNames()[b.slope];
            if (rz::typeHasQ (b.type))     text += "   Q " + pillow::formatQ (b.q);
            if (b.place != rz::Placement::Stereo) text += "   " + pillow::placementNames()[(int) b.place].toUpperCase();
            if (b.tube) text += "   TUBE";
            if (b.xfmr) text += "   XFMR";
            if (! b.on) text += "   OFF";
            col = bandColour (shownBand);
        }
        else if (mouseInside && plot.contains (mousePos))
        {
            text = pillow::formatHz ((float) freqForX (mousePos.x)) + "   " + pillow::formatDb ((float) dbForY (mousePos.y));
        }
        g.setColour (col);
        g.setFont (uiFont (12.0f, true, 0.04f));
        g.drawText (text, juce::Rectangle<float> (plot.getX() + 12.0f, plot.getY() + 7.0f, analyzerButton.getX() - plot.getX() - 20.0f, 19.0f),
                    juce::Justification::centredLeft, true);
    }

    // 오른쪽 위: 분석기 · 범위 버튼
    auto pill = [&g] (juce::Rectangle<float> r, const juce::String& t, bool lit)
    {
        g.setColour (palette::bgDark.withAlpha (0.9f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (palette::panelEdge.withAlpha (0.8f));
        g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
        g.setColour (lit ? palette::text : palette::dim);
        g.setFont (uiFont (10.0f, true, 0.06f));
        g.drawText (t, r, juce::Justification::centred, false);
    };
    pill (analyzerButton, juce::String ("ANALYZER  ") + (analyzerMode == 0 ? "OFF" : (analyzerMode == 1 ? "POST" : "PRE + POST")),
          analyzerMode > 0);
    pill (rangeButton, "RANGE  " + juce::String (rangeDb) + " dB", true);

    g.setColour (palette::panelEdge.withAlpha (0.6f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, 1.0f);
}

//==============================================================================
//  마우스 · 키보드
//==============================================================================
int GraphView::bandAt (juce::Point<float> pos) const noexcept
{
    auto hit = [&] (int i) { return params.bands[(size_t) i].used && nodePosition (i).getDistanceFrom (pos) <= 11.0f; };
    if (selected >= 0 && hit (selected)) return selected;
    for (int i = pillow::rz::kMaxBands - 1; i >= 0; --i)
        if (hit (i)) return i;
    return -1;
}

void GraphView::select (int band)
{
    if (band == selected)
        return;
    selected = band;
    rebuildCurves();
    repaint();
    if (onSelect)
        onSelect (band);
}

void GraphView::setSelectedBand (int band)
{
    if (band == selected)
        return;
    selected = band;
    rebuildCurves();
    repaint();
}

void GraphView::mouseMove (const juce::MouseEvent& e)
{
    mousePos = e.position;
    mouseInside = true;
    hovered = bandAt (e.position);
    const bool overButton = analyzerButton.contains (e.position) || rangeButton.contains (e.position);
    setMouseCursor (hovered >= 0 ? juce::MouseCursor::DraggingHandCursor
                                 : (overButton ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
    repaint();
}

void GraphView::mouseExit (const juce::MouseEvent&)
{
    mouseInside = false;
    hovered = -1;
    repaint();
}

void GraphView::mouseDown (const juce::MouseEvent& e)
{
    endDrag();   // 다른 포인터로 드래그 중이었으면 제스처를 먼저 닫음 (시작 · 끝 짝 맞추기)
    mousePos = e.position;
    if (analyzerButton.contains (e.position)) { cycleAnalyzer(); return; }
    if (rangeButton.contains (e.position))    { cycleRange();    return; }

    const int b = bandAt (e.position);
    if (e.mods.isPopupMenu())
    {
        if (b >= 0) { select (b); showBandMenu (b); }
        else        showAddMenu (e.position);
        return;
    }
    if (b < 0)
    {
        select (-1);
        return;
    }

    select (b);
    const auto& bp = params.bands[(size_t) b];

    if (e.mods.isAltDown())   // Alt + 클릭: 켜기 / 끄기
    {
        setParameter (bandParam (proc, b, pillow::ids::on), bp.on ? 0.0f : 1.0f);
        refreshNow();
        return;
    }

    // 드래그 시작: 점의 실제 위치(화면 밖으로 잘리기 전)에서 움직인 양만큼 따라감
    dragging      = b;
    dragSource    = e.source.getIndex();
    dragStartNode = { xForFreq (bp.freq), rz::typeHasGain (bp.type) ? yForDb (bp.gain) : yForDb (0.0) };
    dragVirtual   = dragStartNode;
    dragLast      = e.position;
    dragStartQ    = bp.q;
    // 위아래 = 게인. 게인이 없는 모양(컷 · 노치)이나 Ctrl(Cmd)을 누른 채면 위아래 = Q
    dragYIsQ      = rz::typeHasQ (bp.type) && (! rz::typeHasGain (bp.type) || e.mods.isCommandDown());
    dragFreqParam = bandParam (proc, b, pillow::ids::freq);
    dragYParam    = dragYIsQ ? bandParam (proc, b, pillow::ids::q)
                             : (rz::typeHasGain (bp.type) ? bandParam (proc, b, pillow::ids::gain) : nullptr);
    if (dragFreqParam != nullptr) dragFreqParam->beginChangeGesture();
    if (dragYParam != nullptr)    dragYParam->beginChangeGesture();
}

void GraphView::mouseDrag (const juce::MouseEvent& e)
{
    mousePos = e.position;
    if (dragging < 0 || e.source.getIndex() != dragSource)
        return;

    const float k = e.mods.isShiftDown() ? 0.1f : 1.0f;   // Shift: 10배 미세
    dragVirtual += (e.position - dragLast) * k;
    dragLast = e.position;

    if (dragFreqParam != nullptr)
    {
        const auto f = (float) juce::jlimit (kMinHz, kMaxHz, freqForX (dragVirtual.x));
        dragFreqParam->setValueNotifyingHost (dragFreqParam->convertTo0to1 (f));
    }
    if (dragYParam != nullptr)
    {
        const float v = dragYIsQ
            ? juce::jlimit (pillow::rz::kQMin, pillow::rz::kQMax, dragStartQ * std::pow (2.0f, (dragStartNode.y - dragVirtual.y) / 60.0f))
            : juce::jlimit (-pillow::rz::kGainMax, pillow::rz::kGainMax, (float) dbForY (dragVirtual.y));
        dragYParam->setValueNotifyingHost (dragYParam->convertTo0to1 (v));
    }
    refreshNow();
}

void GraphView::mouseUp (const juce::MouseEvent& e)
{
    if (dragging >= 0 && e.source.getIndex() != dragSource)
        return;
    endDrag();
}

void GraphView::endDrag()
{
    if (dragging < 0)
        return;
    if (dragFreqParam != nullptr) dragFreqParam->endChangeGesture();
    if (dragYParam != nullptr)    dragYParam->endChangeGesture();
    dragFreqParam = nullptr;
    dragYParam = nullptr;
    dragging = -1;
    dragSource = -1;
    repaint();
}

void GraphView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (analyzerButton.contains (e.position) || rangeButton.contains (e.position) || bandAt (e.position) >= 0
        || e.mods.isPopupMenu())
        return;

    const auto f = (float) juce::jlimit (kMinHz, kMaxHz, freqForX (e.position.x));
    const float gdb = juce::jlimit (-pillow::rz::kGainMax, pillow::rz::kGainMax, (float) dbForY (e.position.y));
    const int idx = addBand (proc, rz::BandType::Bell, f, std::round (gdb * 10.0f) / 10.0f);
    if (idx >= 0)
    {
        refreshNow();
        select (idx);
    }
}

void GraphView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (dragging >= 0)
        return;   // 드래그 중 (Ctrl 드래그로 Q를 잡고 있을 수도 있음)

    const int b = hovered >= 0 ? hovered : selected;
    if (b < 0 || ! params.bands[(size_t) b].used || ! rz::typeHasQ (params.bands[(size_t) b].type))
    {
        juce::Component::mouseWheelMove (e, w);
        return;
    }

    float d = std::abs (w.deltaY) >= std::abs (w.deltaX) ? w.deltaY : w.deltaX;
    if (w.isReversed)
        d = -d;
    const float sens = e.mods.isShiftDown() ? 0.12f : 1.2f;
    const float q = juce::jlimit (pillow::rz::kQMin, pillow::rz::kQMax, params.bands[(size_t) b].q * std::pow (2.0f, d * sens));
    setParameter (bandParam (proc, b, pillow::ids::q), q);
    refreshNow();
}

bool GraphView::keyPressed (const juce::KeyPress& key)
{
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && selected >= 0)
    {
        const int b = selected;
        select (-1);
        removeBand (proc, b);
        refreshNow();
        return true;
    }
    if (key == juce::KeyPress::escapeKey && selected >= 0)
    {
        select (-1);
        return true;
    }
    return false;
}

//==============================================================================
void GraphView::showBandMenu (int band)
{
    const auto& bp = params.bands[(size_t) band];
    juce::PopupMenu menu, shapes, slopes, places;
    menu.setLookAndFeel (&getLookAndFeel());
    menu.addSectionHeader ("BAND " + juce::String (band + 1));

    const auto typeNames = pillow::bandTypeNames();
    for (int t = 0; t < rz::kNumBandTypes; ++t)
        shapes.addItem (100 + t, typeNames[t], true, (int) bp.type == t);
    menu.addSubMenu ("Shape", shapes);

    if (rz::typeHasSlope (bp.type))
    {
        const auto sn = pillow::slopeNames();
        for (int s = 0; s < sn.size(); ++s)
            slopes.addItem (200 + s, sn[s], true, bp.slope == s);
        menu.addSubMenu ("Slope", slopes);
    }

    const auto pn = pillow::placementNames();
    for (int p = 0; p < pn.size(); ++p)
        places.addItem (300 + p, pn[p], true, (int) bp.place == p);
    menu.addSubMenu ("Placement", places);

    menu.addSeparator();
    menu.addItem (1, "Band On", true, bp.on);
    menu.addItem (4, "Tube (EF804S) on this band", true, bp.tube);
    menu.addItem (5, "Transformer on this band", true, bp.xfmr);
    if (rz::typeHasGain (bp.type))
        menu.addItem (2, "Reset Gain");
    menu.addItem (3, "Delete Band");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe = juce::Component::SafePointer<GraphView> (this), band] (int result)
                        {
                            if (safe != nullptr && result != 0)
                                safe->handleBandMenu (band, result);
                        });
}

void GraphView::handleBandMenu (int band, int r)
{
    if (! params.bands[(size_t) band].used)
        return;

    if (r >= 100 && r < 100 + rz::kNumBandTypes)        setParameter (bandParam (proc, band, pillow::ids::type),  (float) (r - 100));
    else if (r >= 200 && r < 200 + (int) rz::kCutSlopes.size()) setParameter (bandParam (proc, band, pillow::ids::slope), (float) (r - 200));
    else if (r >= 300 && r < 300 + rz::kNumPlacements)  setParameter (bandParam (proc, band, pillow::ids::place), (float) (r - 300));
    else if (r == 1) setParameter (bandParam (proc, band, pillow::ids::on), params.bands[(size_t) band].on ? 0.0f : 1.0f);
    else if (r == 2) setParameter (bandParam (proc, band, pillow::ids::gain), 0.0f);
    else if (r == 4) setParameter (bandParam (proc, band, pillow::ids::tube), params.bands[(size_t) band].tube ? 0.0f : 1.0f);
    else if (r == 5) setParameter (bandParam (proc, band, pillow::ids::xfmr), params.bands[(size_t) band].xfmr ? 0.0f : 1.0f);
    else if (r == 3)
    {
        if (selected == band)
            select (-1);
        removeBand (proc, band);
    }
    refreshNow();
}

void GraphView::showAddMenu (juce::Point<float> pos)
{
    const auto f = (float) juce::jlimit (kMinHz, kMaxHz, freqForX (pos.x));
    const float gdb = std::round (juce::jlimit (-pillow::rz::kGainMax, pillow::rz::kGainMax, (float) dbForY (pos.y)) * 10.0f) / 10.0f;

    bool full = true;
    for (const auto& b : params.bands)
        if (! b.used) { full = false; break; }

    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());
    menu.addSectionHeader (full ? juce::String ("ALL 16 BANDS IN USE") : "ADD BAND AT " + pillow::formatHz (f));
    const auto typeNames = pillow::bandTypeNames();
    for (int t = 0; t < rz::kNumBandTypes; ++t)
        menu.addItem (100 + t, typeNames[t], ! full);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe = juce::Component::SafePointer<GraphView> (this), f, gdb] (int result)
                        {
                            if (safe == nullptr || result < 100)
                                return;
                            const auto type = (rz::BandType) (result - 100);
                            const int idx = addBand (safe->proc, type, f, rz::typeHasGain (type) ? gdb : 0.0f);
                            if (idx >= 0)
                            {
                                safe->refreshNow();
                                safe->select (idx);
                            }
                        });
}

void GraphView::applyUiSettings (int range, int analyzer)
{
    if (range != 6 && range != 12 && range != 24)
        range = rangeDb;
    analyzer = juce::jlimit (0, 2, analyzer);

    if (range != rangeDb)
    {
        rangeDb = range;
        rebuildCurves();
        repaint();
    }
    if (analyzer != analyzerMode)
    {
        analyzerMode = analyzer;
        proc.setAnalyzerActive (analyzerMode > 0);
        for (auto& s : spectra)
            std::fill (s.level.begin(), s.level.end(), (float) kSpecFloorDb);
        rebuildSpectrumPaths();
        repaint();
    }
}

void GraphView::cycleRange()
{
    const int next = rangeDb == 12 ? 24 : (rangeDb == 24 ? 6 : 12);
    proc.apvts.state.setProperty (pillow::ids::uiRange, next, nullptr);
    applyUiSettings (next, analyzerMode);
}

void GraphView::cycleAnalyzer()
{
    const int next = analyzerMode == 2 ? 1 : (analyzerMode == 1 ? 0 : 2);
    proc.apvts.state.setProperty (pillow::ids::uiAnalyzer, next, nullptr);
    applyUiSettings (rangeDb, next);
}
} // namespace pillow::ui
