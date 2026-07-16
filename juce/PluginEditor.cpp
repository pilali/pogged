#include "PluginEditor.h"

juce::Font pogged::font(float height, bool bold)
{
    return juce::Font(juce::FontOptions()
        .withName("Arial")
        .withHeight(height)
        .withStyle(bold ? "Bold" : "Plain"));
}

// Mirrors the modgui's formatValue() in script-pogged.js. The two must agree:
// the same patch read on the MOD and in a DAW should show the same numbers.
juce::String pogged::formatValue(const juce::String& id, double v)
{
    if (id.startsWith("pan_")) {
        if (std::abs(v) < 0.005) return "C";
        return (v < 0 ? "L" : "R") + juce::String((int) std::round(std::abs(v) * 100.0));
    }
    if (id == "lp_cutoff")
        return v >= 1000.0 ? juce::String(v / 1000.0, 1) + " kHz"
                           : juce::String((int) std::round(v)) + " Hz";
    if (id == "attack_ms" || id == "filter_env_a" || id == "filter_env_d")
        return v >= 1000.0 ? juce::String(v / 1000.0, 2) + " s"
                           : juce::String((int) std::round(v)) + " ms";
    if (id == "detune_cents") return juce::String(v, 1) + " ct";
    if (id == "lp_q")         return "Q " + juce::String(v, 2);
    // Semitones, signed: the sweep runs heel -> toe, so the sign has to be
    // visible or the two ends look interchangeable.
    if (id == "warp_heel" || id == "warp_toe")
        return (v >= 0 ? "+" : "") + juce::String(v, 1) + " st";
    if (id == "filter_env") {
        if (std::abs(v) < 0.005) return "OFF";     // centre disables the sweep
        return (v > 0 ? "+" : "") + juce::String((int) std::round(v * 100.0)) + " %";
    }
    if (id == "input_gain") return juce::String(v, 2) + " x";
    return juce::String((int) std::round(v * 100.0)) + " %";  // levels 0..2 -> %
}

// ── Look and feel ────────────────────────────────────────────────────────────
void PoggedLNF::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                                 float pos, float, float,
                                 juce::Slider::SliderStyle, juce::Slider&)
{
    const float cx = x + w * 0.5f;
    const float trackW = 10.0f;
    const juce::Rectangle<float> track(cx - trackW * 0.5f, (float) y, trackW, (float) h);

    g.setColour(pogged::kInk);
    g.fillRoundedRectangle(track, 4.0f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawRoundedRectangle(track.reduced(0.5f), 4.0f, 1.0f);

    const float capY = pos;
    juce::Rectangle<float> fill(track.getX(), capY, trackW, track.getBottom() - capY);
    g.setGradientFill(juce::ColourGradient(pogged::kRed, 0, capY,
                                           pogged::kRedLo, 0, track.getBottom(), false));
    g.fillRoundedRectangle(fill, 3.0f);

    const float capW = 28.0f, capH = 18.0f;
    juce::Rectangle<float> cap(cx - capW * 0.5f, capY - capH * 0.5f, capW, capH);
    g.setGradientFill(juce::ColourGradient(pogged::kCap, 0, cap.getY(),
                                           juce::Colour(0xffcfc3a8), 0, cap.getBottom(), false));
    g.fillRoundedRectangle(cap, 3.0f);
    g.setColour(pogged::kEdge);
    g.drawRoundedRectangle(cap, 3.0f, 1.0f);
    g.setColour(pogged::kInk.withAlpha(0.35f));
    g.drawHorizontalLine((int) cap.getCentreY(), cap.getX() + 3, cap.getRight() - 3);
}

// Pan pot: cream dial with a pointer from centre to rim, swept -135..+135
// degrees — the modgui's .pogged-pan-dial rotation, redrawn natively.
void PoggedLNF::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                 float posProportional, float, float, juce::Slider&)
{
    auto b = juce::Rectangle<float>((float) x, (float) y, (float) w, (float) h)
                 .withSizeKeepingCentre((float) juce::jmin(w, h), (float) juce::jmin(w, h))
                 .reduced(1.0f);
    const auto c = b.getCentre();

    // Centre detent mark above the pot, as on the panel.
    g.setColour(pogged::kInk.withAlpha(0.45f));
    g.fillRect(c.x - 0.5f, b.getY() - 4.0f, 1.0f, 3.0f);

    g.setGradientFill(juce::ColourGradient(pogged::kCap, c.x, b.getY(),
                                           juce::Colour(0xffb3a68c), c.x, b.getBottom(), false));
    g.fillEllipse(b);
    g.setColour(pogged::kEdge);
    g.drawEllipse(b, 1.0f);

    const float angle = juce::degreesToRadians(-135.0f + posProportional * 270.0f);
    const float r0 = b.getWidth() * 0.10f, r1 = b.getWidth() * 0.42f;
    juce::Line<float> pointer(c.x + r0 * std::sin(angle), c.y - r0 * std::cos(angle),
                              c.x + r1 * std::sin(angle), c.y - r1 * std::cos(angle));
    g.setColour(pogged::kInk);
    g.drawLine(pointer, 2.0f);
}

// ── PanKnob ──────────────────────────────────────────────────────────────────
PanKnob::PanKnob(juce::AudioProcessorValueTreeState& s, const juce::String& id,
                 juce::LookAndFeel* look)
{
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setDoubleClickReturnValue(true, 0.0);   // snap back to centre
    slider.setLookAndFeel(look);
    addAndMakeVisible(slider);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(s, id, slider);
}

PanKnob::~PanKnob() { slider.setLookAndFeel(nullptr); }
void PanKnob::resized() { slider.setBounds(getLocalBounds()); }

namespace {
    // One flat segment of a button strip: red when engaged, cream otherwise.
    // Both the enum switches and the DRY lamps are made of these, so they read
    // as the same kind of button — which is what the pedal does too.
    class PanelButton : public juce::TextButton
    {
    public:
        PanelButton(const juce::String& text, bool isFirst, bool isLast)
            : juce::TextButton(text), first(isFirst), last(isLast) {}

        bool on = false;

        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced(0.5f);
            juce::Path p;
            const float rad = 4.0f;
            p.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(),
                                  rad, rad, first, last, first, last);
            if (on)
                g.setGradientFill(juce::ColourGradient(juce::Colour(0xffe0484a), 0, r.getY(),
                                                       pogged::kRed, 0, r.getBottom(), false));
            else
                g.setGradientFill(juce::ColourGradient(
                    pogged::kBtnTop.brighter(over ? 0.25f : 0.0f), 0, r.getY(),
                    pogged::kBtnBot.brighter(over ? 0.25f : 0.0f), 0, r.getBottom(), false));
            g.fillPath(p);
            g.setColour(on ? juce::Colour(0xff8e191b) : pogged::kEdge);
            g.strokePath(p, juce::PathStrokeType(1.0f));

            g.setColour(on ? juce::Colour(0xfffff6ee) : juce::Colour(0xff4a4238));
            g.setFont(pogged::font(getHeight() <= 17 ? 8.0f : 9.5f, true));
            g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
        }
    private:
        bool first, last;
    };
}

// ── FaderControl ─────────────────────────────────────────────────────────────
FaderControl::FaderControl(juce::AudioProcessorValueTreeState& s,
                           const juce::String& id, const juce::String& cap,
                           juce::LookAndFeel* look, const juce::String& panID,
                           const juce::String& key, const juce::String& dryID)
    : paramID(id), caption(cap), keycap(key)
{
    slider.setSliderStyle(juce::Slider::LinearVertical);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setLookAndFeel(look);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible(slider);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(s, id, slider);

    if (panID.isNotEmpty()) {
        pan = std::make_unique<PanKnob>(s, panID, look);
        addAndMakeVisible(*pan);
    }

    if (dryID.isNotEmpty()) {
        if (auto* prm = s.getParameter(dryID)) {
            auto* b = new PanelButton("DRY", true, true);
            lamp.reset(b);
            addAndMakeVisible(*lamp);
            lampAtt = std::make_unique<juce::ParameterAttachment>(
                *prm, [this, b](float v) {
                    lampOn = v > 0.5f;
                    b->on = lampOn;
                    b->repaint();
                });
            b->onClick = [this] {
                lampAtt->setValueAsCompleteGesture(lampOn ? 0.0f : 1.0f);
            };
            lampAtt->sendInitialUpdate();
        } else {
            jassertfalse;      // a typo'd ID must not silently do nothing
        }
    }
}

FaderControl::~FaderControl() { slider.setLookAndFeel(nullptr); }

void FaderControl::resized()
{
    auto r = getLocalBounds();
    // Every slot below is reserved whether it is filled or not: a row whose
    // columns reserve different heights has its tracks at different lengths and
    // its labels on different lines, which is exactly what it looked like
    // before the spacers existed.
    auto top = r.removeFromTop(pogged::kPanSize + 6);
    if (pan)
        pan->setBounds(top.withSizeKeepingCentre(pogged::kPanSize, pogged::kPanSize)
                          .withY(top.getY()));
    if (pan) r.removeFromTop(8);                 // the L..R marks
    if (keycap.isNotEmpty()) r.removeFromTop(20);
    auto foot = r.removeFromBottom(21);          // DRY lamp, or its spacer
    if (lamp) lamp->setBounds(foot.withSizeKeepingCentre(30, 16).withY(foot.getY() + 4));
    // A keycap already names the column, so it needs no bottom caption.
    r.removeFromBottom(keycap.isNotEmpty() ? 16 : 34);
    slider.setBounds(r);
}

void FaderControl::paint(juce::Graphics& g)
{
    auto r = getLocalBounds();
    r.removeFromTop(pogged::kPanSize + 6);

    if (pan) {
        // The pot's L..R marks, as printed on the pedal.
        auto lr = r.removeFromTop(8).withSizeKeepingCentre(26, 8);
        g.setColour(pogged::kMuted);
        g.setFont(pogged::font(7.5f));
        g.drawText("L", lr, juce::Justification::centredLeft);
        g.drawText("R", lr, juce::Justification::centredRight);
    }
    if (keycap.isNotEmpty()) {
        auto key = r.removeFromTop(20).withSizeKeepingCentre(34, 15);
        g.setColour(pogged::kInk);
        g.fillRoundedRectangle(key.toFloat(), 3.0f);
        g.setColour(pogged::kPanelTop);
        g.setFont(pogged::font(9.5f, true));
        g.drawText(keycap, key, juce::Justification::centred);
    }

    r.removeFromBottom(21);                      // the lamp slot
    auto bottom = r.removeFromBottom(keycap.isNotEmpty() ? 16 : 34);
    g.setColour(pogged::kMuted);
    g.setFont(pogged::font(10.0f));
    g.drawText(pogged::formatValue(paramID, slider.getValue()),
               bottom.removeFromTop(16), juce::Justification::centred);

    if (keycap.isEmpty()) {
        g.setColour(pogged::kInk);
        g.setFont(pogged::font(10.5f, true));
        g.drawText(caption, bottom, juce::Justification::centred);
    }
}

// ── Segmented switch ─────────────────────────────────────────────────────────
SegControl::SegControl(juce::AudioProcessorValueTreeState& s,
                       const juce::String& id, const juce::String& cap,
                       const juce::StringArray& options)
    : caption(cap)
{
    param = s.getParameter(id);
    jassert(param != nullptr);      // a typo'd ID must not silently do nothing

    for (int i = 0; i < options.size(); ++i) {
        auto* b = new PanelButton(options[i], i == 0, i == options.size() - 1);
        b->onClick = [this, i] {
            if (attachment) attachment->setValueAsCompleteGesture((float) i);
        };
        buttons.add(b);
        addAndMakeVisible(b);
    }
    if (param)
        attachment = std::make_unique<juce::ParameterAttachment>(
            *param, [this](float v) { setIndex((int) std::round(v)); });
    if (attachment) attachment->sendInitialUpdate();
}

void SegControl::setIndex(int i)
{
    current = i;
    for (int k = 0; k < buttons.size(); ++k) {
        static_cast<PanelButton*>(buttons[k])->on = (k == i);
        buttons[k]->repaint();
    }
}

void SegControl::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop(13);                       // caption row
    const int n = juce::jmax(1, buttons.size());
    // Integer-divide the strip and give the remainder away one pixel at a time,
    // so the segments tile exactly with no seam or overhang at the right edge.
    const int base = r.getWidth() / n, extra = r.getWidth() % n;
    int x = r.getX();
    for (int i = 0; i < n; ++i) {
        const int w = base + (i < extra ? 1 : 0);
        buttons[i]->setBounds(x, r.getY(), w, r.getHeight());
        x += w;
    }
}

void SegControl::paint(juce::Graphics& g)
{
    g.setColour(pogged::kMuted);
    g.setFont(pogged::font(9.0f));
    g.drawText(caption.toUpperCase(), getLocalBounds().removeFromTop(12),
               juce::Justification::centredLeft);
}

// ── PoggedEditor ─────────────────────────────────────────────────────────────
namespace {
    struct VoiceSpec { const char* id; const char* keycap; const char* pan; };
    // Order and keycaps are the pedal's own: DRY -2 -1 +5th +1 +2.
    const VoiceSpec kVoices[] = {
        { "dry_level",  "DRY",  "pan_dry"  },
        { "sub2_level", "-2",   "pan_sub2" },
        { "sub1_level", "-1",   "pan_sub1" },
        { "up5_level",  "+5th", "pan_up5"  },
        { "up1_level",  "+1",   "pan_up1"  },
        { "up2_level",  "+2",   "pan_up2"  },
    };

    // dry != nullptr puts that DRY routing lamp at the foot of this column, as
    // the pedal does — the button belongs with the effect it routes.
    struct ToneSpec { const char* id; const char* label; const char* dry; };
    const ToneSpec kTone[] = {
        { "detune_cents", "DETUNE", "dry_detune" },
        { "spread",       "SPREAD", nullptr },
        { "attack_ms",    "ATTACK", "dry_attack" },
        { "attack_sens",  "SENS",   nullptr },     // group break after this
        { "warp",         "WARP",   nullptr },
        { "warp_heel",    "HEEL",   nullptr },
        { "warp_toe",     "TOE",    nullptr },     // group break after this
        { "lp_cutoff",    "FREQ",   "dry_filter" },
        { "lp_q",         "RES",    nullptr },
        { "filter_env",   "ENV",    nullptr },
        { "filter_env_a", "ATK",    nullptr },
        { "filter_env_d", "DEC",    nullptr },
        { "filter_sens",  "TRIG",   nullptr },     // group break after this
        { "input_gain",   "IN",     nullptr },
        { "out_level",    "OUT",    nullptr },
    };
    // Indices the vertical group separators are drawn before.
    const int kToneBreaks[] = { 4, 7, 13 };
}

PoggedEditor::PoggedEditor(PoggedAudioProcessor& p)
    : AudioProcessorEditor(p), proc(p)
{
    for (const auto& v : kVoices) {
        auto* f = new FaderControl(proc.apvts, v.id, {}, &lnf, v.pan, v.keycap);
        voices.add(f);
        addAndMakeVisible(f);
    }
    for (const auto& t : kTone) {
        auto* f = new FaderControl(proc.apvts, t.id, t.label, &lnf, {}, {},
                                   t.dry != nullptr ? juce::String(t.dry) : juce::String());
        tone.add(f);
        addAndMakeVisible(f);
    }

    segs.add(new SegControl(proc.apvts, "focus", "Focus",
                            juce::StringArray { "GRAN", "VOC" }));
    segs.add(new SegControl(proc.apvts, "range_mode", "Range",
                            juce::StringArray { "GTR", "BARI", "BASS" }));
    segs.add(new SegControl(proc.apvts, "filter_mode", "Filter",
                            juce::StringArray { "LP", "BP", "HP" }));
    for (auto* s : segs) addAndMakeVisible(s);

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

    setSize(pogged::kPanelW, pogged::kPanelH);
}

PoggedEditor::~PoggedEditor() {}

void PoggedEditor::paint(juce::Graphics& g)
{
    g.setGradientFill(juce::ColourGradient(pogged::kPanelTop, 0, 0,
                                           pogged::kPanelBot, 0, (float) getHeight(), false));
    g.fillAll();

    g.setColour(pogged::kInk.withAlpha(0.25f));
    for (int idx : kToneBreaks) {
        if (idx <= 0 || idx >= tone.size()) continue;
        auto b = tone[idx]->getBounds();
        const int x = b.getX() - 5;
        g.drawVerticalLine(x, (float) (b.getY() + 10), (float) (b.getBottom() - 24));
    }
}

void PoggedEditor::resized()
{
    brand.setBounds(20, 10, 240, 40);
    subtitle.setBounds(22, 50, 300, 16);
    presetBox.setBounds(getWidth() - 210, 22, 190, 26);

    // A column is laid out one full PITCH wide (track + its gap) rather than
    // just the track's width, and they tile edge to edge. The track is drawn
    // centred either way, so the pitch is identical to the modgui's — but the
    // caption now has the whole pitch to sit in. Sized to the track alone,
    // JUCE clips "DETUNE" to "DETU..." (the modgui's labels simply overflow
    // their box, which a Component cannot do).
    const int pitch = pogged::kColW + pogged::kColGap;
    const int rowVoicesY = 84, rowVoicesH = 196;
    int x = pogged::kMargin - pogged::kColGap / 2;
    for (auto* f : voices) {
        f->setBounds(x, rowVoicesY, pitch, rowVoicesH);
        x += pitch;
    }

    // Switch grid: 3 columns, enums on top, the DRY lamps spanning below.
    const int gridX = x + 28;
    auto grid = juce::Rectangle<int>(gridX, rowVoicesY + 26,
                                     getWidth() - pogged::kMargin - gridX, 40);
    const int cellGap = 16;
    const int cellW = (grid.getWidth() - cellGap * 2) / 3;
    for (int i = 0; i < segs.size(); ++i)
        segs[i]->setBounds(grid.getX() + i * (cellW + cellGap), grid.getY(),
                           cellW, grid.getHeight());
    // Row 2: centred, with a wider gap at each group break.
    const int rowToneY = 308, rowToneH = 208;
    const int nBreaks = (int) (sizeof(kToneBreaks) / sizeof(kToneBreaks[0]));
    const int totalW = tone.size() * pitch + nBreaks * 9;
    x = (getWidth() - totalW) / 2;
    for (int i = 0; i < tone.size(); ++i) {
        for (int b : kToneBreaks) if (b == i) x += 9;
        tone[i]->setBounds(x, rowToneY, pitch, rowToneH);
        x += pitch;
    }
}
