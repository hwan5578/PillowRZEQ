#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace rz = pillow::rz;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout PillowRZAudioProcessor::createParameterLayout()
{
    using FloatParam = juce::AudioParameterFloat;
    using FloatAttr  = juce::AudioParameterFloatAttributes;
    using Range      = juce::NormalisableRange<float>;
    using pillow::ids::forModule;

    auto dbAttr = []
    {
        return FloatAttr()
            .withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return pillow::formatDb (v); })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); });
    };

    auto driveAttr = []
    {
        return FloatAttr()
            .withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); });
    };

    auto qAttr = []
    {
        return FloatAttr()
            .withStringFromValueFunction ([] (float v, int) { return pillow::formatQ (v); })
            .withValueFromStringFunction ([] (const juce::String& s)
                                          { return s.retainCharacters ("0123456789.").getFloatValue(); });
    };

    auto percentAttr = []
    {
        return FloatAttr()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); });
    };

    auto focusAttr = []
    {
        return FloatAttr()
            .withStringFromValueFunction ([] (float v, int)
            {
                const int r = juce::roundToInt (v);
                if (r == 0) return juce::String ("Neutral");
                return (r < 0 ? juce::String ("Odd ") : juce::String ("Even ")) + juce::String (std::abs (r)) + " %";
            })
            .withValueFromStringFunction ([] (const juce::String& s)
            {
                const float n = s.retainCharacters ("0123456789.").getFloatValue();
                return s.containsIgnoreCase ("odd") || s.trimStart().startsWithChar ('-') ? -n : n;
            });
    };

    Range shelfQRange (rz::kShelfQMin, rz::kShelfQMax, 0.001f);
    shelfQRange.setSkewForCentre (0.6f);      // 빈티지 0.5가 약 40% 위치

    Range presQRange (rz::kPresQMin, rz::kPresQMax, 0.001f);
    presQRange.setSkewForCentre (1.0f);       // 기본 1.0이 정중앙

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // ── 전역 ────────────────────────────────────────────────────────────────
    // 레이턴시가 바뀌는 설정이라 자동화는 막아 둠 (재생 중 전환 자체는 페이드로 안전)
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { pillow::ids::quality, 3 }, "Quality",
        juce::StringArray { "Zero Latency", "HQ (Oversampled)" }, 0,
        juce::AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { pillow::ids::routing, 2 }, "Routing",
        juce::StringArray { "Stereo (L/R)", "Mid / Side" }, 0));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::link, 2 }, "Link", true));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::steps, 1 }, "Vintage Steps", false));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::eqOn, 2 }, "EQ On", true));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::driveOn, 2 }, "Drive On", false));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::focus, 2 }, "Focus",
        Range (-100.0f, 100.0f, 0.1f), 0.0f, focusAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::mix, 2 }, "Mix",
        Range (0.0f, 100.0f, 0.1f), 100.0f, percentAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::xfLow, 2 }, "Transformer Low",
        Range (0.0f, 100.0f, 0.1f), 0.0f, percentAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::xfMid, 2 }, "Transformer Mid",
        Range (0.0f, 100.0f, 0.1f), 0.0f, percentAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::xfHigh, 2 }, "Transformer High",
        Range (0.0f, 100.0f, 0.1f), 0.0f, percentAttr()));

    // ── 모듈 1, 2 ───────────────────────────────────────────────────────────
    for (int m = 0; m < 2; ++m)
    {
        const int v = (m == 0) ? 1 : 2;   // 모듈 1의 기존 ID는 v0.1부터 있던 것
        const juce::String n = juce::String (m + 1) + " ";

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { forModule (pillow::ids::mode, m), v }, n + "Type",
            juce::StringArray { "A (Pivot)", "B (Presence)" }, 1));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::treble, m), v }, n + "Treble",
            Range (rz::kTrebleMinDb, rz::kTrebleMaxDb, 0.01f), 0.0f, dbAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::trebleQ, m), v }, n + "Treble Q",
            shelfQRange, rz::kVintageShelfQ, qAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::bass, m), v }, n + "Bass",
            Range (rz::kBassMinDb, rz::kBassMaxDb, 0.01f), 0.0f, dbAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::bassQ, m), v }, n + "Bass Q",
            shelfQRange, rz::kVintageShelfQ, qAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::tilt, m), v }, n + "Pivot",
            Range (-rz::kTiltMaxDb, rz::kTiltMaxDb, 0.01f), 0.0f, dbAttr()));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { forModule (pillow::ids::presFreq, m), v }, n + "Presence Freq",
            juce::StringArray { "1.4 kHz", "2 kHz", "2.8 kHz", "4 kHz" }, 2));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::presGain, m), v }, n + "Presence",
            Range (0.0f, rz::kPresMaxDb, 0.01f), 0.0f, dbAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::presQ, m), v }, n + "Presence Q",
            presQRange, rz::kVintagePresQ, qAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::drive, m), 2 }, n + "Drive",
            Range (0.0f, rz::kDriveMaxDb, 0.01f), 0.0f, driveAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { forModule (pillow::ids::output, m), v }, n + "Level",
            Range (-18.0f, 18.0f, 0.01f), 0.0f, dbAttr()));
    }

    return layout;
}

//==============================================================================
PillowRZAudioProcessor::PillowRZAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    paramQuality = apvts.getRawParameterValue (pillow::ids::quality);
    paramRouting = apvts.getRawParameterValue (pillow::ids::routing);
    paramLink    = apvts.getRawParameterValue (pillow::ids::link);
    paramSteps   = apvts.getRawParameterValue (pillow::ids::steps);
    paramEqOn    = apvts.getRawParameterValue (pillow::ids::eqOn);
    paramDriveOn = apvts.getRawParameterValue (pillow::ids::driveOn);
    paramFocus   = apvts.getRawParameterValue (pillow::ids::focus);
    paramMix     = apvts.getRawParameterValue (pillow::ids::mix);
    paramXfLow   = apvts.getRawParameterValue (pillow::ids::xfLow);
    paramXfMid   = apvts.getRawParameterValue (pillow::ids::xfMid);
    paramXfHigh  = apvts.getRawParameterValue (pillow::ids::xfHigh);

    for (int m = 0; m < 2; ++m)
    {
        auto get = [this, m] (const char* base) { return apvts.getRawParameterValue (pillow::ids::forModule (base, m)); };
        auto& p    = modulePtrs[(size_t) m];
        p.mode     = get (pillow::ids::mode);
        p.treble   = get (pillow::ids::treble);
        p.trebleQ  = get (pillow::ids::trebleQ);
        p.bass     = get (pillow::ids::bass);
        p.bassQ    = get (pillow::ids::bassQ);
        p.tilt     = get (pillow::ids::tilt);
        p.presFreq = get (pillow::ids::presFreq);
        p.presGain = get (pillow::ids::presGain);
        p.presQ    = get (pillow::ids::presQ);
        p.drive    = get (pillow::ids::drive);
        p.output   = get (pillow::ids::output);
        jassert (p.mode != nullptr && p.treble != nullptr && p.trebleQ != nullptr && p.bass != nullptr
                 && p.bassQ != nullptr && p.tilt != nullptr && p.presFreq != nullptr && p.presGain != nullptr
                 && p.presQ != nullptr && p.drive != nullptr && p.output != nullptr);
    }

    jassert (paramQuality != nullptr && paramRouting != nullptr && paramLink != nullptr && paramSteps != nullptr
             && paramEqOn != nullptr && paramDriveOn != nullptr && paramFocus != nullptr && paramMix != nullptr
             && paramXfLow != nullptr && paramXfMid != nullptr && paramXfHigh != nullptr);

    // 모드를 바꾸면 오디오 스레드는 플래그만 세우고, 호스트 알림(setLatencySamples)은 여기 타이머에서
    startTimer (40);
}

PillowRZAudioProcessor::~PillowRZAudioProcessor()
{
    stopTimer();
}

//==============================================================================
rz::Params PillowRZAudioProcessor::readParams() const noexcept
{
    rz::Params p;
    p.routing = paramRouting->load() >= 0.5f ? rz::Routing::MidSide : rz::Routing::Stereo;
    p.link    = paramLink->load() >= 0.5f;
    p.stepped = paramSteps->load() >= 0.5f;
    p.eqOn    = paramEqOn->load() >= 0.5f;
    p.driveOn = paramDriveOn->load() >= 0.5f;
    p.focus   = paramFocus->load() * 0.01f;
    p.mix     = paramMix->load() * 0.01f;
    p.xfLow   = paramXfLow->load() * 0.01f;
    p.xfMid   = paramXfMid->load() * 0.01f;
    p.xfHigh  = paramXfHigh->load() * 0.01f;

    for (size_t m = 0; m < 2; ++m)
    {
        const auto& ptr = modulePtrs[m];
        auto& mp = p.mod[m];
        mp.mode      = ptr.mode->load() >= 0.5f ? rz::Mode::B_Presence : rz::Mode::A_Tilt;
        mp.trebleDb  = ptr.treble->load();
        mp.trebleQ   = ptr.trebleQ->load();
        mp.bassDb    = ptr.bass->load();
        mp.bassQ     = ptr.bassQ->load();
        mp.tiltDb    = ptr.tilt->load();
        mp.presIndex = juce::jlimit (0, 3, (int) std::lround (ptr.presFreq->load()));
        mp.presDb    = ptr.presGain->load();
        mp.presQ     = ptr.presQ->load();
        mp.driveDb   = ptr.drive->load();
        mp.outputDb  = ptr.output->load();
    }
    return p;
}

//==============================================================================
void PillowRZAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepared = false;
    baseRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    hqOrder  = rz::oversamplingOrderFor (baseRate);

    numProcessChannels = juce::jlimit (1, 2, getTotalNumOutputChannels());
    numChannelsInUse.store (numProcessChannels);
    maxBlockSize = juce::jmax (1, samplesPerBlock);

    // HQ: IIR 하프밴드 오버샘플링 (지연이 짧고 아날로그 같은 위상)
    if (hqOrder > 0)
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            (size_t) numProcessChannels, (size_t) hqOrder,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            true,    // 최고 품질
            true);   // 정수 레이턴시 (호스트 지연 보상이 정확해짐)
        oversampler->initProcessing ((size_t) maxBlockSize);   // 내부에서 reset 포함
    }
    else
    {
        oversampler.reset();
    }

    // 두 경로 모두 미리 준비 (전환할 때 오디오 스레드에서 메모리 할당이 없도록)
    const auto params = readParams();
    engineZero.prepare (baseRate, numProcessChannels, maxBlockSize);
    engineHq.prepare (baseRate * (double) (1 << hqOrder), numProcessChannels, maxBlockSize << hqOrder);
    for (auto* e : { &engineZero, &engineHq })
    {
        e->setParams (params);
        e->reset();
    }

    hq = wantsHq();
    fade = Fade::None;
    fadeGain = 1.0f;
    fadeStep = (float) (1.0 / (0.005 * baseRate));   // 5 ms
    processingRate.store (hq ? baseRate * (double) (1 << hqOrder) : baseRate);

    const int latency = latencyFor (hq);
    pendingLatency.store (latency);
    latencyDirty.store (false);
    setLatencySamples (latency);
    prepared = true;
}

int PillowRZAudioProcessor::latencyFor (bool useHq) const noexcept
{
    return (useHq && oversampler != nullptr) ? (int) std::lround (oversampler->getLatencyInSamples()) : 0;
}

bool PillowRZAudioProcessor::wantsHq() const noexcept
{
    return paramQuality->load() >= 0.5f && oversampler != nullptr;
}

void PillowRZAudioProcessor::timerCallback()
{
    if (latencyDirty.exchange (false))
        setLatencySamples (pendingLatency.load());
}

void PillowRZAudioProcessor::resetActivePath() noexcept
{
    (hq ? engineHq : engineZero).reset();    // 오래된 필터 상태가 튀어나오지 않게
    if (hq && oversampler != nullptr)
        oversampler->reset();
}

// 경로 교체 (오디오 스레드, 소리가 0인 순간에만 호출). 메모리 할당·락 없음
void PillowRZAudioProcessor::switchPath (bool toHq) noexcept
{
    hq = toHq;
    (hq ? engineHq : engineZero).setParams (readParams());
    resetActivePath();

    processingRate.store (hq ? baseRate * (double) (1 << hqOrder) : baseRate);
    pendingLatency.store (latencyFor (hq));
    latencyDirty.store (true);
}

bool PillowRZAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void PillowRZAudioProcessor::runActivePath (juce::dsp::AudioBlock<float>& block, int numCh) noexcept
{
    std::array<float*, 2> ptrs {};

    if (hq && oversampler != nullptr)
    {
        auto osBlock = oversampler->processSamplesUp (block);
        for (int c = 0; c < numCh; ++c)
            ptrs[(size_t) c] = osBlock.getChannelPointer ((size_t) c);
        engineHq.process (ptrs.data(), numCh, (int) osBlock.getNumSamples());
        oversampler->processSamplesDown (block);
    }
    else
    {
        for (int c = 0; c < numCh; ++c)
            ptrs[(size_t) c] = block.getChannelPointer ((size_t) c);
        (hq ? engineHq : engineZero).process (ptrs.data(), numCh, (int) block.getNumSamples());
    }
}

void PillowRZAudioProcessor::applyFade (juce::dsp::AudioBlock<float>& block, int numCh) noexcept
{
    if (fade == Fade::None)
        return;

    const int n = (int) block.getNumSamples();
    for (int i = 0; i < n; ++i)
    {
        if (fade == Fade::Out)      fadeGain = juce::jmax (0.0f, fadeGain - fadeStep);
        else if (fade == Fade::In)  fadeGain = juce::jmin (1.0f, fadeGain + fadeStep);

        for (int c = 0; c < numCh; ++c)
            block.getChannelPointer ((size_t) c)[i] *= fadeGain;
    }

    if (fade == Fade::In && fadeGain >= 1.0f)
        fade = Fade::None;
}

void PillowRZAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (! prepared || numSamples == 0)
        return;

    const int numCh = juce::jmin (numProcessChannels, buffer.getNumChannels());
    if (numCh <= 0)
        return;

    // 모드 전환 요청
    const bool wantHq = wantsHq();
    if (wantHq != hq)
        fade = Fade::Out;                    // 페이드인 도중이면 지금 크기에서 다시 내려감
    else if (fade == Fade::Out)
        fade = Fade::In;                     // 페이드아웃 도중에 원래대로 돌려놓으면 바로 복귀

    (hq ? engineHq : engineZero).setParams (readParams());

    juce::dsp::AudioBlock<float> fullBlock (buffer.getArrayOfWritePointers(),
                                            (size_t) numCh, (size_t) numSamples);

    // 호스트가 prepareToPlay 때보다 큰 블록을 보내도 안전하도록 잘라서 처리.
    // 페이드아웃이 끝나는 샘플에서 잘라 바로 새 경로로 넘어감 (블록 크기와 상관없이 전환 공백 10 ms)
    int pos = 0;
    while (pos < numSamples)
    {
        int len = juce::jmin (maxBlockSize, numSamples - pos);
        if (fade == Fade::Out)
            len = juce::jmin (len, juce::jmax (1, (int) std::ceil (fadeGain / fadeStep)));

        auto block = fullBlock.getSubBlock ((size_t) pos, (size_t) len);
        runActivePath (block, numCh);
        applyFade (block, numCh);
        pos += len;

        if (fade == Fade::Out && fadeGain <= 0.0f)
        {
            if (wantHq != hq)
                switchPath (wantHq);
            fade = Fade::In;
        }
    }

    // NaN/Inf가 나오면(입력에 섞여 들어온 경우 등) 이 블록은 무음, 상태는 초기화 -> 다음 블록부터 정상
    double energy = 0.0;
    for (int c = 0; c < numCh; ++c)
    {
        const float* d = buffer.getReadPointer (c);
        for (int i = 0; i < numSamples; ++i)
            energy += (double) d[i] * (double) d[i];
    }
    if (! std::isfinite (energy))
    {
        buffer.clear();
        resetActivePath();
    }
}

void PillowRZAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (! prepared || numSamples == 0)
        return;

    // 바이패스 중에 모드를 바꾸면 바로 적용 (레이턴시도 바로 맞춤)
    const bool wantHq = wantsHq();
    if (wantHq != hq)
        switchPath (wantHq);
    fade = Fade::None;
    fadeGain = 1.0f;

    if (! hq || oversampler == nullptr)
        return;   // ZERO LATENCY: 원신호 그대로

    const int numCh = juce::jmin (numProcessChannels, buffer.getNumChannels());
    if (numCh <= 0)
        return;

    juce::dsp::AudioBlock<float> fullBlock (buffer.getArrayOfWritePointers(),
                                            (size_t) numCh, (size_t) numSamples);

    // HQ: EQ/튜브는 건너뛰고 같은 오버샘플링 필터만 통과 -> 처리할 때와 똑같은 지연
    for (int start = 0; start < numSamples; start += maxBlockSize)
    {
        const int len = juce::jmin (maxBlockSize, numSamples - start);
        auto block = fullBlock.getSubBlock ((size_t) start, (size_t) len);
        oversampler->processSamplesUp (block);
        oversampler->processSamplesDown (block);
    }
}

//==============================================================================
void PillowRZAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void PillowRZAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);

            // v0.2 이하 세션에는 quality 항목이 없음 -> 지금 상태와 상관없이 항상 ZERO LATENCY로 열리게
            if (! tree.getChildWithProperty ("id", pillow::ids::quality).isValid())
                tree.appendChild (juce::ValueTree ("PARAM", { { "id", pillow::ids::quality }, { "value", 0.0f } }), nullptr);

            apvts.replaceState (tree);
        }
}

juce::AudioProcessorEditor* PillowRZAudioProcessor::createEditor()
{
    return new PillowRZAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PillowRZAudioProcessor();
}
