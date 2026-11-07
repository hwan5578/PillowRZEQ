#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace rz = pillow::rz;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout PillowRZAudioProcessor::createParameterLayout()
{
    using FloatParam = juce::AudioParameterFloat;
    using FloatAttr  = juce::AudioParameterFloatAttributes;
    using Range      = juce::NormalisableRange<float>;
    using pillow::ids::band;

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

    auto hzAttr = []
    {
        return FloatAttr()
            .withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return pillow::formatHz (v); })
            .withValueFromStringFunction ([] (const juce::String& s) { return pillow::parseHz (s); });
    };

    // "24", "24 dB", "24 dB/oct" 모두 받음 -> 가장 가까운 단계
    auto slopeAttr = []
    {
        return juce::AudioParameterChoiceAttributes().withValueFromStringFunction ([] (const juce::String& s)
        {
            const int v = s.retainCharacters ("0123456789").getIntValue();
            int best = rz::kDefaultSlope, bestDist = std::numeric_limits<int>::max();
            for (size_t k = 0; k < rz::kCutSlopes.size(); ++k)
            {
                const int d = std::abs (rz::kCutSlopes[k] - v);
                if (d < bestDist) { bestDist = d; best = (int) k; }
            }
            return best;
        });
    };

    // 노브/자동화 위치 = 로그 (주파수는 옥타브, Q는 배수 단위로 고르게)
    auto logRange = [] (float lo, float hi)
    {
        return Range (lo, hi,
                      [] (float start, float end, float t) { return start * std::pow (end / start, t); },
                      [] (float start, float end, float v) { return std::log (std::max (v, start) / start) / std::log (end / start); });
    };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // ── 전역 ────────────────────────────────────────────────────────────────
    // 레이턴시가 바뀌는 설정이라 자동화는 막아 둠 (재생 중 전환 자체는 페이드로 안전)
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { pillow::ids::quality, 3 }, "Quality",
        juce::StringArray { "Zero Latency", "HQ (Oversampled)" }, 0,
        juce::AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::eqOn, 2 }, "EQ On", true));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::driveOn, 2 }, "Tube All", false));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::xfAll, 6 }, "Transformer All", true));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::drive, 2 }, "Drive",
        Range (0.0f, rz::kDriveMaxDb, 0.01f), 0.0f, driveAttr()));

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

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::output, 1 }, "Output",
        Range (-18.0f, 18.0f, 0.01f), 0.0f, dbAttr()));

    // ── 밴드 16개 ───────────────────────────────────────────────────────────
    for (int i = 0; i < rz::kMaxBands; ++i)
    {
        const juce::String n = "Band " + juce::String (i + 1) + " ";

        // 밴드가 있음/없음: 화면에서 추가·삭제할 때만 바뀜 (자동화 쓰기 중에 점이 생겼다 사라지지 않게)
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { band (i, pillow::ids::used), 5 }, n + "Used", false,
            juce::AudioParameterBoolAttributes().withAutomatable (false)));

        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { band (i, pillow::ids::on), 5 }, n + "On", true));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { band (i, pillow::ids::type), 5 }, n + "Shape",
            pillow::bandTypeNames(), (int) rz::BandType::Bell));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { band (i, pillow::ids::freq), 5 }, n + "Freq",
            logRange (rz::kFreqMin, rz::kFreqMax), 1000.0f, hzAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { band (i, pillow::ids::gain), 5 }, n + "Gain",
            Range (-rz::kGainMax, rz::kGainMax, 0.01f), 0.0f, dbAttr()));

        layout.add (std::make_unique<FloatParam> (
            juce::ParameterID { band (i, pillow::ids::q), 5 }, n + "Q",
            logRange (rz::kQMin, rz::kQMax), 1.0f, qAttr()));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { band (i, pillow::ids::slope), 5 }, n + "Slope",
            pillow::slopeNames(), rz::kDefaultSlope, slopeAttr()));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { band (i, pillow::ids::place), 5 }, n + "Placement",
            pillow::placementNames(), (int) rz::Placement::Stereo));

        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { band (i, pillow::ids::tube), 6 }, n + "Tube", false));

        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { band (i, pillow::ids::xfmr), 6 }, n + "Transformer", false));
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
    paramEqOn    = apvts.getRawParameterValue (pillow::ids::eqOn);
    paramDriveOn = apvts.getRawParameterValue (pillow::ids::driveOn);
    paramDrive   = apvts.getRawParameterValue (pillow::ids::drive);
    paramFocus   = apvts.getRawParameterValue (pillow::ids::focus);
    paramMix     = apvts.getRawParameterValue (pillow::ids::mix);
    paramXfLow   = apvts.getRawParameterValue (pillow::ids::xfLow);
    paramXfMid   = apvts.getRawParameterValue (pillow::ids::xfMid);
    paramXfHigh  = apvts.getRawParameterValue (pillow::ids::xfHigh);
    paramOutput  = apvts.getRawParameterValue (pillow::ids::output);
    paramXfAll   = apvts.getRawParameterValue (pillow::ids::xfAll);

    jassert (paramQuality != nullptr && paramEqOn != nullptr && paramDriveOn != nullptr && paramDrive != nullptr
             && paramFocus != nullptr && paramMix != nullptr && paramXfLow != nullptr && paramXfMid != nullptr
             && paramXfHigh != nullptr && paramOutput != nullptr && paramXfAll != nullptr);

    for (int i = 0; i < rz::kMaxBands; ++i)
    {
        auto get = [this, i] (const char* field) { return apvts.getRawParameterValue (pillow::ids::band (i, field)); };
        auto& b = bandPtrs[(size_t) i];
        b.used  = get (pillow::ids::used);
        b.on    = get (pillow::ids::on);
        b.type  = get (pillow::ids::type);
        b.freq  = get (pillow::ids::freq);
        b.gain  = get (pillow::ids::gain);
        b.q     = get (pillow::ids::q);
        b.slope = get (pillow::ids::slope);
        b.place = get (pillow::ids::place);
        b.tube  = get (pillow::ids::tube);
        b.xfmr  = get (pillow::ids::xfmr);
        jassert (b.used != nullptr && b.on != nullptr && b.type != nullptr && b.freq != nullptr
                 && b.gain != nullptr && b.q != nullptr && b.slope != nullptr && b.place != nullptr
                 && b.tube != nullptr && b.xfmr != nullptr);
    }

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
    auto index = [] (const std::atomic<float>* v, int count)
    {
        return juce::jlimit (0, count - 1, (int) std::lround (v->load()));
    };

    rz::Params p;
    p.eqOn     = paramEqOn->load() >= 0.5f;
    p.driveOn  = paramDriveOn->load() >= 0.5f;
    p.drive    = paramDrive->load();
    p.focus    = paramFocus->load() * 0.01f;
    p.mix      = paramMix->load() * 0.01f;
    p.xfLow    = paramXfLow->load() * 0.01f;
    p.xfMid    = paramXfMid->load() * 0.01f;
    p.xfHigh   = paramXfHigh->load() * 0.01f;
    p.outputDb = paramOutput->load();
    p.xfAll    = paramXfAll->load() >= 0.5f;

    for (size_t i = 0; i < p.bands.size(); ++i)
    {
        const auto& ptr = bandPtrs[i];
        auto& b = p.bands[i];
        b.used  = ptr.used->load() >= 0.5f;
        b.on    = ptr.on->load() >= 0.5f;
        b.type  = (rz::BandType) index (ptr.type, rz::kNumBandTypes);
        b.freq  = ptr.freq->load();
        b.gain  = ptr.gain->load();
        b.q     = ptr.q->load();
        b.slope = index (ptr.slope, (int) rz::kCutSlopes.size());
        b.place = (rz::Placement) index (ptr.place, rz::kNumPlacements);
        b.tube  = ptr.tube->load() >= 0.5f;
        b.xfmr  = ptr.xfmr->load() >= 0.5f;
    }
    return p;
}

//==============================================================================
void PillowRZAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepared = false;
    baseRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    hqOrder  = rz::oversamplingOrderFor (baseRate);
    sessionRate.store (baseRate);

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

    hqLatency.store (latencyFor (true));
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

    // 호스트가 처리를 멈춘 상태(정지된 무음 트랙 등)에서 모드를 바꿔도 레이턴시는 바로 알림.
    // 호스트가 이걸 보고 재시작하면 prepareToPlay에서 경로도 맞춰지고, 재생 중이면 오디오 스레드 전환과 같은 값이 됨
    if (prepared.load())
    {
        const int want = paramQuality->load() >= 0.5f ? hqLatency.load() : 0;
        if (want != getLatencySamples())
            setLatencySamples (want);
    }
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

void PillowRZAudioProcessor::pushAnalyzer (int which, const juce::AudioBuffer<float>& buffer, int numCh) noexcept
{
    if (! analyzerActive.load (std::memory_order_relaxed))
        return;
    taps[(size_t) which].push (buffer.getReadPointer (0), numCh > 1 ? buffer.getReadPointer (1) : nullptr,
                               buffer.getNumSamples());
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

    pushAnalyzer (0, buffer, numCh);

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

    pushAnalyzer (1, buffer, numCh);
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

    const int numCh = juce::jmin (numProcessChannels, buffer.getNumChannels());
    if (numCh <= 0)
        return;

    pushAnalyzer (0, buffer, numCh);

    if (hq && oversampler != nullptr)
    {
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

    pushAnalyzer (1, buffer, numCh);   // ZERO LATENCY: 원신호 그대로
}

//==============================================================================
void PillowRZAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void PillowRZAudioProcessor::fillMissingParameters (juce::ValueTree& tree) const
{
    // 저장된 상태에 없는 항목은 기본값으로 -> 불러오기 전 인스턴스 값이 남지 않고 항상 같은 결과
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto id = rp->getParameterID();
            if (! tree.getChildWithProperty ("id", id).isValid())
                tree.appendChild (juce::ValueTree ("PARAM", { { "id", id },
                                                              { "value", rp->convertFrom0to1 (rp->getDefaultValue()) } }),
                                  nullptr);
        }
}

bool PillowRZAudioProcessor::migrateLegacyState (juce::ValueTree& tree)
{
    auto child = [&tree] (const juce::String& id) { return tree.getChildWithProperty ("id", id); };
    auto has   = [&child] (const juce::String& id) { return child (id).isValid(); };
    auto value = [&child] (const juce::String& id, double fallback)
    {
        const auto c = child (id);
        return c.isValid() ? (double) c.getProperty ("value", fallback) : fallback;
    };
    auto forModule = [] (const char* base, int m) { return m == 0 ? juce::String (base) : juce::String (base) + "2"; };

    // v0.1 ~ v0.4의 항목들 (모듈 2는 뒤에 "2")
    static const char* const globalIds[] = { "routing", "link", "steps" };
    static const char* const moduleIds[] = { "mode", "treble", "trebleQ", "trebleHz", "bass", "bassQ", "bassHz",
                                             "tilt", "presHz", "presFreq", "presGain", "presQ" };
    bool legacy = false;
    for (auto* id : globalIds) legacy = legacy || has (id);
    for (auto* id : moduleIds) legacy = legacy || has (forModule (id, 0)) || has (forModule (id, 1));
    if (! legacy)
        return false;

    rz::LegacyParams L;
    L.link    = value ("link", 1.0) >= 0.5;      // v0.1은 모듈이 하나 = 링크와 같음
    L.midSide = value ("routing", 0.0) >= 0.5;
    L.stepped = value ("steps", 0.0) >= 0.5;

    for (int m = 0; m < 2; ++m)
    {
        auto v = [&] (const char* base, double fallback) { return (float) value (forModule (base, m), fallback); };
        auto& md = L.mod[(size_t) m];
        md.modeB    = v ("mode", 1.0) >= 0.5f;
        md.trebleDb = v ("treble", 0.0);
        md.trebleQ  = v ("trebleQ", rz::kVintageShelfQ);
        md.trebleHz = v ("trebleHz", rz::kTrebleRefHz);
        md.bassDb   = v ("bass", 0.0);
        md.bassQ    = v ("bassQ", rz::kVintageShelfQ);
        md.bassHz   = v ("bassHz", rz::kBassRefHz);
        md.tiltDb   = v ("tilt", 0.0);
        md.presDb   = v ("presGain", 0.0);
        md.presQ    = v ("presQ", rz::kVintagePresQ);

        // v0.3 이하: Presence 주파수는 4단계 선택 (0..3)
        if (has (forModule ("presHz", m)))
            md.presHz = v ("presHz", rz::kPresenceDefaultHz);
        else if (has (forModule ("presFreq", m)))
            md.presHz = (float) rz::kPresenceHz[(size_t) juce::jlimit (0, 3, (int) std::lround (v ("presFreq", 2.0)))];
    }

    const auto bands = rz::migrateLegacyBands (L);

    // Drive / Level 은 모듈마다 있었음 -> 하나로 (링크면 모듈 1 그대로, 아니면 큰 드라이브 · 평균 레벨)
    const double drive1 = value ("drive", 0.0),  drive2 = value ("drive2", drive1);
    const double out1   = value ("output", 0.0), out2   = value ("output2", out1);
    const double drive  = L.link ? drive1 : std::max (drive1, drive2);
    const double output = L.link ? out1 : 0.5 * (out1 + out2);

    // 옛 항목 지우기 (남겨 두면 다음에 불러올 때 또 변환됨)
    auto remove = [&tree, &child] (const juce::String& id)
    {
        for (auto c = child (id); c.isValid(); c = child (id))
            tree.removeChild (c, nullptr);
    };
    for (auto* id : globalIds) remove (id);
    for (auto* id : moduleIds) { remove (forModule (id, 0)); remove (forModule (id, 1)); }
    remove ("drive2");
    remove ("output2");

    auto set = [&tree, &child] (const juce::String& id, double v)
    {
        auto c = child (id);
        if (c.isValid()) c.setProperty ("value", v, nullptr);
        else             tree.appendChild (juce::ValueTree ("PARAM", { { "id", id }, { "value", v } }), nullptr);
    };

    set (pillow::ids::drive, drive);
    set (pillow::ids::output, output);

    for (int i = 0; i < rz::kMaxBands; ++i)
    {
        using pillow::ids::band;
        const bool usedSlot = i < (int) bands.size();
        const rz::BandParams b = usedSlot ? bands[(size_t) i] : rz::BandParams {};
        set (band (i, pillow::ids::used),  usedSlot ? 1.0 : 0.0);
        set (band (i, pillow::ids::on),    b.on ? 1.0 : 0.0);
        set (band (i, pillow::ids::type),  (double) (int) b.type);
        set (band (i, pillow::ids::freq),  (double) b.freq);
        set (band (i, pillow::ids::gain),  (double) b.gain);
        set (band (i, pillow::ids::q),     (double) b.q);
        set (band (i, pillow::ids::slope), (double) b.slope);
        set (band (i, pillow::ids::place), (double) (int) b.place);
    }
    return true;
}

void PillowRZAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            if (! tree.isValid())
                return;

            migrateLegacyState (tree);       // v0.4 이하 -> 같은 곡선의 밴드
            fillMissingParameters (tree);
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
