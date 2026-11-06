#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace rz = pillow::rz;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout PillowRZAudioProcessor::createParameterLayout()
{
    using FloatParam = juce::AudioParameterFloat;
    using FloatAttr  = juce::AudioParameterFloatAttributes;
    using Range      = juce::NormalisableRange<float>;

    auto dbAttr = []
    {
        return FloatAttr()
            .withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return pillow::formatDb (v); })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); });
    };

    auto qAttr = []
    {
        return FloatAttr()
            .withStringFromValueFunction ([] (float v, int) { return pillow::formatQ (v); })
            .withValueFromStringFunction ([] (const juce::String& s)
                                          { return s.retainCharacters ("0123456789.").getFloatValue(); });
    };

    Range shelfQRange (rz::kShelfQMin, rz::kShelfQMax, 0.001f);
    shelfQRange.setSkewForCentre (0.6f);      // 빈티지 0.5가 약 40% 위치

    Range presQRange (rz::kPresQMin, rz::kPresQMax, 0.001f);
    presQRange.setSkewForCentre (1.0f);       // 기본 1.0이 정중앙

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { pillow::ids::mode, 1 }, "Mode",
        juce::StringArray { "A (Tilt)", "B (Presence)" }, 1));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { pillow::ids::steps, 1 }, "Vintage Steps", false));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::treble, 1 }, "Treble",
        Range (rz::kTrebleMinDb, rz::kTrebleMaxDb, 0.01f), 0.0f, dbAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::trebleQ, 1 }, "Treble Q",
        shelfQRange, rz::kVintageShelfQ, qAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::bass, 1 }, "Bass",
        Range (rz::kBassMinDb, rz::kBassMaxDb, 0.01f), 0.0f, dbAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::bassQ, 1 }, "Bass Q",
        shelfQRange, rz::kVintageShelfQ, qAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::tilt, 1 }, "Tilt",
        Range (-rz::kTiltMaxDb, rz::kTiltMaxDb, 0.01f), 0.0f, dbAttr()));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { pillow::ids::presFreq, 1 }, "Presence Freq",
        juce::StringArray { "1.4 kHz", "2 kHz", "2.8 kHz", "4 kHz" }, 2));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::presGain, 1 }, "Presence",
        Range (0.0f, rz::kPresMaxDb, 0.01f), 0.0f, dbAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::presQ, 1 }, "Presence Q",
        presQRange, rz::kVintagePresQ, qAttr()));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::tube, 1 }, "Tube",
        Range (0.0f, 100.0f, 0.1f), 0.0f,
        FloatAttr()
            .withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; })
            .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); })));

    layout.add (std::make_unique<FloatParam> (
        juce::ParameterID { pillow::ids::output, 1 }, "Output",
        Range (-18.0f, 18.0f, 0.01f), 0.0f, dbAttr()));

    return layout;
}

//==============================================================================
PillowRZAudioProcessor::PillowRZAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    paramMode     = apvts.getRawParameterValue (pillow::ids::mode);
    paramSteps    = apvts.getRawParameterValue (pillow::ids::steps);
    paramTreble   = apvts.getRawParameterValue (pillow::ids::treble);
    paramTrebleQ  = apvts.getRawParameterValue (pillow::ids::trebleQ);
    paramBass     = apvts.getRawParameterValue (pillow::ids::bass);
    paramBassQ    = apvts.getRawParameterValue (pillow::ids::bassQ);
    paramTilt     = apvts.getRawParameterValue (pillow::ids::tilt);
    paramPresFreq = apvts.getRawParameterValue (pillow::ids::presFreq);
    paramPresGain = apvts.getRawParameterValue (pillow::ids::presGain);
    paramPresQ    = apvts.getRawParameterValue (pillow::ids::presQ);
    paramTube     = apvts.getRawParameterValue (pillow::ids::tube);
    paramOutput   = apvts.getRawParameterValue (pillow::ids::output);

    jassert (paramMode != nullptr && paramSteps != nullptr && paramTreble != nullptr
             && paramTrebleQ != nullptr && paramBass != nullptr && paramBassQ != nullptr
             && paramTilt != nullptr && paramPresFreq != nullptr && paramPresGain != nullptr
             && paramPresQ != nullptr && paramTube != nullptr && paramOutput != nullptr);
}

//==============================================================================
rz::Params PillowRZAudioProcessor::readParams() const noexcept
{
    rz::Params p;
    p.mode      = paramMode->load() >= 0.5f ? rz::Mode::B_Presence : rz::Mode::A_Tilt;
    p.stepped   = paramSteps->load() >= 0.5f;
    p.trebleDb  = paramTreble->load();
    p.trebleQ   = paramTrebleQ->load();
    p.bassDb    = paramBass->load();
    p.bassQ     = paramBassQ->load();
    p.tiltDb    = paramTilt->load();
    p.presIndex = juce::jlimit (0, 3, (int) std::lround (paramPresFreq->load()));
    p.presDb    = paramPresGain->load();
    p.presQ     = paramPresQ->load();
    p.tube      = paramTube->load() * 0.01f;
    p.outputDb  = paramOutput->load();
    return p;
}

//==============================================================================
void PillowRZAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int order = rz::oversamplingOrderFor (sampleRate);

    numProcessChannels = juce::jmax (1, getTotalNumOutputChannels());
    maxBlockSize       = juce::jmax (1, samplesPerBlock);

    // EQ와 튜브를 모두 오버샘플된 레이트에서 처리 (IIR 하프밴드: 지연이 짧고 아날로그 같은 위상)
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) numProcessChannels, (size_t) order,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
        true,    // 최고 품질
        true);   // 정수 레이턴시 (호스트 지연 보상이 정확해짐)
    oversampler->initProcessing ((size_t) maxBlockSize);   // 내부에서 reset 포함

    const double rate = sampleRate * (double) (1 << order);
    processingRate.store (rate);

    engine.prepare (rate, numProcessChannels);
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

    const int numCh = juce::jmin (numProcessChannels, buffer.getNumChannels(), 8);
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

        std::array<float*, 8> ptrs {};
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

    const int numCh = juce::jmin (numProcessChannels, buffer.getNumChannels(), 8);
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
