#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Native reproduction of the MOD modgui (icon-pogged.html + stylesheet), laid
// out after the real POG3: one board — level | voices | attack/filter/detune —
// where the secondary parameters are small knobs ABOVE the fader they modify
// (Q + ENV over FILTER, SPREAD over DETUNE, MASTER over INPUT GAIN), which
// groups them instead of lining up fifteen identical faders. Below it, a SETUP
// strip for what the pedal keeps in its OLED menu. Everything is drawn with
// paths/gradients — no bitmaps — so it stays crisp at any zoom.
//
// The layout, grouping, captions and value formatting are kept one-for-one with
// the modgui: same panel size, column order, groups and words. A player moving
// between the MOD and a DAW should see one instrument.
//
// NOT on the panel: `warp` and `freeze`. Both are EXP-jack modes on the pedal —
// the pedal's POSITION, not a setting — so they live in the host's addressing
// (automation, MIDI learn), where every parameter is reachable whether or not
// the editor draws it. Everything else is here; that is checked by diffing the
// TTL's symbols against this file.

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
    constexpr int kPanelW   = 640;
    constexpr int kPanelH   = 500;
    constexpr int kColW     = 36;   // one fader column
    constexpr int kColWide  = 52;   // ... carrying two knobs (the FILTER block)
    constexpr int kColGap   = 12;
    constexpr int kKnob     = 22;
    constexpr int kMargin   = 20;

    juce::Font font (float height, bool bold = false);

    // Shared by both front-ends' readouts; see the modgui's formatValue().
    juce::String formatValue (const juce::String& paramID, double v);
}

// ── Look and feel: vertical fader + rotary knob ──────────────────────────────
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

// ── A small pot riding above the fader it modifies (modgui .pogged-knob) ─────
// caption is drawn above it; `bipolar` paints the centre mark the pedal prints
// over a pot whose middle means something (the pans, and the filter ENV whose
// centre is off). Double-click returns to the parameter's default, which the
// attachment wires by itself.
class KnobControl : public juce::Component
{
public:
    KnobControl(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
                const juce::String& caption, juce::LookAndFeel*,
                bool bipolar = false);
    ~KnobControl() override;
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    juce::Slider slider;
    juce::String caption;
    bool bipolar;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KnobControl)
};

// ── A vertical fader column (modgui .pogged-fader) ───────────────────────────
// Every slot below is RESERVED whether filled or not — knobs, L..R marks,
// keycap, caption, lamp. A column that skips one has a longer track and a
// readout on a different line from its neighbours': that has slipped past the
// eye three times, so tools/modgui_measure.js now checks it on the modgui side
// and this class keeps the two panels in step.
//   knobs  — 0..2 pots above (the pedal's FILTER block carries Q + ENV)
//   showLR — the pan pot's L..R marks, as printed on the pedal
//   keycap — the voice's name in a dark key; a column with one carries no
//            bottom caption, since the pedal names each voice once
//   dryID  — a DRY routing lamp at the foot: the pedal puts each DRY button at
//            the bottom of the column of the effect it routes, not in a block
//            of its own, so the button sits next to the thing it acts on
class FaderControl : public juce::Component
{
public:
    struct KnobSpec {
        juce::String paramID, caption;
        bool bipolar = false;      // paints the centre mark
    };

    FaderControl(juce::AudioProcessorValueTreeState&, const juce::String& paramID,
                 const juce::String& caption, juce::LookAndFeel*,
                 juce::Array<KnobSpec> knobs = {}, bool showLR = false,
                 const juce::String& keycap = {}, const juce::String& dryID = {});
    ~FaderControl() override;
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    juce::Slider slider;
    juce::String paramID, caption, keycap;
    bool showLR;
    juce::OwnedArray<KnobControl> knobs;
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

// FOCUS is a 3-way engine selector (Granular / Vocoder / Hybrid) — a SegControl
// hung under the voices it switches, spanning them ALL (the pedal's bracket
// spans only +1/+2 because its POG algorithm can bend nothing else; ours
// switches every voice). Was a 2-state lamp before §30 added the Hybrid engine.

// ── The editor ───────────────────────────────────────────────────────────────
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
    juce::OwnedArray<FaderControl> cols;      // level | voices | effects
    juce::OwnedArray<SegControl>   segs;      // SETUP: range, filter mode
    juce::OwnedArray<KnobControl>  setup;     // SETUP: env atk/dec/trig, heel/toe
    std::unique_ptr<SegControl>    focus;    // FOCUS: GRAN / VOC / HYB
    juce::ComboBox presetBox;
    juce::Label brand, subtitle;
    juce::Rectangle<int> focusBracket, dryBracket;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PoggedEditor)
};
