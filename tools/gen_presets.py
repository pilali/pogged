#!/usr/bin/env python3
"""Generate the Pogged factory presets.

Single source of truth for the factory presets shipped across every format:

  * pogged.lv2/<Name>.ttl     — LV2 preset
  * pogged.lv2/manifest.ttl   — re-listing every preset + binary + modgui
  * juce/pogged_presets.h     — C++ table consumed by the JUCE plugin
                                (VST3 / AU / Standalone)

Run from anywhere:  python3 tools/gen_presets.py
The same name/value pair drives the LV2 and the native formats, so a preset
sounds identical in MOD, a DAW, and the standalone app.
"""

import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URI  = "https://github.com/pilali/pogged"

# ── Preset definitions ─────────────────────────────────────────────────────
# Symbols / ranges mirror pogged.ttl:
#   dry_level 0..2   sub1_level 0..2   sub2_level 0..2
#   up1_level 0..2   up2_level 0..2    up5_level 0..2
#   detune_cents 0..25
#   attack_ms 0..2000   attack_sens 0..1
#   lp_cutoff 20..20000   lp_q 0.5..8   out_level 0..2
#   pan_* -1..1 (-1 hard left, 0 centre, +1 hard right)

# Pans are centred unless a preset says otherwise, so a preset only spells out
# what it actually places in the stereo field.
PAN_DEFAULTS = {s: 0.0 for s in
                ("pan_dry", "pan_sub1", "pan_sub2", "pan_up5", "pan_up1", "pan_up2")}


def P(name, **vals):
    v = dict(PAN_DEFAULTS)
    v.update(vals)
    return {"name": name, "vals": v}

PRESETS = [
    P("Classic POG",
      dry_level=1.0, sub1_level=0.8, sub2_level=0.0, up1_level=0.8, up2_level=0.0,
      up5_level=0.0, detune_cents=0.0, attack_ms=0.0, attack_sens=0.35,
      lp_cutoff=20000, lp_q=0.707, out_level=1.0),

    P("Fat Organ",
      dry_level=0.3, sub1_level=1.0, sub2_level=0.0, up1_level=1.0, up2_level=0.7,
      up5_level=0.0, detune_cents=6.0, attack_ms=0.0, attack_sens=0.35,
      lp_cutoff=2500, lp_q=0.707, out_level=1.0),

    P("12-String",
      dry_level=1.0, sub1_level=0.0, sub2_level=0.0, up1_level=0.6, up2_level=0.0,
      up5_level=0.0, detune_cents=9.0, attack_ms=0.0, attack_sens=0.35,
      lp_cutoff=8000, lp_q=0.707, out_level=1.0),

    P("Sub Bass",
      dry_level=0.6, sub1_level=1.4, sub2_level=0.7, up1_level=0.0, up2_level=0.0,
      up5_level=0.0, detune_cents=0.0, attack_ms=0.0, attack_sens=0.35,
      lp_cutoff=700, lp_q=1.2, out_level=1.0),

    P("Slow Cathedral",
      dry_level=0.5, sub1_level=0.6, sub2_level=0.0, up1_level=1.0, up2_level=0.9,
      up5_level=0.0, detune_cents=5.0, attack_ms=900, attack_sens=0.4,
      lp_cutoff=4000, lp_q=0.707, out_level=1.0),

    P("Resonant Synth",
      dry_level=0.0, sub1_level=1.0, sub2_level=0.0, up1_level=1.0, up2_level=0.0,
      up5_level=0.0, detune_cents=0.0, attack_ms=0.0, attack_sens=0.35,
      lp_cutoff=900, lp_q=5.0, out_level=1.0),

    P("String Machine",
      dry_level=0.0, sub1_level=0.5, sub2_level=0.0, up1_level=1.0, up2_level=0.5,
      up5_level=0.0, detune_cents=14.0, attack_ms=350, attack_sens=0.45,
      lp_cutoff=5000, lp_q=0.707, out_level=1.0),

    P("Bass Synth",
      dry_level=0.4, sub1_level=1.2, sub2_level=0.4, up1_level=0.3, up2_level=0.0,
      up5_level=0.0, detune_cents=0.0, attack_ms=0.0, attack_sens=0.3,
      lp_cutoff=1400, lp_q=2.2, out_level=1.0),

    # Showcases the +5th (a POG3 voice, absent from the POG2). The quint is
    # the classic drawbar-organ interval, so it earns its slot rather than
    # just demonstrating the feature. Ninth preset: the bank no longer mirrors
    # the POG2's eight slots now that it carries a POG3 voice.
    P("Quint Organ",
      dry_level=0.4, sub1_level=0.9, sub2_level=0.0, up1_level=0.8, up2_level=0.3,
      up5_level=0.6, detune_cents=4.0, attack_ms=0.0, attack_sens=0.35,
      lp_cutoff=3000, lp_q=0.707, out_level=1.0),
]

# Symbols emitted in each .ttl preset (alphabetical, LV2 convention).
ORDER = sorted([
    "dry_level", "sub1_level", "sub2_level", "up1_level", "up2_level",
    "up5_level", "detune_cents", "attack_ms", "attack_sens", "lp_cutoff",
    "lp_q", "out_level",
    "pan_dry", "pan_sub1", "pan_sub2", "pan_up5", "pan_up1", "pan_up2",
])


TTL_PREFIX = """\
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
"""


def filename(name):
    return name.replace(" ", "_").replace("-", "_") + ".ttl"


def ttl_value(v):
    return f"{float(v):.1f}" if float(v) == int(v) else repr(float(v))


def cpp_value(v):
    s = f"{float(v):.1f}" if float(v) == int(v) else repr(float(v))
    return s + "f"


def write_preset_ttl(path, name, vals):
    rows = []
    for sym in ORDER:
        rows.append(f'\t\tlv2:symbol "{sym}" ;\n\t\tpset:value {ttl_value(vals[sym])}')
    body = "\n\t] , [\n".join(rows)
    text = (
        TTL_PREFIX
        + f'\n<{filename(name)}>\n'
        + "\ta pset:Preset ;\n"
        + f"\tlv2:appliesTo <{URI}> ;\n"
        + f'\trdfs:label "{name}" ;\n'
        + "\tlv2:port [\n"
        + body
        + "\n\t] .\n"
    )
    with open(path, "w") as f:
        f.write(text)


def write_manifest(path):
    out = [
        "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .",
        "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .",
        "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .",
        "",
        f"<{URI}>",
        "    a lv2:Plugin ;",
        "    lv2:binary <pogged.so> ;",
        "    rdfs:seeAlso <pogged.ttl> .",
        "",
    ]
    for p in PRESETS:
        fn = filename(p["name"])
        out += [
            f"<{fn}>",
            f"    lv2:appliesTo <{URI}> ;",
            "    a pset:Preset ;",
            f"    rdfs:seeAlso <{fn}> .",
            "",
        ]
    out += [f"<{URI}> rdfs:seeAlso <modgui.ttl> .", ""]
    with open(path, "w") as f:
        f.write("\n".join(out))


def write_cpp_header(path):
    L = []
    L.append("// Generated by tools/gen_presets.py — do not edit by hand.")
    L.append("//")
    L.append("// Pogged factory presets. The same name/value pairs drive the LV2")
    L.append("// .ttl presets, so a program sounds identical across MOD (LV2),")
    L.append("// a DAW (VST3/AU) and the standalone app.")
    L.append("#pragma once")
    L.append("")
    L.append("namespace pogged {")
    L.append("")
    L.append("struct PresetParam { const char* symbol; float value; };")
    L.append("struct Preset { const char* name; const PresetParam* params; int numParams; };")
    L.append("")
    for i, p in enumerate(PRESETS):
        L.append(f"// {p['name']}")
        L.append(f"static const PresetParam kPreset{i}[] = {{")
        for sym in ORDER:
            L.append(f'    {{ "{sym}", {cpp_value(p["vals"][sym])} }},')
        L.append("};")
        L.append("")
    L.append("static const Preset kPresets[] = {")
    for i, p in enumerate(PRESETS):
        L.append(f'    {{ "{p["name"]}", kPreset{i}, (int) (sizeof(kPreset{i}) / sizeof(kPreset{i}[0])) }},')
    L.append("};")
    L.append("")
    L.append(f"static constexpr int kNumPresets = {len(PRESETS)};")
    L.append("")
    L.append("} // namespace pogged")
    L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L))


def remove_stale_presets(directory):
    keep = {filename(p["name"]) for p in PRESETS}
    keep |= {"manifest.ttl", "pogged.ttl", "modgui.ttl"}
    for fn in os.listdir(directory):
        if fn.endswith(".ttl") and fn not in keep:
            os.remove(os.path.join(directory, fn))


def main():
    lv2_dir = os.path.join(REPO, "pogged.lv2")
    remove_stale_presets(lv2_dir)

    for p in PRESETS:
        write_preset_ttl(os.path.join(lv2_dir, filename(p["name"])),
                         p["name"], p["vals"])

    write_manifest(os.path.join(lv2_dir, "manifest.ttl"))
    write_cpp_header(os.path.join(REPO, "juce", "pogged_presets.h"))

    print(f"Generated {len(PRESETS)} presets:")
    for p in PRESETS:
        print("  -", p["name"])


if __name__ == "__main__":
    main()
