#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "pogged_presets.h"

using APVTS = juce::AudioProcessorValueTreeState;
using Range = juce::NormalisableRange<float>;
namespace { const int kVer = 1; }

// Parameter IDs are exactly the LV2 .ttl symbols so state is portable.
APVTS::ParameterLayout PoggedAudioProcessor::createLayout()
{
    using AF = juce::AudioParameterFloat;
    using FA = juce::AudioParameterFloatAttributes;

    APVTS::ParameterLayout p;

    auto pid = [](const char* s){ return juce::ParameterID { s, kVer }; };

    // lp_cutoff: logarithmic → skew so the centre sits musically.
    Range cutoffRange(20.0f, 20000.0f);
    cutoffRange.setSkewForCentre(1000.0f);
    Range qRange(0.5f, 8.0f);          qRange.setSkewForCentre(1.5f);
    // Attack: log-like skew so the musically dense 0–300 ms zone gets most of
    // the travel (linear made a 20 ms swell unreachable on the fader).
    Range atkRange(0.0f, 2000.0f);     atkRange.setSkewForCentre(200.0f);

    p.add(std::make_unique<AF>(pid("dry_level"),    "Dry",          Range(0.0f, 2.0f), 1.0f));
    p.add(std::make_unique<AF>(pid("sub1_level"),   "Sub Octave",   Range(0.0f, 2.0f), 0.8f));
    p.add(std::make_unique<AF>(pid("sub2_level"),   "Sub -2 Oct",   Range(0.0f, 2.0f), 0.0f));
    p.add(std::make_unique<AF>(pid("up1_level"),    "Octave Up",    Range(0.0f, 2.0f), 0.8f));
    p.add(std::make_unique<AF>(pid("up2_level"),    "2 Octaves Up", Range(0.0f, 2.0f), 0.0f));
    p.add(std::make_unique<AF>(pid("detune_cents"), "Detune",       Range(0.0f, 25.0f), 0.0f, FA{}.withLabel("cents")));
    p.add(std::make_unique<AF>(pid("attack_ms"),    "Attack",       atkRange, 0.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("attack_sens"),  "Attack Sens",  Range(0.0f, 1.0f), 0.35f));
    p.add(std::make_unique<AF>(pid("lp_cutoff"),    "LP Filter",    cutoffRange, 20000.0f, FA{}.withLabel("Hz")));
    p.add(std::make_unique<AF>(pid("lp_q"),         "Resonance",    qRange, 0.707f));
    p.add(std::make_unique<AF>(pid("out_level"),    "Output",       Range(0.0f, 2.0f), 1.0f));

    return p;
}

PoggedAudioProcessor::PoggedAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input",  juce::AudioChannelSet::mono(), true)
        .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    auto raw = [this](const char* id){ return apvts.getRawParameterValue(id); };
    pDry    = raw("dry_level");
    pSub1   = raw("sub1_level");
    pSub2   = raw("sub2_level");
    pUp1    = raw("up1_level");
    pUp2    = raw("up2_level");
    pDetune = raw("detune_cents");
    pAttack = raw("attack_ms");
    pSens   = raw("attack_sens");
    pCutoff = raw("lp_cutoff");
    pQ      = raw("lp_q");
    pOut    = raw("out_level");
}

PoggedAudioProcessor::~PoggedAudioProcessor()
{
    if (dsp) pogged_dsp_free(dsp);
}

void PoggedAudioProcessor::prepareToPlay(double sampleRate, int)
{
    // (Re)create the DSP if the sample rate changed — allocation happens here,
    // off the audio thread.
    if (dsp == nullptr || sampleRate != currentSampleRate) {
        if (dsp) pogged_dsp_free(dsp);
        dsp = pogged_dsp_new(sampleRate);
        currentSampleRate = sampleRate;
    }
    if (dsp) pogged_dsp_reset(dsp);
    // The dry path is undelayed and the wet lag varies per voice, so no single
    // latency figure applies — report zero, matching the hardware POG.
    setLatencySamples(0);
}

bool PoggedAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    // Mono engine: any mono/stereo input is summed to mono, the mono result is
    // fanned out to every output channel.
    const auto in = l.getMainInputChannelSet();
    return in == juce::AudioChannelSet::mono()
        || in == juce::AudioChannelSet::stereo()
        || in.isDisabled();
}

void PoggedAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (dsp == nullptr || n == 0) return;

    const PoggedParams p {
        pDry->load(), pSub1->load(), pSub2->load(), pUp1->load(), pUp2->load(),
        pDetune->load(), pAttack->load(), pSens->load(),
        pCutoff->load(), pQ->load(), pOut->load()
    };

    // Mono engine (guitar): sum the input to mono, process once, fan out.
    monoScratch.setSize(1, n, false, false, true);
    float* mono = monoScratch.getWritePointer(0);
    const int numIn = getTotalNumInputChannels();
    if (numIn > 0) {
        juce::FloatVectorOperations::copy(mono, buffer.getReadPointer(0), n);
        for (int ch = 1; ch < numIn; ++ch)
            juce::FloatVectorOperations::add(mono, buffer.getReadPointer(ch), n);
        if (numIn > 1)
            juce::FloatVectorOperations::multiply(mono, 1.0f / (float) numIn, n);
    } else {
        juce::FloatVectorOperations::clear(mono, n);
    }

    pogged_dsp_process(dsp, &p, mono, mono, (uint32_t) n);

    const int numOut = getTotalNumOutputChannels();
    for (int ch = 0; ch < numOut; ++ch)
        juce::FloatVectorOperations::copy(buffer.getWritePointer(ch), mono, n);
}

juce::AudioProcessorEditor* PoggedAudioProcessor::createEditor()
{
    return new PoggedEditor(*this);
}

// ── Factory presets ────────────────────────────────────────────────────────
// Unified with the LV2 .ttl presets (juce/pogged_presets.h). Each preset is a
// list of {symbol, value} pairs; values are applied through the matching APVTS
// parameter so host automation and the editor stay in sync.
int PoggedAudioProcessor::getNumPrograms() { return pogged::kNumPresets; }

const juce::String PoggedAudioProcessor::getProgramName(int index)
{
    if (index < 0 || index >= pogged::kNumPresets) return {};
    return pogged::kPresets[index].name;
}

void PoggedAudioProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= pogged::kNumPresets) return;
    currentProgram = index;

    const auto& preset = pogged::kPresets[index];
    for (int i = 0; i < preset.numParams; ++i)
    {
        const auto& pp = preset.params[i];
        if (auto* param = apvts.getParameter(pp.symbol))
            param->setValueNotifyingHost(param->convertTo0to1(pp.value));
    }
}

void PoggedAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, dest);
}

void PoggedAudioProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

// Mandatory entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PoggedAudioProcessor();
}
