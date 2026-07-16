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
    p.add(std::make_unique<AF>(pid("up5_level"),    "5th Up",       Range(0.0f, 2.0f), 0.0f));
    p.add(std::make_unique<AF>(pid("detune_cents"), "Detune",       Range(0.0f, 25.0f), 0.0f, FA{}.withLabel("cents")));
    p.add(std::make_unique<AF>(pid("attack_ms"),    "Attack",       atkRange, 0.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("attack_sens"),  "Attack Sens",  Range(0.0f, 1.0f), 0.35f));
    p.add(std::make_unique<AF>(pid("lp_cutoff"),    "LP Filter",    cutoffRange, 20000.0f, FA{}.withLabel("Hz")));
    p.add(std::make_unique<AF>(pid("lp_q"),         "Resonance",    qRange, 0.707f));
    p.add(std::make_unique<AF>(pid("out_level"),    "Output",       Range(0.0f, 2.0f), 1.0f));

    // Per-voice pan (POG3): -1 hard left, 0 centre, +1 hard right.
    Range panRange(-1.0f, 1.0f);
    p.add(std::make_unique<AF>(pid("pan_dry"),  "Pan Dry",          panRange, 0.0f));
    p.add(std::make_unique<AF>(pid("pan_sub1"), "Pan Sub Octave",   panRange, 0.0f));
    p.add(std::make_unique<AF>(pid("pan_sub2"), "Pan Sub -2 Oct",   panRange, 0.0f));
    p.add(std::make_unique<AF>(pid("pan_up5"),  "Pan 5th Up",       panRange, 0.0f));
    p.add(std::make_unique<AF>(pid("pan_up1"),  "Pan Octave Up",    panRange, 0.0f));
    p.add(std::make_unique<AF>(pid("pan_up2"),  "Pan 2 Octaves Up", panRange, 0.0f));

    // POG3 SPREAD: stereo delay on the +5th/+1/+2 voices (R = 3x L).
    p.add(std::make_unique<AF>(pid("spread"),   "Spread",           Range(0.0f, 1.0f), 0.0f));

    // Multimode filter + envelope sweep (POG3). Mode order follows the POG3
    // menu: Low-Pass, Band-Pass, High-Pass.
    Range envAtkRange(1.0f, 1000.0f);  envAtkRange.setSkewForCentre(80.0f);
    Range envDcyRange(1.0f, 2000.0f);  envDcyRange.setSkewForCentre(250.0f);
    p.add(std::make_unique<juce::AudioParameterChoice>(
        pid("filter_mode"), "Filter Mode",
        juce::StringArray { "Low Pass", "Band Pass", "High Pass" }, 0));
    p.add(std::make_unique<AF>(pid("filter_env"),   "Filter Env",        Range(-1.0f, 1.0f), 0.0f));
    p.add(std::make_unique<AF>(pid("filter_env_a"), "Filter Env Attack", envAtkRange, 50.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("filter_env_d"), "Filter Env Decay",  envDcyRange, 200.0f, FA{}.withLabel("ms")));
    p.add(std::make_unique<AF>(pid("filter_sens"),  "Filter Env Sens",   Range(0.0f, 1.0f), 0.5f));

    // Instrument range: sizes the sub voices' grains from the lowest note the
    // instrument can play (a baritone's low B puts the sub at 31 Hz).
    p.add(std::make_unique<juce::AudioParameterChoice>(
        pid("range_mode"), "Range",
        juce::StringArray { "Guitar", "Baritone", "Bass" }, 0));

    // FOCUS (POG3): which transposition engine. Granular is the POG sound and
    // answers in 3 ms; the phase vocoder is clean on chords but lags ~85 ms.
    p.add(std::make_unique<juce::AudioParameterChoice>(
        pid("focus"), "Focus",
        juce::StringArray { "Granular (fast)", "Vocoder (clean)" }, 0));

    return p;
}

PoggedAudioProcessor::PoggedAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input",  juce::AudioChannelSet::mono(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
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
    pUp5    = raw("up5_level");
    pPanDry  = raw("pan_dry");
    pPanSub1 = raw("pan_sub1");
    pPanSub2 = raw("pan_sub2");
    pPanUp5  = raw("pan_up5");
    pPanUp1  = raw("pan_up1");
    pPanUp2  = raw("pan_up2");
    pSpread  = raw("spread");
    pFiltMode = raw("filter_mode");
    pFiltEnv  = raw("filter_env");
    pFiltEnvA = raw("filter_env_a");
    pFiltEnvD = raw("filter_env_d");
    pFiltSens = raw("filter_sens");
    pRange    = raw("range_mode");
    pFocus    = raw("focus");
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
    // Mono-in engine: any mono/stereo input is summed to mono. The engine now
    // emits stereo (per-voice pan); a mono output bus is still allowed and gets
    // the L+R fold-down, which is lossless while the pans sit centred.
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

    // Positional — field order must match PoggedParams (= the LV2 port order),
    // so the appended fields (up5_level, then the pans) go last.
    const PoggedParams p {
        pDry->load(), pSub1->load(), pSub2->load(), pUp1->load(), pUp2->load(),
        pDetune->load(), pAttack->load(), pSens->load(),
        pCutoff->load(), pQ->load(), pOut->load(), pUp5->load(),
        pPanDry->load(), pPanSub1->load(), pPanSub2->load(),
        pPanUp5->load(), pPanUp1->load(), pPanUp2->load(), pSpread->load(),
        pFiltMode->load(), pFiltEnv->load(), pFiltEnvA->load(),
        pFiltEnvD->load(), pFiltSens->load(), pRange->load(), pFocus->load()
    };

    // Mono-in engine (guitar): sum the input to mono, process once to stereo.
    // Scratch channels: 0 = mono in, 1 = out L, 2 = out R. The engine needs two
    // distinct output buffers, and the host's bus may be mono, so it cannot
    // write into `buffer` directly.
    scratch.setSize(3, n, false, false, true);
    float* mono = scratch.getWritePointer(0);
    float* wl   = scratch.getWritePointer(1);
    float* wr   = scratch.getWritePointer(2);

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

    pogged_dsp_process(dsp, &p, mono, wl, wr, (uint32_t) n);

    const int numOut = getTotalNumOutputChannels();
    if (numOut >= 2) {
        juce::FloatVectorOperations::copy(buffer.getWritePointer(0), wl, n);
        juce::FloatVectorOperations::copy(buffer.getWritePointer(1), wr, n);
        for (int ch = 2; ch < numOut; ++ch)   // >2 outputs: mirror the left
            juce::FloatVectorOperations::copy(buffer.getWritePointer(ch), wl, n);
    } else if (numOut == 1) {
        // Fold down rather than dropping R, or hard-panned voices would vanish.
        float* o = buffer.getWritePointer(0);
        for (int i = 0; i < n; ++i) o[i] = 0.5f * (wl[i] + wr[i]);
    }
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
