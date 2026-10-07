# Évaluation du code — Pogged

Deux passes, 2026-07-25 : un relevé sur `master` (0de152f), puis les correctifs.
Chaque constat porte son statut. Le workflow qui régénère tout ça est décrit à
la fin et tourne en CI (`.github/workflows/code-eval.yml`).

`make audit` répond à « est-ce que ça sonne encore juste ». Ce document répond
aux trois autres questions, celles qu'aucun test n'assertait :

1. **qu'est-ce qu'il y a dedans** — inventaire des fonctions ;
2. **qu'est-ce qui est incohérent** — écarts entre le code, les `.ttl`, le
   Makefile, le CMake et le README ;
3. **qu'est-ce que ça coûte** — profil CPU mesuré, et où le récupérer.

---

## 1. Inventaire

**143 définitions** (fonctions libres, méthodes, constructeurs) sur ~8 800
lignes. Liste complète et à jour : [`docs/function-inventory.md`](function-inventory.md),
régénérée par `make eval-functions` et vérifiée en CI.

Vue d'ensemble du cœur DSP (`src/`, ~4 100 lignes) :

| Fichier | Défs | Rôle | API publique |
|---|---:|---|---|
| `pogged_dsp.cpp` | 11 | orchestration : ring, mixage, pan, filtre, dry | `pogged_dsp_new/free/reset/process` + 7 helpers statiques |
| `stream_vocoder.hpp` | 19 | vocodeur de phase par pic (Laroche & Dolson) | `init set_ratio set_swell hold set_hold_release tune set_hints process` ; privé : `_analyse _process_frame _try_parametric _render_hinted _kernel _fft` |
| `stream_multivocoder.hpp` | 19 | multi-résolution (2 ou 3 fenêtres + crossover LR8) | même interface que `StreamVocoder` + `set_xover prony prony_fmin long_only` |
| `stream_shifter.hpp` | 9 | granulaire SOLA 2 taps | `setup set_ratio set_lag_ratio reset process` ; privé : `_set_env_rate _lag0 _read _aligned` |
| `octave_anchor.hpp` | 5 | ancrage d'octave (§28, non compilé sans `ANCHOR=`) | `init reset set_ratio process` |
| `stream_filterbank.hpp` | 5 | spike expérimental, banc de filtres | `init reset set_ratio process channels` |
| `freeze_loop.hpp` | 5 | boucle de freeze avec couture fondue | `init reset armed capture process` |
| `biquad.hpp` `envelope.hpp` `onset_detector.hpp` `delay_line.hpp` | 3–5 chacun | briques | `setup/set process reset` |

Wrappers : `plugin.cpp` (LV2, 7 fonctions) et `juce/` (~1 460 lignes, 44
définitions, dont 22 pour l'éditeur).

Point structurel : la complexité de ce projet n'est pas dans le nombre de
fonctions, elle est dans les **directives de préprocesseur**. `pogged_dsp.cpp`
ouvre une cinquantaine de blocs conditionnels sur 5 macros
(`POGGED_NO_VOCODER`, `POGGED_PV_N`, `POGGED_DYN_FOCUS`, `POGGED_FREEZE_SMOOTH`,
`POGGED_HYB_SWELL`). L'inventaire indique la garde de chaque définition pour
cette raison, et c'est de là que venaient I1 à I3.

---

## 2. Incohérences

### I1 — `HYBRID=1` ne compilait pas sur les deux cibles MOD — **corrigé (politique)**

Le README annonçait « works with any TARGET » ; en réalité
`make TARGET=moddwarf-new HYBRID=1` et `make TARGET=modduox-new HYBRID=1`
échouaient à la compilation (le membre §37b `pv_fund` est déclaré `SubVocoder`,
alias qui n'existe que dans le build multi-résolution).

Décision : les deux cartes MOD **ne doivent pas** avoir l'hybride — le Dwarf n'a
pas de vocodeur du tout et le Duo X épingle une fenêtre unique justement pour
tenir son budget ; l'hybride demande les deux moteurs vivants à chaque attaque.
`HYBRID` devient donc un **défaut par cible** : 1 partout (desktop, Pi 5, JUCE),
0 sur les cartes MOD, et le demander sur une carte MOD est une erreur `make`
explicite plutôt qu'une erreur C++ trois écrans plus bas. La matrice de build
vérifie ce refus (`MUSTFAIL`), ce n'est plus une panne tolérée.

### I2 — le LV2 livré et le VST3/AU livré n'étaient pas le même plugin — **corrigé**

`juce/CMakeLists.txt` codait en dur `POGGED_DYN_FOCUS` + les six réglages
hybrides validés ; le `make` par défaut n'en définissait aucun. À `.ttl`
identique, le port `focus` avait pour défaut « Hybrid » qui se lisait
silencieusement « Vocoder », le port `sustain` était totalement inerte, et le
Freeze n'avait pas le même son des deux côtés.

`HYBRID=1` étant désormais le défaut sur toute cible qui peut le porter, le LV2
desktop/Pi et le VST3/AU embarquent le même moteur.

### I3 — la CI ne compilait et n'auditait qu'un point de la matrice — **corrigé**

Deux problèmes distincts, le second plus grave que le premier :

- `build.yml` lançait `make` puis `make audit` sans aucun flag. Tout ce qui vit
  derrière `POGGED_DYN_FOCUS` n'était jamais compilé. C'est ainsi que I1 est
  passée.
- **`make audit` ignorait `CXXFLAGS` de toute façon.** `AUDIT_FLAGS` valait
  `-O2 -std=c++17 -Isrc`, sans les défines : les harnais étaient compilés avec
  le jeu de macros par défaut quoi qu'on demande, donc `make audit HYBRID=1`
  auditait exactement la même chose que `make audit`. Le chemin hybride n'avait
  jamais été mesuré, par personne.

`AUDIT_FLAGS` reprend maintenant les `-D`/`-U` du build, et la CI audite quatre
moteurs en matrice : hybride (défaut desktop/Pi), `HYBRID=0`, moteur Dwarf
(sans vocodeur) et moteur Duo X (fenêtre 2048 épinglée).

Faire tourner l'audit sur ces moteurs a immédiatement montré que **trois
harnais avaient des seuils calibrés sur un seul moteur** — vérifié en les
relançant sur le code d'avant les correctifs, où ils échouent à l'identique :
ce sont des lacunes de test préexistantes, pas des régressions.

| Harnais | Ce qui cassait | Correctif |
|---|---|---|
| `focus_test` | exigeait `voc < gran - 2.0`, un écart absolu — donc en fait une assertion sur la qualité du **granulaire**, qui a cassé le jour où le granulaire s'est amélioré (2,4 dB avec les grains §34 contre 4,3 avant), pendant que le vocodeur tenait +0,1 dB au-dessus du plancher | critères exprimés **par rapport au plancher** : vocodeur près du plancher, granulaire clairement au-dessus. Et sous `POGGED_PV_N` la borne suit la fenêtre : 2048 mesure +2,3 dB sur un accord grave, propriété documentée de la fenêtre, pas une régression |
| `range_test` | « le mode baryton ne doit pas être pire que guitare » comparait 0,2 dB à 0,3 dB — du bruit de mesure, parce que les grains §34 suppriment déjà l'ondulation dans **les deux** modes | assertion conditionnée : la comparaison là où l'ondulation est mesurable, l'**absence** d'ondulation là où elle ne l'est pas. Dans les deux cas quelque chose est asserté |
| `polyswell_test` | assertait le swell par bin du vocodeur y compris sous `POGGED_NO_VOCODER`, où il n'y a pas de vocodeur ; et la borne de tenue du sub (−2,0 dB) n'avait aucune marge (−2,04 mesuré en hybride) | sortie anticipée en report-only sans vocodeur, comme `focus_test` le faisait déjà ; borne du sub à −2,5 dB **uniquement** en hybride, où son vocodeur est long-window-only (§30), avec les trois mesures en commentaire |

Les quatre moteurs passent maintenant `make audit`.

### I4 — `delay_line.hpp` absent des dépendances du Makefile — **corrigé**

`HEADERS` listait `stream_filterbank.hpp`, que le plugin n'inclut pas, et
oubliait `delay_line.hpp`, qui porte tout le SPREAD : le modifier ne déclenchait
aucune reconstruction. Remplacé par `$(wildcard src/*.h src/*.hpp)` —
reconstruire un peu trop souvent est sans conséquence pour un plugin compilé en
une commande, rater un en-tête ne l'est pas.

### I5 — un port `sustain_ms` documenté à trois endroits, inexistant — **corrigé**

README, `Makefile` et `pogged_dsp.cpp` parlaient des ports « `sustain` /
`sustain_ms` ». Il n'y a qu'un port `sustain` (idx 37) qui porte les deux rôles.
Reliquat d'un design à deux ports abandonné ; les trois mentions sont nettoyées
et `pogged_dsp.h` le dit maintenant explicitement.

### I6 — `pogged_dsp.h` décrivait `focus` comme `[0/1]` — **corrigé**

Devenu `[0/1/2]`, avec la note que 2 exige un build `POGGED_DYN_FOCUS` et se lit
comme 1 ailleurs. C'est l'en-tête public du cœur : c'est là que lit quiconque
embarque le core dans un autre hôte.

### I7 — ~1,8 Mio par instance alloués et jamais traités — **corrigé**

`PoggedVocoder pv[N_VOICES]` en allouait 8 ; les deux emplacements sub n'étaient
ni initialisés ni traités (les subs tournent sur `pv_sub[]`, §27). `pv[]` est
maintenant dimensionné `N_VOICES - 2` et indexé via une table `PV_IDX`.

Et les deux `OctaveAnchor` (121 Kio chacun) n'étaient plus « compilés en une
constante nulle » comme le prétendait le commentaire : le calcul par échantillon
disparaissait bien, les membres non. Seul le préprocesseur peut retirer un
membre, donc l'anchor n'est plus **construit du tout** sans `ANCHOR=` — voir C6.

### I8 — `pv_sub[]` et `anc[]` indexés par l'enum `Voice` — **corrigé**

Cela ne marchait que parce que `V_SUB1`/`V_SUB2` valent 0 et 1, alors que le
commentaire de `act[]` affirme que l'enum est réorganisable. Un
`static_assert(V_SUB1 == 0 && V_SUB2 == 1)` met l'invariant là où le compilateur
l'applique, avec un second sur `PV_IDX`.

### I9 — API morte et artefact commité — **corrigé**

`MultiVocoder::peak_fmax()` et `hint_rotw()` n'avaient aucun appelant (§26 est
explicitement « RETIRED ») : retirés, avec une note sur ce qui a existé là. Les
membres qu'ils réglaient restent publics sur `StreamVocoderT` pour un harnais
hors-ligne. `build_err.txt` — une erreur de compilation locale d'une machine
sans `lv2-dev` — est supprimé et ajouté au `.gitignore`.

### I10 — `make GRAIN_UP_PER=0` n'a jamais compilé — **corrigé** *(trouvé par la matrice)*

Constat de la nouvelle matrice de build, pas de la relecture. Le Makefile
construisait ses littéraux à la main (`-DPOGGED_GRAIN_UP_PERIODS=$(GRAIN_UP_PER)f`),
ce qui ne marche que pour la forme exacte du défaut : `GRAIN_UP_PER=0` produisait
`0f`, qui n'est pas un littéral C++. La façon documentée de restaurer l'ancien
grain fixe était donc morte, et la même fragilité touchait `GRAIN_PER=2`,
`HYB_FLOOR=0`, `ANCHOR=1`, `GRAIN_UP=10.5`. Les suffixes sont supprimés : chacune
de ces macros initialise un `static constexpr float`, la conversion se fait côté
C++ et n'importe quelle valeur raisonnable passe.

### I11 — le `.mk` mod-plugin-builder livrait le mauvais moteur — **corrigé**

Le plus sérieux des constats de packaging, trouvé en vérifiant le `.mk` :

- il passait `CXXFLAGS=` en ligne de commande, ce qui **écrase** le
  `CXXFLAGS ?=` par cible du Makefile — là où vivait `-DPOGGED_NO_VOCODER`. La
  défine était silencieusement perdue et **le Dwarf était compilé avec le
  vocodeur même que son A35 ne peut pas porter** ;
- il codait `TARGET=moddwarf-new` en dur, donc `./build modduox-new pogged`
  compilait le moteur du Dwarf au lieu de celui du Duo X.

Les défines de feature sont passées en `override +=` dans le Makefile (elles
survivent à n'importe quel `CXXFLAGS` externe) et le `.mk` dérive sa cible du
`BR2_GCC_TARGET_CPU` de Buildroot — Dwarf = Cortex-A35, Duo X = Cortex-A53,
la même chose qui choisit déjà le `-mcpu`. **À confirmer sur un arbre
mod-plugin-builder réel** : le mapping est dérivé d'une variable Buildroot
standard, il n'a pas pu être exécuté ici.

### Non-défauts vérifiés

Notés pour éviter qu'ils soient « corrigés » plus tard :

- `set_hold_release()` n'est appelé que dans les branches `frz_smooth` /
  `sustain_on`, donc jamais restauré quand les deux retombent. Sans effet :
  chaque `hold(true)` est précédé, dans le même bloc, de la branche qui vient de
  poser la release. Le couplage est implicite, pas cassé.
- Le budget de lag granulaire tient dans le ring dans le pire cas documenté
  (voix +2 warpée à l'octave, grains taille basse : ≈ 8 750 échantillons pour un
  ring de 16 384 à 48 kHz ; la marge se conserve à 96 et 192 kHz).

---

## 3. CPU

Mesures : x86-64, g++ 13.3, `-O3 -ffast-math`, blocs de 64 échantillons à
48 kHz (échéance **1 333 µs/bloc**), six voix ouvertes + detune + spread, entrée
= accord de mi à quatre notes replaqué chaque seconde. `make eval-cpu` reproduit.
Ce qui compte pour un thread temps réel est le **pire bloc**, pas la moyenne.

**Avant / après, sur la configuration livrée** (flags hybrides — ceux du VST3/AU,
et désormais aussi ceux du LV2 desktop/Pi), deux passes concordantes :

| Focus | avant (moy. / p99 / pire) | après | |
|---|---|---|---|
| 0 granulaire | 115 / 302 / 450 µs | **32 / 107 / 154 µs** | **−73 % / −65 % / −66 %** |
| 1 vocodeur | 122 / 354 / 425 µs | 113 / 328 / 408 µs | ≈ inchangé |
| 2 hybride | 119 / 344 / 478 µs | 109 / 291 / 409 µs | ≈ inchangé |

### C1 — Granular coûtait le prix du vocodeur — **corrigé**, c'est tout le gain ci-dessus

`voice_raw` forçait les deux moteurs à chaque échantillon dans **tous** les modes
(`constexpr bool always = true`). Le commentaire justifiait ce choix pour le mode
hybride, où `g_focus` bascule à chaque attaque — c'est exact, mais seulement pour
le mode 2. Les modes 0 et 1 ont une cible fixe atteinte par le crossfade lent, et
les gardes `> 1e-4f` couvrent déjà les 150 ms de fondu, ce qui est précisément ce
qui laisse à l'OLA du vocodeur le temps de se remplir. `always` devient
`(fsel >= 1.5f)`.

### C2 — l'enveloppe de grain, 32 % du moteur granulaire en `std::cos` — **corrigé**

Deux substitutions **exactes**, pas des approximations :

- le Hann du tap 0 vient d'un phaseur unitaire tourné de 2π/g par échantillon et
  resynchronisé à (1,0) à chaque respawn, donc l'erreur de rotation ne peut pas
  s'accumuler au-delà d'un grain (~1e-5 au pire) ;
- celui du tap 1 vaut **exactement** `1 - amp0` (les curseurs sont verrouillés à
  une demi-grain, `cos(θ+π) = -cos θ`). Le grain est forcé pair pour que ce
  décalage soit un nombre entier d'échantillons — ce qui rend l'invariant « les
  enveloppes somment à 1 », sur lequel toute la classe repose, exact par
  construction, là où deux `cos()` indépendants ne l'atteignaient qu'à
  l'arrondi flottant près.

Le profil garde le chronométrage d'une paire de `cos` à côté du vrai code, comme
chiffre permanent de ce que coûterait un retour en arrière (~36 % ici).

### C3 — `PV_OS=4` sur tous les builds — **corrigé**

`POGGED_PV_OS` valait 8 par défaut et seul le chemin `HYBRID=1` le mettait à 4 :
le LV2 nominal payait deux fois plus de trames FFT que le VST3/AU livré. Mesuré
en mode vocodeur : moyenne 188 → 127 µs (**−33 %**), pire bloc 546 → 431 µs
(−21 %). Le README documentait déjà que « the user confirmed OS=8≈OS=4 by ear ».
`PV_OS ?= 4` s'applique maintenant à toute cible ; `make PV_OS=8` restaure les
trames denses.

### C4 — le build JUCE est ~25 % plus lent que le build LV2 — **ouvert**

Mêmes flags DSP : `-O3 -ffast-math` 110 µs de moyenne contre `-O3` 138 µs
(**+25 %**). `nm` confirme que l'objet sans fast-math appelle l'assistant
hors-ligne `__mulsc3` (multiplication complexe avec traitement NaN/Inf), et que
`-fcx-limited-range` **ou** `-ffast-math` le fait disparaître :

```
-O3                        __mulsc3: 1
-O3 -fcx-limited-range     __mulsc3: 0
-O3 -ffast-math            __mulsc3: 0
```

Les papillons de `_fft` sont exclusivement des `std::complex<float>` : c'est là
que l'appel se paie. **Laissé ouvert délibérément** : sur cette machine x86,
`-fcx-limited-range` seul est resté dans le bruit, et un flag de mathématiques
flottantes sur le chemin DSP se valide sur la cible et à l'oreille, pas sur un
runner. À reprendre sur le Pi, `make audit` à l'appui.

### C5 — micro-gains dans les boucles internes — **corrigés**

- `_fft` : la conjugaison inverse sortie de la boucle de papillon la plus interne
  (elle y branchait `M/2·log2(M)` fois par transformée) — le signe est un
  constant par appel, résolu une fois.
- OLA : `% OUTBUF` remplacé par un masque, `OUTBUF` étant une puissance de deux
  et les index positifs, avec `static_assert`. 4 096 opérations par trame.
- `_analyse` : l'historique Prony n'est plus écrit quand `PRONY_ON` est faux —
  ce qui est exactement le cas des voix sub (`prony(false, false)`, §27), pour
  qui c'était `BINS` stockages complexes par trame de pur gaspillage.
- `_aligned` : classement sur `r·|r|/e` au lieu de `r/√e`, même ordre (`x·|x|`
  est strictement croissante et `e > 0`), une racine de moins par décalage
  candidat, et il y en a `_align/2` par respawn.

Ces gains sont petits devant les deux FFT, ce qui explique que les colonnes
vocodeur du tableau bougent peu. Mesuré pour ne **pas** le recommander : la
paire `std::abs` + `std::arg` par bin ne pèse que **4 %** d'une trame — inutile
de chercher un `atan2` rapide.

### C6 — l'anchor reste à 0 par défaut, et redevient auditionnable — **corrigé**

`ANCHOR=` non passé, l'anchor n'est plus compilé du tout (2 × 121 Kio de
membres en moins, cf. I7). Passer n'importe quelle valeur — `make ANCHOR=0.35` —
le construit et l'active comme avant. Sa boucle interne reste à un `std::sin()`
par emplacement actif, plafonnée à 128 par échantillon : c'est la première chose
à revoir (oscillateur récursif ou table partagée) si l'audition doit devenir
autre chose qu'une expérience ponctuelle.

---

## 4. Le workflow

```sh
make eval               # les trois passes
make eval-matrix        # 1. compile les configurations documentées
make eval-functions     # 2. régénère docs/function-inventory.md
make eval-cpu           # 3. profil CPU + empreinte mémoire
```

`make eval-cpu` hérite des `CXXFLAGS` du plugin : `make eval-cpu TARGET=rpi5`
profile ce que ce build-là fera réellement.

La matrice **interroge le Makefile** (`make print-flags TARGET=... HYBRID=...`)
au lieu de réécrire sa logique — une matrice qui garde sa propre copie de
l'expansion des flags teste sa copie, pas le build. Elle ne garde que les `-D`/`-U`
(les `-mcpu` d'une cible croisée ne veulent rien dire pour le compilateur hôte, et
la question posée est « quel code compile », donc une question de préprocesseur).
Deux types d'entrée : `BUILD`, qui doit compiler, et `MUSTFAIL`, que le Makefile
doit **refuser** avant qu'un compilateur ne démarre — demander l'hybride sur une
carte MOD est une chose supportée pour laquelle se faire rembarrer, pas un build
supporté.

En CI :

- `build.yml` — construit et audite **quatre moteurs** (hybride, `HYBRID=0`,
  Dwarf, Duo X), plus macOS et Windows.
- `code-eval.yml` — sur les pull requests, à la demande, et le lundi matin (pour
  attraper la rouille due aux mises à jour de toolchain). Matrice de build et
  inventaire bloquants ; profil CPU informatif, jamais bloquant — un runner
  partagé est la mauvaise machine pour asserter un budget temps réel.

`tools/eval/known_failures.txt` est maintenant vide. Le mécanisme reste : la
matrice échoue sur une panne absente du fichier **et** sur une entrée du fichier
qui s'est remise à compiler, donc la liste ne peut que rétrécir, volontairement.

Les trois outils sont sans dépendance externe (`ctags` n'existe pas sur les
toolchains Buildroot de MOD) et ne compilent que `pogged_dsp.cpp`, qui n'a pas
besoin des en-têtes LV2 — la matrice tourne donc partout où tourne un compilateur
C++17.
