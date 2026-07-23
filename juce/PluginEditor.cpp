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
// std::round, not (int): the modgui's toFixed(0) rounds where a cast truncates,
// so 50 ms stored as 49.9999 read "49 ms" here and "50 ms" there.
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

// Cream dial with a pointer from centre to rim, swept -135..+135 degrees — the
// modgui's .pogged-knob-dial rotation, redrawn natively.
void PoggedLNF::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                 float posProportional, float, float, juce::Slider&)
{
    auto b = juce::Rectangle<float>((float) x, (float) y, (float) w, (float) h)
                 .withSizeKeepingCentre((float) juce::jmin(w, h), (float) juce::jmin(w, h))
                 .reduced(1.0f);
    const auto c = b.getCentre();

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

// ── Panel button + the reserved row heights ──────────────────────────────────
namespace {
    // One flat segment of a button strip: red when engaged, cream otherwise.
    // Both the enum switches and the lamps are made of these, so they read as
    // the same kind of button. Lamps rather than an OFF/ON pair: on a pedal red
    // means engaged, so lighting an "OFF" segment would read exactly backwards.
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
            const float rad = 3.5f;
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
            g.setFont(pogged::font(getHeight() <= 16 ? 7.5f : 9.0f, true));
            g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
        }
    private:
        bool first, last;
    };

    // Every column reserves each of these, filled or not; see FaderControl.
    constexpr int kKnobRow = 34;   // knob + its caption above it
    constexpr int kLRRow   = 12;   // the pan's L..R marks
    constexpr int kKeyRow  = 20;   // keycap + its margin
    constexpr int kValRow  = 16;   // numeric readout
    constexpr int kLabRow  = 12;   // bottom caption
    constexpr int kLampRow = 20;   // DRY lamp + its margin
}

// ── KnobControl ──────────────────────────────────────────────────────────────
KnobControl::KnobControl(juce::AudioProcessorValueTreeState& s, const juce::String& id,
                         const juce::String& cap, juce::LookAndFeel* look, bool bi)
    : caption(cap), bipolar(bi)
{
    jassert(s.getParameter(id) != nullptr);   // a typo'd ID must not silently do nothing
    slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setLookAndFeel(look);
    addAndMakeVisible(slider);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(s, id, slider);

    // AFTER the attachment, deliberately: SliderParameterAttachment's own
    // constructor assigns textFromValueFunction (and the double-click default),
    // so anything set before it is silently overwritten — the knob would then
    // print the parameter's generic text instead of ours, and read differently
    // from the modgui.
    //
    // A knob carries no permanent readout on either panel (too small), so its
    // value shows while you drag it, as the modgui's tooltip does, through the
    // SAME formatter so the two agree. Double-click returns to the parameter's
    // default, which the attachment already wired.
    slider.textFromValueFunction = [id](double v) { return pogged::formatValue(id, v); };
    slider.setPopupDisplayEnabled(true, false, nullptr);
}

KnobControl::~KnobControl() { slider.setLookAndFeel(nullptr); }

void KnobControl::resized()
{
    slider.setBounds(getLocalBounds().removeFromBottom(pogged::kKnob)
                         .withSizeKeepingCentre(pogged::kKnob, pogged::kKnob));
}

void KnobControl::paint(juce::Graphics& g)
{
    auto top = getLocalBounds().removeFromTop(getHeight() - pogged::kKnob);
    if (caption.isNotEmpty()) {
        g.setColour(pogged::kMuted);
        g.setFont(pogged::font(7.0f));
        g.drawText(caption.toUpperCase(), top, juce::Justification::centredBottom);
    }
    if (bipolar) {   // centre mark on the panel above the pot, as on the pedal
        g.setColour(pogged::kInk.withAlpha(0.45f));
        g.fillRect(getWidth() * 0.5f - 0.5f, (float) top.getBottom() - 3.0f, 1.0f, 3.0f);
    }
}

// ── FaderControl ─────────────────────────────────────────────────────────────
FaderControl::FaderControl(juce::AudioProcessorValueTreeState& s,
                           const juce::String& id, const juce::String& cap,
                           juce::LookAndFeel* look, juce::Array<KnobSpec> knobSpecs,
                           bool lr, const juce::String& key, const juce::String& dryID)
    : paramID(id), caption(cap), keycap(key), showLR(lr)
{
    slider.setSliderStyle(juce::Slider::LinearVertical);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setLookAndFeel(look);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible(slider);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(s, id, slider);

    for (const auto& k : knobSpecs) {
        auto* kc = new KnobControl(s, k.paramID, k.caption, look, k.bipolar);
        knobs.add(kc);
        addAndMakeVisible(kc);
    }

    if (dryID.isNotEmpty()) {
        if (auto* prm = s.getParameter(dryID)) {
            auto* b = new PanelButton("DRY", true, true);
            lamp.reset(b);
            addAndMakeVisible(*lamp);
            lampAtt = std::make_unique<juce::ParameterAttachment>(
                *prm, [this, b](float v) { lampOn = v > 0.5f; b->on = lampOn; b->repaint(); });
            b->onClick = [this] { lampAtt->setValueAsCompleteGesture(lampOn ? 0.0f : 1.0f); };
            lampAtt->sendInitialUpdate();
        } else {
            jassertfalse;
        }
    }
}

FaderControl::~FaderControl() { slider.setLookAndFeel(nullptr); }

void FaderControl::resized()
{
    auto r = getLocalBounds();

    auto knobRow = r.removeFromTop(kKnobRow);
    if (! knobs.isEmpty()) {
        const int n = knobs.size();
        const int w = pogged::kKnob + 6;
        auto row = knobRow.withSizeKeepingCentre(n * w, kKnobRow);
        for (int i = 0; i < n; ++i)
            knobs[i]->setBounds(row.getX() + i * w, row.getY(), w, kKnobRow);
    }
    r.removeFromTop(kLRRow);      // L..R marks, painted
    r.removeFromTop(kKeyRow);     // keycap, painted

    auto foot = r.removeFromBottom(kLampRow);
    if (lamp) lamp->setBounds(foot.withSizeKeepingCentre(30, 15).withY(foot.getY() + 5));
    r.removeFromBottom(kLabRow);  // caption, painted
    r.removeFromBottom(kValRow);  // readout, painted

    slider.setBounds(r);
}

void FaderControl::paint(juce::Graphics& g)
{
    auto r = getLocalBounds();
    r.removeFromTop(kKnobRow);

    auto lrRow = r.removeFromTop(kLRRow);
    if (showLR) {
        auto lr = lrRow.withSizeKeepingCentre(26, kLRRow);
        g.setColour(pogged::kMuted);
        g.setFont(pogged::font(7.5f));
        g.drawText("L", lr, juce::Justification::centredLeft);
        g.drawText("R", lr, juce::Justification::centredRight);
    }

    auto keyRow = r.removeFromTop(kKeyRow);
    if (keycap.isNotEmpty()) {
        auto key = keyRow.withSizeKeepingCentre(32, 15).withY(keyRow.getY());
        g.setColour(pogged::kInk);
        g.fillRoundedRectangle(key.toFloat(), 3.0f);
        g.setColour(pogged::kPanelTop);
        g.setFont(pogged::font(9.0f, true));
        g.drawText(keycap, key, juce::Justification::centred);
    }

    r.removeFromBottom(kLampRow);
    auto labRow = r.removeFromBottom(kLabRow);
    auto valRow = r.removeFromBottom(kValRow);

    g.setColour(pogged::kMuted);
    g.setFont(pogged::font(9.5f));
    g.drawText(pogged::formatValue(paramID, slider.getValue()), valRow,
               juce::Justification::centred);

    // A keycap already names the column; the pedal names each voice once.
    if (keycap.isEmpty()) {
        g.setColour(pogged::kInk);
        g.setFont(pogged::font(10.0f, true));
        g.drawText(caption, labRow, juce::Justification::centred);
    }
}

// ── SegControl ───────────────────────────────────────────────────────────────
SegControl::SegControl(juce::AudioProcessorValueTreeState& s,
                       const juce::String& id, const juce::String& cap,
                       const juce::StringArray& options)
    : caption(cap)
{
    param = s.getParameter(id);
    jassert(param != nullptr);

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
    if (caption.isNotEmpty()) r.removeFromTop(12);   // caption row (skip if none)
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
    g.setFont(pogged::font(8.0f));
    g.drawText(caption.toUpperCase(), getLocalBounds().removeFromTop(11),
               juce::Justification::centredLeft);
}

// ── PoggedEditor ─────────────────────────────────────────────────────────────
namespace {
    using KS = FaderControl::KnobSpec;

    // Column order mirrors the modgui exactly: level | voices | effects.
    struct ColSpec {
        const char* id;
        const char* caption;      // empty when a keycap names the column
        const char* keycap;
        bool  showLR;             // the pan pot's L..R marks
        const char* dry;          // DRY lamp at the foot
        bool  wide;               // carries two knobs (the pedal's FILTER block)
        std::vector<KS> knobs;
    };

    std::vector<ColSpec> columns()
    {
        return {
            { "input_gain", "IN", "", false, "", false, {{ "out_level", "MASTER" }} },

            { "dry_level",  "", "DRY",  true, "", false, {{ "pan_dry",  "", true }} },
            { "sub2_level", "", "-2",   true, "", false, {{ "pan_sub2", "", true }} },
            { "sub1_level", "", "-1",   true, "", false, {{ "pan_sub1", "", true }} },
            { "up5_level",  "", "+5th", true, "", false, {{ "pan_up5",  "", true }} },
            { "up1_level",  "", "+1",   true, "", false, {{ "pan_up1",  "", true }} },
            { "up2_level",  "", "+2",   true, "", false, {{ "pan_up2",  "", true }} },

            { "attack_ms",    "ATTACK", "", false, "dry_attack", false,
              {{ "attack_sens", "SENS" }} },
            { "sustain",      "SUSTAIN", "", false, "", false, {} },
            { "lp_cutoff",    "FILTER", "", false, "dry_filter", true,
              {{ "lp_q", "Q" }, { "filter_env", "ENV", true }} },
            { "detune_cents", "DETUNE", "", false, "dry_detune", false,
              {{ "spread", "SPREAD" }} },
        };
    }
    // Group breaks: before the voices, and before the effects.
    const int kBreaks[] = { 1, 7 };
    constexpr int kVoiceFirst = 1, kVoiceLast = 6;
}

PoggedEditor::PoggedEditor(PoggedAudioProcessor& p)
    : AudioProcessorEditor(p), proc(p)
{
    for (const auto& c : columns()) {
        juce::Array<KS> ks;
        for (const auto& k : c.knobs) ks.add(k);
        auto* f = new FaderControl(proc.apvts, c.id, c.caption, &lnf, ks,
                                   c.showLR, c.keycap, c.dry);
        cols.add(f);
        addAndMakeVisible(f);
    }

    // FOCUS: 3-way engine selector, no caption (its place under the voices and
    // the bracket already say what it is), mirroring the modgui's inline seg.
    focus = std::make_unique<SegControl>(proc.apvts, "focus", "",
                                         juce::StringArray { "GRAN", "VOC", "HYB" });
    addAndMakeVisible(*focus);

    // SETUP: what the POG3 keeps in its OLED menu. We have no OLED, and burying
    // these in the host's generic panel would be hostile — so they get a strip
    // of their own, below the performance controls, which keeps the pedal's own
    // hierarchy.
    segs.add(new SegControl(proc.apvts, "range_mode", "Range",
                            juce::StringArray { "GTR", "BARI", "BASS" }));
    segs.add(new SegControl(proc.apvts, "filter_mode", "Filter",
                            juce::StringArray { "LP", "BP", "HP" }));
    for (auto* s : segs) addAndMakeVisible(s);

    setup.add(new KnobControl(proc.apvts, "filter_env_a", "ENV ATK",  &lnf));
    setup.add(new KnobControl(proc.apvts, "filter_env_d", "ENV DEC",  &lnf));
    setup.add(new KnobControl(proc.apvts, "filter_sens",  "ENV TRIG", &lnf));
    setup.add(new KnobControl(proc.apvts, "warp_heel",    "WARP HEEL", &lnf, true));
    setup.add(new KnobControl(proc.apvts, "warp_toe",     "WARP TOE",  &lnf, true));
    for (auto* k : setup) addAndMakeVisible(k);

    brand.setText("POGGED", juce::dontSendNotification);
    brand.setFont(pogged::font(28.0f, true));
    brand.setColour(juce::Label::textColourId, pogged::kRed);
    addAndMakeVisible(brand);

    subtitle.setText("POLYPHONIC OCTAVE GENERATOR", juce::dontSendNotification);
    subtitle.setFont(pogged::font(9.5f));
    subtitle.setColour(juce::Label::textColourId, pogged::kMuted);
    addAndMakeVisible(subtitle);

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem(proc.getProgramName(i), i + 1);
    presetBox.setSelectedId(proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram(presetBox.getSelectedId() - 1); };
    addAndMakeVisible(presetBox);

    setSize(pogged::kPanelW, pogged::kPanelH);
}

PoggedEditor::~PoggedEditor() {}

void PoggedEditor::paint(juce::Graphics& g)
{
    g.setGradientFill(juce::ColourGradient(pogged::kPanelTop, 0, 0,
                                           pogged::kPanelBot, 0, (float) getHeight(), false));
    g.fillAll();

    // Group separators, drawn in the wider gaps.
    g.setColour(pogged::kInk.withAlpha(0.25f));
    for (int idx : kBreaks) {
        if (idx <= 0 || idx >= cols.size()) continue;
        auto b = cols[idx]->getBounds();
        g.drawVerticalLine(b.getX() - 10, (float) (b.getY() + 34), (float) (b.getBottom() - 40));
    }

    // The FOCUS bracket, hung off the voice columns it groups. (The DRY bracket
    // around the three DRY lamps was removed — it was superfluous.)
    g.setColour(pogged::kInk.withAlpha(0.35f));
    {
        const auto* br = &focusBracket;
        g.drawVerticalLine(br->getX(), (float) br->getY(), (float) br->getBottom());
        g.drawVerticalLine(br->getRight(), (float) br->getY(), (float) br->getBottom());
        g.drawHorizontalLine(br->getBottom(), (float) br->getX(), (float) br->getRight());
    }

    // SETUP strip: set apart from the performance controls above it.
    g.setColour(pogged::kInk.withAlpha(0.18f));
    const int y = getHeight() - 74;
    g.drawHorizontalLine(y, (float) pogged::kMargin, (float) (getWidth() - pogged::kMargin));
    g.setColour(juce::Colour(0xffa0968a));
    g.setFont(pogged::font(8.0f));
    g.drawText("SETUP", pogged::kMargin, y + 5, 60, 10, juce::Justification::centredLeft);
}

void PoggedEditor::resized()
{
    brand.setBounds(18, 8, 240, 36);
    subtitle.setBounds(20, 44, 300, 14);
    presetBox.setBounds(getWidth() - 200, 18, 180, 24);

    // Columns tile at one full PITCH each (track + its gap). The track is drawn
    // centred either way, so the pitch matches the modgui's — but the caption
    // gets the whole pitch to sit in. Sized to the track alone, JUCE clips
    // "DETUNE" to "DETU..." where the modgui's labels simply overflow.
    const int rowY = 78, rowH = 300;
    const auto specs = columns();
    int totalW = 0;
    for (size_t i = 0; i < specs.size(); ++i) {
        totalW += (specs[i].wide ? pogged::kColWide : pogged::kColW) + pogged::kColGap;
        for (int b : kBreaks) if (b == (int) i) totalW += 8;
    }
    int x = (getWidth() - totalW) / 2;

    for (int i = 0; i < cols.size(); ++i) {
        for (int b : kBreaks) if (b == i) x += 8;
        const int w = specs[(size_t) i].wide ? pogged::kColWide : pogged::kColW;
        cols[i]->setBounds(x, rowY, w + pogged::kColGap, rowH);
        x += w + pogged::kColGap;
    }

    // Brackets hang off the columns they group rather than off magic numbers,
    // and BELOW them: the columns' bottom 20px is the DRY lamp row, so a
    // bracket drawn inside that band would cross the very lamps it groups.
    // Matches the modgui's .pogged-bracket { bottom: -14px }.
    const int brTop = rowY + rowH + 7, brH = 7;

    // A column's bounds carry its trailing gap, and the track is drawn centred
    // in them — so the VISIBLE band is narrower than the bounds. A bracket must
    // span the bands, not the bounds, or it starts half a gap wide of the first
    // column. Same span as the modgui's .pogged-bracket-* left/right pair.
    auto band = [&](int i) {
        const int w  = specs[(size_t) i].wide ? pogged::kColWide : pogged::kColW;
        const int cx = cols[i]->getX() + (w + pogged::kColGap) / 2;
        return juce::Range<int>(cx - w / 2, cx + w / 2);
    };
    focusBracket = juce::Rectangle<int>(
        band(kVoiceFirst).getStart(), brTop,
        band(kVoiceLast).getEnd() - band(kVoiceFirst).getStart(), brH);
    // The 3-way selector rides ON the bracket, vertically centred on its line
    // (as the panel does). Wider than the old lamp to fit GRAN/VOC/HYB.
    focus->setBounds(focusBracket.getCentreX() - 57, focusBracket.getBottom() - 8, 114, 16);

    // SETUP strip.
    const int sy = getHeight() - 50;
    segs[0]->setBounds(pogged::kMargin, sy - 12, 108, 32);
    segs[1]->setBounds(pogged::kMargin + 128, sy - 12, 84, 32);
    const int kw = pogged::kKnob + 34;
    int sx = getWidth() - pogged::kMargin - (int) setup.size() * kw;
    for (auto* k : setup) {
        k->setBounds(sx, sy - 14, kw, kKnobRow);
        sx += kw;
    }
}
