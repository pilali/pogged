#include "PluginEditor.h"

juce::Font pogged::font(float height, bool bold)
{
    return juce::Font(juce::FontOptions()
        .withName("Arial")
        .withHeight(height)
        .withStyle(bold ? "Bold" : "Plain"));
}

// ── Vertical fader look ──────────────────────────────────────────────────────
void PoggedLNF::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                                 float pos, float, float,
                                 juce::Slider::SliderStyle, juce::Slider&)
{
    const float cx = x + w * 0.5f;
    const float trackW = 10.0f;
    const juce::Rectangle<float> track(cx - trackW * 0.5f, (float) y, trackW, (float) h);

    // Slot
    g.setColour(pogged::kInk);
    g.fillRoundedRectangle(track, 4.0f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawRoundedRectangle(track.reduced(0.5f), 4.0f, 1.0f);

    // Red fill from the bottom up to the cap
    const float capY = pos;
    juce::Rectangle<float> fill(track.getX(), capY, trackW, track.getBottom() - capY);
    g.setGradientFill(juce::ColourGradient(pogged::kRed, 0, capY,
                                           pogged::kRedLo, 0, track.getBottom(), false));
    g.fillRoundedRectangle(fill, 3.0f);

    // Cream cap
    const float capW = 28.0f, capH = 18.0f;
    juce::Rectangle<float> cap(cx - capW * 0.5f, capY - capH * 0.5f, capW, capH);
    g.setGradientFill(juce::ColourGradient(pogged::kCap, 0, cap.getY(),
                                           juce::Colour(0xffcfc3a8), 0, cap.getBottom(), false));
    g.fillRoundedRectangle(cap, 3.0f);
    g.setColour(juce::Colour(0xff8d8272));
    g.drawRoundedRectangle(cap, 3.0f, 1.0f);
    g.setColour(pogged::kInk.withAlpha(0.35f));
    g.drawHorizontalLine((int) cap.getCentreY(), cap.getX() + 3, cap.getRight() - 3);
}

// ── FaderControl ─────────────────────────────────────────────────────────────
FaderControl::FaderControl(juce::AudioProcessorValueTreeState& s,
                           const juce::String& id, const juce::String& cap,
                           juce::LookAndFeel* look)
    : paramID(id), caption(cap)
{
    slider.setSliderStyle(juce::Slider::LinearVertical);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setLookAndFeel(look);
    addAndMakeVisible(slider);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(s, id, slider);
}

FaderControl::~FaderControl() { slider.setLookAndFeel(nullptr); }

void FaderControl::resized()
{
    // Reserve the bottom 34 px for the value readout + caption.
    slider.setBounds(getLocalBounds().removeFromTop(getHeight() - 34));
}

juce::String FaderControl::formatValue(const juce::String& id, double v) const
{
    if (id == "lp_cutoff")
        return v >= 1000.0 ? juce::String(v / 1000.0, 1) + " kHz"
                           : juce::String((int) v) + " Hz";
    if (id == "attack_ms")
        return v >= 1000.0 ? juce::String(v / 1000.0, 2) + " s"
                           : juce::String((int) v) + " ms";
    if (id == "detune_cents") return juce::String(v, 1) + " ct";
    if (id == "lp_q")         return "Q " + juce::String(v, 2);
    if (id == "attack_sens")  return juce::String((int) (v * 100.0)) + " %";
    return juce::String((int) (v * 100.0)) + " %";   // levels 0..2 -> 0..200 %
}

void FaderControl::paint(juce::Graphics& g)
{
    auto bottom = getLocalBounds().removeFromBottom(34);
    auto valueRow = bottom.removeFromTop(16);
    auto labelRow = bottom;

    g.setColour(pogged::kMuted);
    g.setFont(pogged::font(11.0f));
    g.drawText(formatValue(paramID, slider.getValue()), valueRow,
               juce::Justification::centred);

    g.setColour(pogged::kInk);
    g.setFont(pogged::font(11.5f, true));
    g.drawText(caption, labelRow, juce::Justification::centred);
}

// ── PoggedEditor ─────────────────────────────────────────────────────────────
namespace {
    struct FaderSpec { const char* id; const char* label; };
    const FaderSpec kFaders[] = {
        { "dry_level",    "DRY" },
        { "sub1_level",   "SUB" },
        { "sub2_level",   "SUB -2" },
        { "up1_level",    "+1 OCT" },
        { "up2_level",    "+2 OCT" },
        { "detune_cents", "DETUNE" },
        { "attack_ms",    "ATTACK" },
        { "attack_sens",  "SENS" },
        { "lp_cutoff",    "LP FILT" },
        { "lp_q",         "RES" },
        { "out_level",    "OUT" },
    };
}

PoggedEditor::PoggedEditor(PoggedAudioProcessor& p)
    : AudioProcessorEditor(p), proc(p)
{
    for (const auto& f : kFaders)
        faders.add(new FaderControl(proc.apvts, f.id, f.label, &lnf));
    for (auto* f : faders) addAndMakeVisible(f);

    brand.setText("POGGED", juce::dontSendNotification);
    brand.setFont(pogged::font(30.0f, true));
    brand.setColour(juce::Label::textColourId, pogged::kRed);
    addAndMakeVisible(brand);

    subtitle.setText("POLYPHONIC OCTAVE GENERATOR", juce::dontSendNotification);
    subtitle.setFont(pogged::font(10.5f));
    subtitle.setColour(juce::Label::textColourId, pogged::kMuted);
    addAndMakeVisible(subtitle);

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem(proc.getProgramName(i), i + 1);
    presetBox.setSelectedId(proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] {
        proc.setCurrentProgram(presetBox.getSelectedId() - 1);
    };
    addAndMakeVisible(presetBox);

    setSize(640, 380);
}

PoggedEditor::~PoggedEditor() {}

void PoggedEditor::paint(juce::Graphics& g)
{
    g.setGradientFill(juce::ColourGradient(pogged::kPanelTop, 0, 0,
                                           pogged::kPanelBot, 0, (float) getHeight(), false));
    g.fillAll();

    // Group separators between voices | character | filter/out.
    g.setColour(pogged::kInk.withAlpha(0.22f));
    for (int gx : { 5, 8 }) {
        if (gx < faders.size()) {
            auto b = faders[gx]->getBounds();
            const int x = b.getX() - 8;
            g.drawVerticalLine(x, (float) (b.getY() + 6), (float) (b.getBottom() - 20));
        }
    }
}

void PoggedEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop(80);
    brand.setBounds(20, 10, 240, 40);
    subtitle.setBounds(22, 50, 300, 16);
    presetBox.setBounds(getWidth() - 200, 22, 180, 26);

    auto board = r.reduced(20, 8);
    const int n = faders.size();
    const int gap = 6;
    const int fw = (board.getWidth() - gap * (n - 1)) / n;
    int x = board.getX();
    for (int i = 0; i < n; ++i) {
        // Extra visual gap before the character (idx 5) and filter (idx 8) groups.
        if (i == 5 || i == 8) x += 6;
        faders[i]->setBounds(x, board.getY(), fw, board.getHeight());
        x += fw + gap;
    }
}
