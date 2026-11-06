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

    jassert (paramRouting != nullptr && paramLink != nullptr && paramSteps != nullptr && paramEqOn != nullptr
             && paramDriveOn != nullptr && paramFocus != nullptr && paramMix != nullptr
             && paramXfLow != nullptr && paramXfMid != nullptr && paramXfHigh != nullptr);
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
    const int order = rz::oversamplingOrderFor (sampleRate);

    numProcessChannels = juce::jlimit (1, 2, getTotalNumOutputChannels());
    numChannelsInUse.store (numProcessChannels);
    maxBlockSize = juce::jmax (1, samplesPerBlock);

    // EQ·튜브·트랜스포머를 모두 오버샘플된 레이트에서 처리 (IIR 하프밴드: 지연이 짧고 아날로그 같은 위상)
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) numProcessChannels, (size_t) order,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
        true,    // 최고 품질
        true);   // 정수 레이턴시 (호스트 지연 보상이 정확해짐)
    oversampler->initProcessing ((size_t) maxBlockSize);   // 내부에서 reset 포함

    const double rate = sampleRate * (double) (1 << order);
    processingRate.store (rate);

    engine.prepare (rate, numProcessChannels, maxBlockSize << order);
    engine.setParams (readParams());
    engine.reset();

    setLatencySamples ((int) std::lround (oversampler->getLatencyInSamples()));
}

bool PillowRZAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void PillowRZAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (oversampler == nullptr || numSamples == 0)
        return;

    engine.setParams (readParams());

    const int numCh = juce::jmin (numProcessChannels, buffer.getNumChannels());
    if (numCh <= 0)
        return;

    juce::dsp::AudioBlock<float> fullBlock (buffer.getArrayOfWritePointers(),
                                            (size_t) numCh, (size_t) numSamples);

    // 호스트가 prepareToPlay 때보다 큰 블록을 보내도 안전하도록 잘라서 처리
    for (int start = 0; start < numSamples; start += maxBlockSize)
    {
        const int len = juce::jmin (maxBlockSize, numSamples - start);
        auto block = fullBlock.getSubBlock ((size_t) start, (size_t) len);

        auto osBlock = oversampler->processSamplesUp (block);

        std::array<float*, 2> ptrs {};
        for (int c = 0; c < numCh; ++c)
            ptrs[(size_t) c] = osBlock.getChannelPointer ((size_t) c);

        engine.process (ptrs.data(), numCh, (int) osBlock.getNumSamples());

        oversampler->processSamplesDown (block);
    }
}

void PillowRZAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (oversampler == nullptr || numSamples == 0)
        return;

    const int numCh = juce::jmin (numProcessChannels, buffer.getNumChannels());
    if (numCh <= 0)
        return;

    juce::dsp::AudioBlock<float> fullBlock (buffer.getArrayOfWritePointers(),
                                            (size_t) numCh, (size_t) numSamples);

    // EQ/튜브는 건너뛰고 같은 오버샘플링 필터만 통과 -> 처리할 때와 똑같은 지연
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
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
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
