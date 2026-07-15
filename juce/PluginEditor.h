#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Native reproduction of the MOD modgui (icon-pogged.html + stylesheet):
// a cream EHX-style panel with a row of vertical faders. Everything is drawn
// with paths/gradients — no bitmaps — so it stays crisp at any zoom.

namespace pogged {
    const juce::Colour kPanelTop  { 0xfff3ead8 };
    const juce::Colour kPanelBot  { 0xffded1b4 };
    const juce::Colour kInk       { 0xff2b2622 };
    const juce::Colour kRed       { 0xffc22326 };
    const juce::Colour kRedLo     { 0xffa91d20 };
    const juce::Colour kCap       { 0xfffdf8ec };
    const juce::Colour kMuted     { 0xff6b6154 };

    juce::Font font (float height, bool bold = false);
}

// ── Vertical fader look (dark slot, red fill, cream cap) ─────────────────────
class PoggedLNF : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h,
                          float pos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
};

// ── A vertical fader + value readout + caption (modgui .pogged-fader) ────────
class FaderControl : public juce::Component
{
public:
    FaderControl(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
                 const juce::String& caption, juce::LookAndFeel*);
    ~FaderControl() override;
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    juce::String formatValue(const juce::String& id, double v) const;

    juce::Slider slider;
    juce::String paramID;
    juce::String caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FaderControl)
};

// ── The editor: logo + preset row + fader board ──────────────────────────────
class PoggedEditor : public juce::AudioProcessorEditor
{
public:
    explicit PoggedEditor(PoggedAudioProcessor&);
    ~PoggedEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    PoggedAudioProcessor& proc;
    PoggedLNF lnf;
    juce::OwnedArray<FaderControl> faders;
    juce::ComboBox presetBox;
    juce::Label brand, subtitle;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PoggedEditor)
};
