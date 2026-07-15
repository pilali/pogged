# Playbook — porter un plugin LV2 vers VST3 / AU / Windows (JUCE)

Guide réutilisable pour transformer un plugin **LV2/MOD** existant en plugin
**multi-plateforme** (VST3 + Audio Unit + Standalone, macOS universel + Windows)
**sans toucher au son**, via un cœur DSP partagé et des wrappers JUCE.

Issu d'un portage réel et validé (VST3 + AU fonctionnels dans Logic Pro,
binaires universels confirmés, CI verte macOS + Windows).

---

## 0. Comment utiliser ce document dans une nouvelle session Claude Code

Ouvre Claude Code à la racine du dépôt du plugin LV2 à porter, puis colle :

> J'ai un plugin **LV2** que je veux rendre multi-plateforme (VST3 + AU +
> Standalone, macOS universel + Windows via CI) en suivant le playbook
> `docs/lv2-to-multiplatform.md` (copie-le dans ce dépôt si besoin). Applique-le
> phase par phase. Commence par la **Phase 0** (audit) puis la **Phase 1**
> (extraction du cœur DSP), et **vérifie que la sortie reste bit-identique**
> avant de continuer. Ne pousse / ne signe rien sans me demander.

Le principe directeur, à rappeler à l'agent :

> **Un seul cœur DSP host-agnostic, plusieurs wrappers minces.** Le code de
> traitement ne dépend d'aucun format de plugin ; LV2, JUCE/VST3/AU incluent
> tous le même fichier C.

---

## 1. Le modèle

```
            src/<plugin>_dsp.{c,h}        <-- cœur DSP, zéro dépendance de format
               |              |
   src/<plugin>.c          juce/PluginProcessor.cpp
   (wrapper LV2)           (wrapper JUCE -> VST3 / AU / Standalone)
```

- Le **cœur** expose : un struct de paramètres, un état opaque, et
  `new / free / reset / process`.
- Chaque **wrapper** ne fait que mapper les contrôles de l'hôte sur le struct de
  paramètres et appeler `process`.
- Toute correction de son profite à toutes les versions d'un coup.

---

## Phase 0 — Audit du plugin LV2

À récupérer / lire avant tout :

1. **`*.ttl`** : la liste des ports — pour chaque port de contrôle, noter
   `symbol`, `name`, `default`, `minimum`, `maximum`, l'unité (`units:unit`),
   et les propriétés (`lv2:logarithmic`, `lv2:integer`, `lv2:enumeration`,
   `lv2:toggled`, `scalePoint`). **C'est la source de vérité des paramètres
   JUCE** (mêmes IDs, plages, défauts, enums).
2. **Le `.c` du DSP** : repérer la frontière entre
   - l'**état DSP** + les fonctions de traitement (portable), et
   - le **wrapper LV2** (`instantiate` / `connect_port` / `run` / `cleanup`,
     pointeurs de ports).
3. Le nombre de canaux (mono ? stéréo ?) et si le plugin a une entrée audio.

Décider des **codes d'identité** (réutilisés partout) :

| Champ | Règle | Exemple |
|-------|-------|---------|
| Manufacturer code | 4 caractères, **≥ 1 majuscule** | `Plli` |
| Plugin code       | 4 caractères, **≥ 1 majuscule** | `Gsyn` |
| Company name      | libre | `pilali` |
| Bundle ID         | reverse-DNS | `com.pilali.<plugin>` |

> ⚠️ Si manufacturer **ou** plugin code est tout en minuscules, l'AU ne se
> charge pas. Mets au moins une majuscule à chacun.

---

## Phase 1 — Extraire le cœur DSP (`<plugin>_dsp.{c,h}`)

Objectif : sortir tout le traitement dans un module **sans aucun `#include` de
format** (pas de `lv2.h`), consommable depuis C **et** C++.

### Interface (`src/<plugin>_dsp.h`)

```c
#ifndef PLUGIN_DSP_H
#define PLUGIN_DSP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {           /* indispensable : JUCE est du C++ */
#endif

/* Une valeur par contrôle, recopiée des ports LV2 / paramètres JUCE.
   Valeurs brutes acceptées : le clamp se fait dans process(). */
typedef struct {
    float param_a;
    float param_b;
    /* ... un champ par port de contrôle, mêmes noms/symboles que le .ttl ... */
} PluginParams;

typedef struct PluginDsp PluginDsp;          /* état opaque */

PluginDsp* plugin_dsp_new(double sample_rate);
void       plugin_dsp_free(PluginDsp*);
void       plugin_dsp_reset(PluginDsp*);     /* = activate() : vide filtres/états */
void       plugin_dsp_process(PluginDsp*, const PluginParams*,
                              const float* in, float* out, uint32_t n);

#ifdef __cplusplus
}
#endif
#endif
```

### Implémentation (`src/<plugin>_dsp.c`)

- Y déplacer **tel quel** : helpers, filtres, oscillateurs, etc.
- Le `struct PluginDsp` = l'ancien struct **moins** les pointeurs de ports.
- `plugin_dsp_new` ← le corps de `instantiate` (allocations comprises).
- `plugin_dsp_free` ← le corps de `cleanup`.
- `plugin_dsp_reset` ← le corps de `activate`.
- `plugin_dsp_process` ← le corps de `run`, mais en lisant `p->param_x` au lieu
  de `*self->port_x`. **Conserver le clamp par bloc à l'identique** (mêmes
  bornes, même ordre d'opérations) pour ne rien changer au son.

### Réduire le wrapper LV2 (`src/<plugin>.c`)

Il ne garde que : l'enum des ports, un petit struct `{ PluginDsp* dsp; pointeurs
de ports; }`, et `instantiate/connect_port/activate/run/cleanup` qui délèguent.
`run` construit un `PluginParams` à partir des ports et appelle
`plugin_dsp_process`.

### Build : compiler les deux sources

Dans le Makefile LV2, passer de `$<` à la liste des sources :

```make
SRC := src/<plugin>.c src/<plugin>_dsp.c
$(SO): $(SRC) src/<plugin>_dsp.h | $(BUNDLE)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)
```

### ✅ Vérifier que la sortie est BIT-IDENTIQUE (étape non négociable)

Compiler l'ancienne version (depuis git) et la nouvelle, puis comparer
échantillon par échantillon sur toutes les combinaisons de modes :

```c
/* cmp.c — dlopen old.so & new.so, même entrée, compare les sorties */
/* connecte tous les ports, exécute run() sur chaque combinaison de
   paramètres discrets, et imprime max|A-B| (doit être 0.000e+00). */
```

```sh
git show <branche>:src/<plugin>.c > /tmp/old.c
cc -O2 -ffast-math -fPIC $(pkg-config --cflags lv2) -shared -fvisibility=hidden \
   /tmp/old.c -lm -o /tmp/old.so
make                                   # build la version refactorée
cc -O2 $(pkg-config --cflags lv2) cmp.c -ldl -lm -o /tmp/cmp
/tmp/cmp /tmp/old.so <bundle>/<plugin>.so   # attendu : worst diff 0.000e+00
```

Vérifier aussi que le core s'inclut **depuis C++** (frontière `extern "C"`) :

```sh
c++ -std=c++17 -Isrc -x c++ - src/<plugin>_dsp.c -lm -o /tmp/t <<'EOF'
#include "<plugin>_dsp.h"
int main(){ auto* d=plugin_dsp_new(48000.0); plugin_dsp_free(d); return 0; }
EOF
```

> **Commit** la Phase 1 seulement une fois le diff à zéro confirmé.

---

## Phase 2 — Projet JUCE (VST3 + AU + Standalone)

Créer un dossier `juce/`.

### `juce/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.22)

# macOS : binaires universels (arm64 + x86_64) + min OS. AVANT project().
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS archs")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "min macOS")
endif()

project(<Plugin> VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE : fetch auto (override -DGSYNTH_JUCE_DIR=/path pour offline).
set(JUCE_TAG "8.0.8" CACHE STRING "JUCE git tag")
option(COPY_AFTER_BUILD "Installer dans les dossiers user après build" ON)
if(DEFINED LOCAL_JUCE_DIR)
    add_subdirectory(${LOCAL_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${JUCE_TAG} GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(<Plugin>
    COMPANY_NAME "<company>"
    BUNDLE_ID com.<company>.<plugin>
    PLUGIN_MANUFACTURER_CODE <Mfr4>     # 4 car., >=1 majuscule
    PLUGIN_CODE <Cod4>                  # 4 car., >=1 majuscule
    IS_SYNTH FALSE                      # TRUE si instrument
    NEEDS_MIDI_INPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${COPY_AFTER_BUILD}
    FORMATS AU VST3 Standalone          # PAS de VST2 (SDK retiré par Steinberg)
    VST3_CATEGORIES Fx                  # ou Fx Filter / Instrument...
    AU_MAIN_TYPE kAudioUnitType_Effect  # ou kAudioUnitType_MusicDevice
    PRODUCT_NAME "<Plugin>")

set(DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_sources(<Plugin> PRIVATE
    PluginProcessor.cpp PluginEditor.cpp ${DSP_DIR}/<plugin>_dsp.c)
target_include_directories(<Plugin> PRIVATE ${DSP_DIR})
target_compile_definitions(<Plugin> PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(<Plugin>
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
```

### `PluginProcessor` (points clés)

- **Paramètres via `AudioProcessorValueTreeState`** (APVTS) → automation, presets,
  sauvegarde d'état, et un éditeur générique « gratuit » au début.
- **IDs = symboles du `.ttl`** → l'état est cohérent entre LV2 et JUCE.
- Mapper chaque port :
  - flottant linéaire → `AudioParameterFloat` + `NormalisableRange{min,max}`.
  - flottant `lv2:logarithmic` (ex. fréquence) → `range.setSkewForCentre(centre)`.
  - `lv2:enumeration` / `scalePoint` → `AudioParameterChoice{ {labels...}, defaut }`.
  - unité → `AudioParameterFloatAttributes{}.withLabel("Hz")`.
- Cacher les pointeurs atomiques en ctor : `apvts.getRawParameterValue("id")`.
- `prepareToPlay` : (re)créer le DSP si le sample rate change, puis `reset`.
  L'allocation a lieu **ici**, hors du thread audio.
- **Moteur mono** (cas guitare) : sommer l'entrée en mono, `process` une fois,
  recopier sur toutes les sorties.

```cpp
void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const PluginParams p { pA->load(), pB->load() /* ... */ };
    monoScratch.setSize(1, n, false, false, true);
    float* mono = monoScratch.getWritePointer(0);
    if (getTotalNumInputChannels() > 0)
        juce::FloatVectorOperations::copy(mono, buffer.getReadPointer(0), n);
    else
        juce::FloatVectorOperations::clear(mono, n);
    plugin_dsp_process(dsp, &p, mono, mono, (uint32_t) n);   // in-place mono OK
    for (int ch = 0; ch < getTotalNumOutputChannels(); ++ch)
        juce::FloatVectorOperations::copy(buffer.getWritePointer(ch), mono, n);
}

bool isBusesLayoutSupported(const BusesLayout& l) const override {
    const auto out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    const auto in = l.getMainInputChannelSet();
    return in == out || in.isDisabled();
}
```

- État : `getStateInformation` / `setStateInformation` via `apvts.copyState()`
  (XML → binaire).
- Point d'entrée obligatoire :
  ```cpp
  juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new <Plugin>AudioProcessor(); }
  ```

> Au début, `createEditor()` peut retourner
> `new juce::GenericAudioProcessorEditor(*this)` pour tester tout de suite, puis
> on passe à l'éditeur custom (Phase 3).

---

## Phase 3 — Éditeur custom (optionnel mais recommandé pour l'identité)

Pour **reproduire l'UI MOD** (modgui) en natif (net à toute résolution) :

- Sous-classer `juce::LookAndFeel_V4` et **override `drawLinearSlider`** (faders
  verticaux) ou `drawRotarySlider` (knobs) : dessiner directement avec
  `ColourGradient`, `fillRoundedRectangle`, etc. — pas de bitmap, donc net en
  Retina / au zoom.
- L'éditeur : `setLookAndFeel(&lnf)` (les enfants en héritent ;
  `setLookAndFeel(nullptr)` au destructeur), des `juce::Slider` (un par contrôle)
  reliés par `AudioProcessorValueTreeState::SliderAttachment`, des `juce::Label`
  pour les noms courts, et `paint()` pour le fond/marque/séparateurs/groupes.
- Reproduire les **groupes et l'ordre** exactement comme le modgui.
- JUCE 8 : construire les polices avec **`FontOptions`**
  (`juce::Font(juce::FontOptions().withHeight(h).withStyle("Bold"))`) — évite les
  avertissements de dépréciation.
- Brancher : `createEditor()` retourne ton éditeur ; ajouter `PluginEditor.cpp`
  aux `target_sources`.

---

## Phase 4 — macOS universel + version minimale

Déjà couvert par le `CMakeLists` (Phase 2) :
`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` et `CMAKE_OSX_DEPLOYMENT_TARGET`.
Vérifier après build :

```sh
lipo -info "<...>/VST3/<Plugin>.vst3/Contents/MacOS/<Plugin>"   # -> x86_64 arm64
lipo -info "<...>/AU/<Plugin>.component/Contents/MacOS/<Plugin>" # -> x86_64 arm64
```

---

## Phase 5 — CI GitHub Actions (macOS universel + Windows)

`.github/workflows/build.yml` — pas besoin de PC Windows :

```yaml
name: build
on:
  push: { branches: [ JUCE ], tags: [ 'v*' ] }
  workflow_dispatch:
jobs:
  macos:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - run: cmake -B juce/build -S juce -DCMAKE_BUILD_TYPE=Release
             -DCOPY_AFTER_BUILD=OFF -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
      - run: cmake --build juce/build --config Release --parallel
      - run: lipo -info "juce/build/<Plugin>_artefacts/Release/AU/<Plugin>.component/Contents/MacOS/<Plugin>" || true
      - uses: actions/upload-artifact@v4
        with:
          name: <Plugin>-macOS-universal
          path: |
            juce/build/<Plugin>_artefacts/Release/AU/<Plugin>.component
            juce/build/<Plugin>_artefacts/Release/VST3/<Plugin>.vst3
            juce/build/<Plugin>_artefacts/Release/Standalone/<Plugin>.app
  windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - run: cmake -B juce/build -S juce -DCMAKE_BUILD_TYPE=Release -DCOPY_AFTER_BUILD=OFF
      - run: cmake --build juce/build --config Release --parallel
      - uses: actions/upload-artifact@v4
        with:
          name: <Plugin>-Windows
          path: |
            juce/build/<Plugin>_artefacts/Release/VST3/<Plugin>.vst3
            juce/build/<Plugin>_artefacts/Release/Standalone/<Plugin>.exe
```

- En CI, mettre `COPY_AFTER_BUILD=OFF` (on empaquette les artefacts, on
  n'installe pas dans des dossiers user).
- Le workflow se déclenche **au push de la branche** → pense à pousser.

---

## Phase 6 — Validation

```sh
cd juce
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

- **Charger dans un vrai hôte** (Logic / Reaper / Live) : c'est la vérité.
- Optionnel mais excellent : `pluginval --strictness-level 8 <chemin du .vst3>`.
- Vérifier les 13… (N) paramètres, l'automation, le son vs le LV2.

---

## ⚠️ Pièges rencontrés (et comment les éviter)

| Symptôme | Cause | Solution |
|----------|-------|----------|
| `auval` dit *"didn't find the component"* / erreur `-50`, AU absent de `auval -a` **alors que le composant est parfait** | Sur macOS (Monterey surtout) `auval` / le cache AU sont capricieux ; le scan tiers est in-process | **Valider dans un vrai hôte** (Logic le voit après un rescan de plugins). Ne pas s'enliser dans `auval`/registrar/cache. |
| L'AU n'apparaît nulle part après build | `COPY_PLUGIN_AFTER_BUILD` à OFF → le `.component` reste dans `build/`, jamais installé | `COPY_PLUGIN_AFTER_BUILD ON` (local) ; il s'installe dans `~/Library/Audio/Plug-Ins/Components`. |
| L'install/signature ne se rejoue pas | L'étape de copie est un `POST_BUILD` : si la cible est à jour, rien ne se passe | Forcer le relink (`rm -rf build/<...>/AU/<Plugin>.component` puis rebuild) ou rebuild propre. |
| Composant en `x86_64` seul sur un Mac Apple Silicon → l'hôte natif ne le voit pas | Shell **x86_64 sous Rosetta** (souvent un env **conda** `base` en x86_64) → toolchain x86_64 | Construire **universel** (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`) ; vérifier `uname -m` / `lipo -info`. |
| L'AU ne se charge pas du tout | Manufacturer **ou** plugin code tout en minuscules | Mettre ≥ 1 majuscule à chacun. |
| Build VST2 / SDK manquant | Steinberg ne distribue plus le SDK VST2 | Cibler **VST3** (et éventuellement CLAP). |
| Avertissements `Font(float)` dépréciés | API JUCE 8 | Utiliser `juce::FontOptions`. |
| Le son a (légèrement) changé après extraction | Clamp/ordre modifiés dans `process` | Re-vérifier le diff bit-à-bit ; conserver le clamp par bloc **à l'identique**. |
| CI ne démarre pas | La branche n'est pas sur GitHub | Pousser la branche (`git push -u origin <branche>`). |

---

## Distribution (étape ultérieure, macOS)

Pour diffuser **hors de sa propre machine**, sinon Gatekeeper bloque :

1. **Apple Developer ID** (99 $/an).
2. Secrets GitHub : certificat `.p12` (base64) + mot de passe ; Apple ID +
   mot de passe d'app (ou clé App Store Connect).
3. Dans le job macOS : `codesign --deep --options runtime --sign "Developer ID
   Application: …"` sur le `.component` / `.vst3`, puis
   `xcrun notarytool submit … --wait`, puis `xcrun stapler staple`.
4. Windows : signature Authenticode **optionnelle** (sinon SmartScreen avertit).

Localement, la signature **ad-hoc** appliquée par JUCE suffit pour tester.

---

## Checklist de portage

- [ ] Phase 0 — ports `.ttl` relevés, codes d'identité choisis, mono/stéréo connu
- [ ] Phase 1 — `<plugin>_dsp.{c,h}` extrait, **diff bit-à-bit = 0**, commit
- [ ] Phase 2 — projet JUCE compile, paramètres mappés, charge dans un hôte
- [ ] Phase 3 — éditeur custom (si UI voulue)
- [ ] Phase 4 — `lipo -info` confirme l'universel
- [ ] Phase 5 — CI verte, artefacts macOS + Windows
- [ ] Phase 6 — validé dans un hôte (+ `pluginval`)
- [ ] Distribution — signature / notarisation (si diffusion publique)
```
