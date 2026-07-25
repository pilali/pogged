# Évaluation du code — Pogged

Rapport de la passe d'évaluation du 2026-07-25, sur `master` (0de152f).

`make audit` répond à « est-ce que ça sonne encore juste ». Ce document répond
aux trois autres questions, celles qu'aucun test n'assertait :

1. **qu'est-ce qu'il y a dedans** — inventaire des fonctions ;
2. **qu'est-ce qui est incohérent** — écarts entre le code, les `.ttl`, le
   Makefile, le CMake et le README ;
3. **qu'est-ce que ça coûte** — profil CPU mesuré, et où le récupérer.

Le workflow qui régénère tout ça est décrit à la fin, et tourne en CI
(`.github/workflows/code-eval.yml`).

---

## 1. Inventaire

**144 définitions** (fonctions libres, méthodes, constructeurs) sur 8 787
lignes. Liste complète et à jour : [`docs/function-inventory.md`](function-inventory.md),
régénérée par `make eval-functions`.

Vue d'ensemble du cœur DSP (`src/`, 4 063 lignes) :

| Fichier | Défs | Rôle | API publique |
|---|---:|---|---|
| `pogged_dsp.cpp` | 11 | orchestration : ring, mixage, pan, filtre, dry | `pogged_dsp_new/free/reset/process` + 7 helpers statiques |
| `stream_vocoder.hpp` | 19 | vocodeur de phase par pic (Laroche & Dolson) | `init set_ratio set_swell hold set_hold_release tune set_hints process` ; privé : `_analyse _process_frame _try_parametric _render_hinted _kernel _fft` |
| `stream_multivocoder.hpp` | 21 | multi-résolution (2 ou 3 fenêtres + crossover LR8) | même interface que `StreamVocoder` + `set_xover prony prony_fmin long_only peak_fmax hint_rotw` |
| `stream_shifter.hpp` | 8 | granulaire SOLA 2 taps | `setup set_ratio set_lag_ratio reset process` ; privé : `_lag0 _read _aligned` |
| `octave_anchor.hpp` | 5 | ancrage d'octave (§28, désactivé par défaut) | `init reset set_ratio process` |
| `stream_filterbank.hpp` | 5 | spike expérimental, banc de filtres | `init reset set_ratio process channels` |
| `freeze_loop.hpp` | 5 | boucle de freeze avec couture fondue | `init reset armed capture process` |
| `biquad.hpp` `envelope.hpp` `onset_detector.hpp` `delay_line.hpp` | 3–5 chacun | briques | `setup/set process reset` |

Wrappers : `plugin.cpp` (LV2, 7 fonctions) et `juce/` (1 461 lignes, 44 définitions,
dont 22 pour l'éditeur).

Point structurel notable : la complexité de ce projet n'est pas dans le nombre
de fonctions, elle est dans les **directives de préprocesseur**. `pogged_dsp.cpp`
ouvre 53 blocs conditionnels, souvent imbriqués, sur 5 macros (`POGGED_NO_VOCODER`,
`POGGED_PV_N`, `POGGED_DYN_FOCUS`, `POGGED_FREEZE_SMOOTH`, `POGGED_HYB_SWELL`).
L'inventaire indique la garde de chaque définition pour cette raison. C'est aussi
d'où viennent les incohérences I1 à I3.

---

## 2. Incohérences

Classées par gravité. I1 est vérifiée par compilation, I7 et les mesures de la
section 3 sont chiffrées ; le reste est de la lecture croisée code/`.ttl`/docs.

### I1 — `HYBRID=1` ne compile pas sur les deux cibles MOD *(bloquant)*

Le README annonce que `HYBRID` « works with any TARGET ». En réalité :

```
make TARGET=moddwarf-new HYBRID=1   → error: 'SubVocoder' does not name a type
make TARGET=modduox-new  HYBRID=1   → error: StreamVocoderT<2048> has no member named 'long_only'
```

Cause : le membre §37b `SubVocoder pv_fund;` (`pogged_dsp.cpp:428`) est déclaré
avec un alias qui n'existe que dans le build multi-résolution. `POGGED_NO_VOCODER`
supprime l'alias ; `POGGED_PV_N` le fait pointer sur `StreamVocoder`, qui n'a pas
de `long_only()`. Les deux blocs `#ifdef POGGED_DYN_FOCUS` autour de `pv_fund`
n'ont pas de garde `#ifndef POGGED_NO_VOCODER` / `#ifndef POGGED_PV_N`.

C'est justement la combinaison qui intéresserait le Duo X : quad A53, vocodeur
épinglé à 2 048 pour le CPU, et le hybride est *le* mode qui rend le vocodeur
utilisable en jeu. Consigné dans `tools/eval/known_failures.txt` en attendant
un correctif.

### I2 — le LV2 livré et le VST3/AU livré ne sont pas le même plugin

`juce/CMakeLists.txt` code en dur `POGGED_DYN_FOCUS` + les six réglages hybrides
validés. Le `make` par défaut — celui du README, celui de `make install`, celui
de la CI — n'en définit aucun. Trois conséquences visibles par l'utilisateur, à
`.ttl` identique :

- `pogged.ttl` donne `focus` **defaut = 2 (Hybrid)**. Dans le build LV2 par
  défaut, `focus` est clampé à `[0,1]` (`pogged_dsp.cpp:886`) : le réglage par
  défaut du plugin livré est donc silencieusement « Vocoder ». Le commentaire du
  `.ttl` documente le repli, mais le résultat reste qu'un POG installé depuis
  `make install` n'a jamais de mode hybride.
- Le port `sustain` (idx 37, 0–5 000 ms, présent dans le `.ttl` **et** dans la
  modgui) est totalement inerte dans ce build : tout le §37 vit derrière
  `POGGED_DYN_FOCUS`. Seul le README le dit ; ni le `.ttl` ni la modgui.
- Le Freeze n'a pas le même son des deux côtés (maintien spectral vs boucle).

Deux sorties possibles, l'une ou l'autre : aligner les défauts du Makefile sur
ceux du CMake, ou marquer les ports concernés dans le `.ttl`.

### I3 — la CI ne compile qu'un point de la matrice

`.github/workflows/build.yml` lance `make` puis `make audit`, sans aucun flag.
Tout ce qui est derrière `POGGED_DYN_FOCUS` — moteur hybride, sustain §37,
freeze spectral §38, c'est-à-dire le code le plus récent, le plus complexe, et
celui que le VST3/AU embarque réellement — n'est **ni compilé ni audité** en CI.
I1 est passée inaperçue pour cette raison exacte.

C'est ce que corrige `tools/eval/build_matrix.sh` : 21 configurations compilées,
avec baseline de régressions connues.

### I4 — `delay_line.hpp` n'est pas dans les dépendances du Makefile

`HEADERS` (Makefile:166) liste 10 en-têtes, dont `stream_filterbank.hpp` — que
`pogged_dsp.cpp` **n'inclut pas** (c'est un spike, utilisé seulement par
`tools/`). En revanche `delay_line.hpp`, qui est inclus et qui porte tout le
SPREAD, **n'y est pas**. Modifier `delay_line.hpp` ne déclenche donc aucune
reconstruction : `make` répond « nothing to do » et le `.so` garde l'ancien code.

### I5 — un port `sustain_ms` est documenté à trois endroits, il n'existe pas

- `README.md` : « Sustain on/off and its release are the `sustain` / `sustain_ms`
  ports »
- `Makefile:136` : idem
- `pogged_dsp.cpp:347` : « a runtime feature (the `sustain` / `sustain_ms` ports) »

Il n'y a qu'un port `sustain` (idx 37) qui porte les deux rôles — ce que
`pogged_dsp.h` décrit correctement, et ce que le code fait. Reliquat d'un design
à deux ports abandonné.

### I6 — `pogged_dsp.h` décrit `focus` comme `[0/1]`

`pogged_dsp.h:55` : `float focus; /* idx 28 [0/1] ... */`. Le `.ttl`, le
paramètre JUCE et le DSP traitent tous 0/1/2. C'est l'en-tête public du cœur :
c'est là que va lire quelqu'un qui embarque le core dans un autre hôte.

### I7 — ~1,8 Mio par instance alloués et jamais traités

`PoggedVocoder pv[N_VOICES]` en alloue 8 ; les deux emplacements sub ne sont
jamais initialisés ni traités (les subs tournent sur `pv_sub[]`, §27). Mesuré par
`make eval-cpu` :

```
MultiVocoder<4096,2048,8>       824 056 B   × 2 emplacements morts = 1,57 Mio
OctaveAnchor<>                  120 952 B   × 2 (ANCHOR_MIX = 0)   = 0,23 Mio
```

Soit ≈ 1,8 Mio sur les ≈ 8 Mio d'une instance. Ce sont des tableaux membres de
taille fixe (aucun tas), donc invisibles pour un profileur d'allocation et bien
visibles en RSS — et en pression de cache sur un Dwarf ou un Duo X. Un tableau
de 6 avec une table d'indirection, ou `anc[]` sous `#if POGGED_ANCHOR_MIX > 0`,
suffit.

### I8 — `pv_sub[]` et `anc[]` sont indexés par l'enum `Voice`

`p->pv_sub[v]` et `p->anc[v]` sont écrits avec `v == V_SUB1` / `V_SUB2` sur des
tableaux de taille 2. Cela ne marche que parce que ces deux énumérateurs valent
0 et 1 — alors que le commentaire de `act[]` (`pogged_dsp.cpp:989`) affirme
explicitement que l'enum « is free to be reordered ». Une réorganisation de
`Voice` déborderait `pv_sub[2]` sans le moindre diagnostic. Un
`static_assert(V_SUB1 == 0 && V_SUB2 == 1)` réconcilierait les deux affirmations
pour le prix d'une ligne.

### I9 — API morte et artefact commité

- `MultiVocoder::peak_fmax()` et `MultiVocoder::hint_rotw()` : aucun appelant,
  nulle part (§26 est explicitement « RETIRED » dans les commentaires).
- `build_err.txt` est versionné à la racine : c'est une erreur de compilation
  locale d'une machine sans `lv2-dev`. Le `.gitignore` ne l'attrape pas.

### Non-défauts vérifiés

Deux points qui *ressemblent* à des bugs et n'en sont pas — notés pour éviter
qu'ils soient « corrigés » plus tard :

- `set_hold_release()` n'est appelé que dans les branches `frz_smooth` /
  `sustain_on`, donc jamais restauré quand les deux retombent. Sans effet :
  chaque `hold(true)` est précédé, dans le même bloc, de la branche qui vient de
  poser la release. Le couplage est implicite, pas cassé.
- Le budget de lag granulaire tient dans le ring dans le pire cas documenté
  (voix +2 warpée à l'octave, grains taille basse : ≈ 8 750 échantillons pour un
  ring de 16 384 à 48 kHz ; la marge se conserve à 96 et 192 kHz).

---

## 3. Optimisations CPU

Toutes les mesures : x86-64, g++ 13.3, `-O3 -ffast-math`, blocs de 64
échantillons à 48 kHz (échéance **1 333 µs/bloc**), six voix ouvertes + detune +
spread, entrée = accord de mi à quatre notes replaqué chaque seconde. Reproduire
avec `make eval-cpu`. Ce qui compte pour un thread temps réel est le **pire
bloc**, pas la moyenne : les deux sont donnés.

Référence, build LV2 par défaut :

| Focus | moyenne | p99 | pire bloc |
|---|---:|---:|---:|
| 0 granulaire | 26 µs (2,0 %) | 111 µs (8,3 %) | 213 µs (16,0 %) |
| 1 vocodeur | 191 µs (14,3 %) | 452 µs (33,9 %) | 660 µs (49,5 %) |

### C1 — en build hybride, choisir « Granular » coûte le prix du vocodeur *(gain le plus important)*

`voice_raw` (`pogged_dsp.cpp:1280`) force les deux moteurs à chaque échantillon :

```cpp
#ifdef POGGED_DYN_FOCUS
    constexpr bool always = true;
#else
    constexpr bool always = false;
#endif
```

Le commentaire justifie ce choix pour le mode 2 : `g_focus` bascule à chaque
attaque, sauter un moteur figerait son état streaming. C'est exact — **pour le
mode 2**. Les modes 0 et 1 ont une cible fixe et un crossfade lent, exactement
comme dans le build statique, et les gardes existantes `g_gran > 1e-4f` /
`g_voc > 1e-4f` couvrent déjà la traversée des 150 ms. Le `constexpr` est trop
large d'un mode.

Prototype mesuré (`const bool always = (fsel >= 1.5f);`), flags JUCE/hybride :

| Focus | avant | après | |
|---|---:|---:|---|
| 0 granulaire | 105 µs / pire 380 µs | **29 µs / pire 130 µs** | **−72 % / −66 %** |
| 1 vocodeur | 104 µs / pire 380 µs | 113 µs / pire 369 µs | inchangé (bruit) |
| 2 hybride | 105 µs / pire 409 µs | 106 µs / pire 396 µs | inchangé |

C'est le build que le VST3/AU embarque, et « Granular » est précisément le mode
qu'un utilisateur de Pi ou de MOD choisit *pour économiser du CPU* — aujourd'hui
il paie plein tarif.

### C2 — l'enveloppe de grain : 32 % du moteur granulaire en `std::cos`

`StreamShifter::process` évalue deux `std::cos` par échantillon et par voix
(`stream_shifter.hpp:117`). Mesuré, une voix, 960 000 échantillons :
`process` = 28 874 µs, dont **9 224 µs (32 %)** pour la paire de cosinus.

Or le curseur est un **entier** dans `[0, grain)`. Deux corrections, chacune
suffisante à elle seule :

- **complémentarité** — les taps sont décalés de `grain/2` et les enveloppes de
  Hann somment exactement à 1 (le commentaire de la classe le dit lui-même) :
  `amp[1] = 1.0f - amp[0]`. Une ligne, exact, moitié du coût.
- **table** — une table de Hann de `grain` entrées construite dans `setup()`
  (≤ 7 680 flottants au plafond de 160 ms) supprime le reste.

Jusqu'à ≈ 30 % du moteur granulaire. Sur MOD Dwarf (`POGGED_NO_VOCODER`) le
granulaire *est* le plugin entier.

### C3 — le build LV2 par défaut tourne en OS=8, le JUCE livré en OS=4

`POGGED_PV_OS` vaut 8 par défaut dans `pogged_dsp.cpp:30` ; seul le chemin
`HYBRID=1` du Makefile le met à 4. Le build LV2 nominal paie donc deux fois plus
de trames FFT que le VST3/AU livré. Mesuré, mode vocodeur :

| | moyenne | p99 | pire bloc |
|---|---:|---:|---:|
| OS=8 (défaut LV2) | 188 µs | 445 µs | 546 µs (41 %) |
| OS=4 | **127 µs** | 350 µs | **431 µs (32 %)** |
| | **−33 %** | −21 % | **−21 %** |

Le README documente déjà que « the user confirmed OS=8≈OS=4 by ear », et le
CMake a tranché dans ce sens. Passer le défaut à 4 rend un tiers du budget
vocodeur sur desktop et Pi sans rien changer d'audible.

### C4 — le build JUCE est ~25 % plus lent que le build LV2, à code identique

Mêmes flags DSP, blocs de 64 :

| flags de compilation | moyenne |
|---|---:|
| `-O3 -ffast-math` (Makefile) | 110 µs |
| `-O3` (JUCE : `juce_recommended_config_flags` n'ajoute pas de fast-math) | 138 µs (**+25 %**) |

`nm` confirme que l'objet sans fast-math appelle l'assistant hors-ligne
`__mulsc3` (multiplication complexe avec traitement NaN/Inf) — et que
`-fcx-limited-range` **ou** `-ffast-math` le fait disparaître :

```
-O3                        __mulsc3: 1
-O3 -fcx-limited-range     __mulsc3: 0
-O3 -ffast-math            __mulsc3: 0
```

Les papillons de `_fft` sont exclusivement des `std::complex<float>` : c'est là
que l'appel se paie. Sur cette machine x86 `-fcx-limited-range` seul est resté
dans le bruit ; sur un cœur ARM in-order (A35/A53) le coût d'appel pèse
nettement plus. À ajouter dans `juce/CMakeLists.txt` sur la TU DSP, puis à
re-mesurer sur le Pi — pas à supposer.

### C5 — micro-gains dans les boucles internes du vocodeur

Petits pris isolément, tous gratuits :

- `_fft` : `if (inverse) w = std::conj(w);` est dans la boucle de papillon la
  plus interne. Templater `_fft` sur `inverse`, ou conjuguer la table une fois.
- OLA : `(_out_write + 2*i) % OUTBUF`, deux fois par échantillon de sortie
  (`stream_vocoder.hpp:608`). `OUTBUF` est une puissance de deux et les opérandes
  sont positifs — `& (OUTBUF-1)` est équivalent et moins cher, le `%` signé
  obligeant le compilateur à émettre la correction de signe. 4 096 par trame.
- `_analyse` écrit `_hist[_hist_idx][k]` pour **chaque** bin de **chaque** trame,
  alors que l'historique n'est lu que par `_try_parametric` — que les voix sub
  désactivent explicitement (`prony(false, false)`). Conditionner l'écriture à
  `PRONY_ON` économise `BINS` stockages complexes par trame, et rendre `_hist`
  conditionnel économise 131 Kio par instance.
- `_aligned` calcule un `std::sqrt` par décalage candidat (`r / sqrt(e)`).
  Comparer `r·|r| / e` donne le même classement sans racine.

**Ne pas** chercher un `atan2` rapide : mesuré, la paire `std::abs` + `std::arg`
par bin ne représente que **3 %** d'une trame. Le coût est dans les deux FFT.

### C6 — `OctaveAnchor` : jusqu'à 128 `std::sin` par échantillon

`OctaveAnchor::process` (`octave_anchor.hpp:65`) somme un `std::sin()` par
emplacement actif de la grille, plafonné à `MAXACT = 128`. La fonctionnalité est
livrée désactivée (`ANCHOR_MIX = 0`, coût nul), mais quiconque compile
`ANCHOR=0.35` pour l'auditionner — ce que le Makefile invite explicitement à
faire — paie 128 sinus libm par échantillon. Un oscillateur récursif par
emplacement (phaseur unitaire, deux multiplications par échantillon) ou une table
de sinus partagée rendrait la fonctionnalité auditionnable, ce qui est le
préalable à la juger.

### Ordre de traitement suggéré

| | Gain mesuré | Coût | Risque |
|---|---|---|---|
| C3 (`PV_OS=4` par défaut) | −33 % vocodeur LV2 | une ligne de Makefile | nul (déjà tranché à l'oreille) |
| C1 (`always` au runtime) | −72 % en Focus granulaire | une ligne | faible, `focus_test` couvre le crossfade |
| C2 (Hann complémentaire) | −16 % granulaire | une ligne | nul (identité exacte) |
| C2 (table de Hann) | −32 % granulaire | ~10 lignes | faible |
| C5 (micro-gains) | non chiffré isolément | ~20 lignes | faible |
| C4 (`-fcx-limited-range`) | à mesurer sur cible | une ligne de CMake | à valider par `make audit` |

---

## 4. Le workflow

```sh
make eval               # les trois passes
make eval-matrix        # 1. compile les 21 configurations documentées
make eval-functions     # 2. régénère docs/function-inventory.md
make eval-cpu           # 3. profil CPU + empreinte mémoire
```

`make eval-cpu` hérite des `CXXFLAGS` du plugin : `make eval-cpu TARGET=rpi5
HYBRID=1` profile ce que ce build-là fera réellement.

En CI, `.github/workflows/code-eval.yml` tourne sur les pull requests, à la
demande, et le lundi matin (pour attraper la rouille due aux mises à jour de
toolchain, pas aux commits) :

- **matrice de build** — bloquante. Échoue sur une configuration cassée absente
  de `tools/eval/known_failures.txt`, **et** sur une configuration listée qui
  s'est remise à compiler. La liste ne peut donc que rétrécir, et seulement
  volontairement.
- **inventaire** — bloquant. Régénéré puis comparé à la copie versionnée : une
  documentation qui dérive est pire que pas de documentation.
- **profil CPU** — informatif, jamais bloquant (deux fois : flags LV2 par défaut
  et flags JUCE). Un runner GitHub partagé est la mauvaise machine pour asserter
  un budget temps réel ; les résultats vont dans le résumé de job et en artefact,
  pour comparaison entre runs.

Les trois outils sont sans dépendance externe (`ctags` n'existe pas sur les
toolchains Buildroot de MOD) et ne compilent que `pogged_dsp.cpp`, qui n'a pas
besoin des en-têtes LV2 — la matrice tourne donc partout où tourne un compilateur
C++17.
