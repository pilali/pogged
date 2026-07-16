#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Native reproduction of the MOD modgui (icon-pogged.html + stylesheet):
// a cream EHX-style panel, two rows of vertical faders — voices with their pan
// pots plus a switch grid, then character | warp | filter | level. Everything
// is drawn with paths/gradients — no bitmaps — so it stays crisp at any zoom.
//
// The layout, grouping, captions and value formatting are deliberately kept
// one-for-one with the modgui: same panel size, same column order, same words.
// A player moving between the MOD and a DAW should see one instrument, and any
// control that exists in one front-end must exist in the other — every LV2
// control port is reachable from both.

namespace pogged {
    const juce::Colour kPanelTop  { 0xfff3ead8 };
    const juce::Colour kPanelBot  { 0xffded1b4 };
    const juce::Colour kInk       { 0xff2b2622 };
    const juce::Colour kRed       { 0xffc22326 };
    const juce::Colour kRedLo     { 0xffa91d20 };
    const juce::Colour kCap       { 0xfffdf8ec };
    const juce::Colour kMuted     { 0xff6b6154 };
    const juce::Colour kEdge      { 0xff8d8272 };
    const juce::Colour kBtnTop    { 0xffefe5d0 };
    const juce::Colour kBtnBot    { 0xffd8cbb0 };

    // Geometry shared with the stylesheet, so the two panels stay the same size.
    constexpr int kPanelW   = 880;
    constexpr int kPanelH   = 560;
    constexpr int kColW     = 36;   // one fader column
    constexpr int kColGap   = 12;
    constexpr int kPanSize  = 22;
    constexpr int kMargin   = 24;

    juce::Font font (float height, bool bold = false);

    // Shared by both front-ends' readouts; see the modgui's formatValue().
    juce::String formatValue (const juce::String& paramID, double v);
}

// ── Vertical fader look (dark slot, red fill, cream cap) ─────────────────────
class PoggedLNF : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h,
                          float pos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h,
                          float posProportional, float startAngle, float endAngle,
                          juce::Slider&) override;
};

// ── A small pan pot, riding above a voice fader (modgui .pogged-pan) ─────────
class PanKnob : public juce::Component
{
public:
    PanKnob(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
            juce::LookAndFeel*);
    ~PanKnob() override;
    void resized() override;
private:
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanKnob)
};

// ── A vertical fader column (modgui .pogged-fader) ───────────────────────────
// Optional, and each empty slot is still RESERVED so every track in a row
// starts and ends on the same line (the modgui does this with
// .pogged-pan-spacer / .pogged-lamp-spacer, for the same reason):
//   panID  — a pan pot above, with the pedal's L..R marks
//   keycap — the voice's name in a dark key, as the pedal prints it; a column
//            with one carries no bottom caption, since it is already named
//   dryID  — a DRY routing lamp at the foot. The pedal puts each DRY button at
//            the bottom of the column of the effect it routes, not in a block
//            of its own, so the button sits next to the thing it acts on.
class FaderControl : public juce::Component
{
public:
    FaderControl(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
                 const juce::String& caption, juce::LookAndFeel*,
                 const juce::String& panID = {}, const juce::String& keycap = {},
                 const juce::String& dryID = {});
    ~FaderControl() override;
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    juce::Slider slider;
    juce::String paramID;
    juce::String caption;
    juce::String keycap;
    std::unique_ptr<PanKnob> pan;
    std::unique_ptr<juce::TextButton> lamp;
    std::unique_ptr<juce::ParameterAttachment> lampAtt;
    bool lampOn = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FaderControl)
};

// ── A segmented enum switch (modgui .pogged-seg) ─────────────────────────────
// One button per value; the active one lights red. Attaches to a choice
// parameter in both directions, so host automation lights the right segment.
class SegControl : public juce::Component
{
public:
    SegControl(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
               const juce::String& caption, const juce::StringArray& options);
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    void setIndex(int i);

    juce::String caption;
    juce::OwnedArray<juce::TextButton> buttons;
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    int current = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SegControl)
};

// ── The editor: logo + preset row + two fader rows + switch grid ─────────────
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
    juce::OwnedArray<FaderControl> voices;   // row 1
    juce::OwnedArray<FaderControl> tone;     // row 2
    juce::OwnedArray<SegControl>   segs;
    juce::ComboBox presetBox;
    juce::Label brand, subtitle;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PoggedEditor)
};
