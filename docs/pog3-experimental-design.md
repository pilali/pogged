# Note de design — vers un moteur de pitch « à la POG3 »

Branche : `claude/pog3-pitch-shifter-experimental-snrh0l`.

Document de travail, **théorique** (nous n'avons pas la pédale pour mesurer).
Il fige l'analyse avant d'écrire du code : pourquoi les deux moteurs actuels
plafonnent, ce que le comportement observé du POG3 nous apprend sur son
architecture probable, et la direction qu'on retient. Rien ici n'est du son
validé — c'est une feuille de route à confronter à `make audit`.

---

## 0. Le problème, tel qu'il se pose vraiment

Pogged transpose **tout le signal polyphonique**, sans pitch-tracking : chaque
voix est un lecteur à ratio fixe (0.5 / 0.25 / 2^(7/12) / 2 / 4) derrière la
tête d'écriture d'un ring partagé. Deux moteurs commutables (`Focus`) :

| Moteur | Fichier | Latence | Défaut mesuré |
|---|---|---|---|
| Granulaire (SOLA) | `src/stream_shifter.hpp` | 3 ms | +4,8 dB de ripple sur le sub d'un accord |
| Vocodeur per-peak | `src/stream_vocoder.hpp` | 84,5 ms (N=4096) | propre sur accord (+0,0 dB), mais latence + smearing d'attaque |

Le cahier des charges de cette branche demande **les deux à la fois** :
- la latence de quelques ms du POG3,
- un rendu propre par voix, y compris sur accords graves,
- et — critère décisif ci-dessous — la gestion d'attaques multiples en arpège
  **sans perturber la résonance des notes déjà en train de sonner**.

---

## 1. Trois observations sur le POG3, et ce qu'elles éliminent

### 1.1 « Les attaques multiples ne modifient pas les notes tenues »

C'est le critère le plus discriminant, et c'est l'**anti-signature du FFT par
blocs**. Le vocodeur actuel (`stream_vocoder.hpp`) ré-analyse tout le spectre à
chaque hop, re-choisit ses pics (max local ±2 bins) et **re-partitionne les
régions** entre pics :

```
1. Peaks on the ANALYSIS spectrum (local max over ±2 bins).
2. Region boundaries at the magnitude minimum between peaks.
3. Translate each region ...
```

Une nouvelle attaque en arpège injecte de l'énergie → la carte des pics change →
les frontières de région bougent → les partiels **déjà en train de sonner**
changent de région, de phasor (`_rot[]` est per-bin mais incrémenté
*region-wide*), donc de rendu. La résonance des notes tenues bouge. C'est
structurel au couple *block-FFT + peak-picking*, pas un réglage.

→ **Un traitement où chaque canal vit sa vie** (un résonateur / oscillateur par
bande, mis à jour échantillon par échantillon) n'a pas ce défaut : une attaque
n'excite que les canaux concernés ; les autres continuent de sonner intacts.

### 1.2 « Quelques ms de latence » — mais pas *uniforme*

La faute du block-FFT n'est pas sa latence *totale*, c'est qu'elle est
**uniforme sur toute la bande**. Le N=4096 paie 84,5 ms *partout*, alors que
seule la basse en a besoin pour résoudre des partiels serrés. Or l'oreille
tolère très bien un sub en retard et très mal un aigu / transitoire en retard.

Le POG3 « à quelques ms » est très probablement à **latence dépendante de la
fréquence** : aigus et attaques quasi instantanés, octave grave plus lente,
et personne ne le remarque parce que c'est là que l'oreille pardonne.

Un **banc de filtres octave** donne ça *nativement* : étages aigus = filtres
courts = faible latence ; seul l'étage grave paie une fenêtre longue. Un FFT
global en est incapable — une seule taille de fenêtre pour toutes les
fréquences.

### 1.3 « Rendu inégalé sur chaque bande », octaves seulement

Le POG ne produit que des **octaves** (et une quinte). Les shifters classiques
sont généralistes (ratio quelconque) et paient cette généralité. La faible
latence + la propreté du POG viennent très probablement de ce qu'il est
**spécialisé** : décaler d'une octave dans une décomposition dyadique
(sous-bandes espacées en octaves), c'est glisser d'un niveau dans l'arbre —
opération courte, à phase préservée par sous-bande.

---

## 2. La limite qu'on ne contourne pas : Gabor

À poser noir sur blanc pour ne pas se raconter d'histoires : un filtre assez
étroit pour résoudre deux partiels à Δf a un temps d'établissement (ring time /
retard de groupe) de l'ordre de **1/Δf**, **quelle que soit** la technique —
FFT, filtre glissant, ondelette. Le comment de `stream_vocoder.hpp` le chiffre
déjà : sur un Mi majeur grave, partiels à 42,8 Hz → il faut des bins ≤ 11,7 Hz →
fenêtre ≈ 4096 → ~85 ms.

Donc si le POG3 « résout » des accords graves à quelques ms, l'une de ces trois
choses est vraie, et **aucune n'est “battre Gabor”** :

- **(a)** il ne sépare pas réellement les partiels graves serrés — il décale la
  basse en bloc et l'oreille pardonne ;
- **(b)** ses « quelques ms » sont la latence des aigus/attaques, et son octave
  grave est en réalité plus retardée (latence dépendante de la fréquence, §1.2) ;
- **(c)** ses artefacts existent mais sont perceptivement plus doux que ce que
  mesure notre métrique en dB de ripple.

**Hypothèse de travail retenue : (a) + (b).** C'est reproductible ; « battre
Gabor » ne l'est pas. Notre cible n'est donc pas « faible latence partout »
mais **« dépenser le budget de latence uniquement là où l'oreille l'exige »**,
et **traitement continu par canal** pour le critère §1.1.

---

## 3. Familles d'architectures candidates

Toutes « non-FFT-classiques » au sens qui compte (continu / per-canal /
latence variable).

### 3.1 Vocodeur en banc de filtres — forme Flanagan (1966), pas la forme bloc

Une banque de passe-bandes ; par canal, on extrait amplitude et **fréquence
instantanées** ; on resynthétise avec une banque d'oscillateurs dont la
fréquence est ×ratio. Continu (échantillon par échantillon), pas de frontières
de blocs, latence = retard de groupe des filtres.

- **Pour** : répond directement au §1.1 (per-canal, pas de re-partition) ; la
  latence suit la largeur de bande, donc peut être variable en fréquence.
- **Contre** : coûteux si beaucoup de canaux ; le suivi de fréquence
  instantanée par canal est le point délicat (bruit sur canaux faibles).

### 3.2 Banc dyadique / constant-Q (arbre QMF, ondelettes)

Sous-bandes espacées en octaves. Décaler d'une octave = déplacer une sous-bande
d'un niveau. Taillé pour −2/−1/+1/+2.

- **Pour** : latence naturellement dépendante de la fréquence (§1.2) ; exploite
  la structure « octaves seulement » (§1.3) ; phase préservée par sous-bande.
- **Contre** : le **+5te (ratio 1,4983) casse l'histoire dyadique** — voir §4.
  Les aliasing/recouvrements des QMF demandent des filtres soignés.

### 3.3 Sliding DFT (Jacobsen–Lyons)

Résolution type-FFT mais **mise à jour à chaque échantillon, sans frontières de
blocs**.

- **Pour** : supprime précisément l'artefact de re-partition en arpège (§1.1)
  tout en gardant la résolution grave ; migration incrémentale depuis le
  vocodeur actuel (même analyse en fréquence, sans le hop).
- **Contre** : latence toujours bornée par Gabor dans le grave ; coût O(bins)
  par échantillon ; stabilité numérique du récurseur glissant à surveiller.

### Choix pour le premier spike

**§3.1 (banc de filtres Flanagan), 3 octaves**, parce que c'est celui qui
attaque frontalement le critère décisif §1.1 et qu'il permet la latence
variable §1.2. On tiendra §3.3 (sliding DFT) en réserve comme évolution
incrémentale du vocodeur existant si le banc de filtres se révèle trop cher.

---

## 4. Le cas de la quinte (à budgéter dès le départ)

Un arbre dyadique gère les octaves élégamment mais **pas** le +5te
(ratio 1,4983, non dyadique). Options, à trancher avant d'implémenter :

1. **Mécanisme séparé pour le +5te** : garder le moteur actuel (granulaire ou
   vocodeur) uniquement pour cette voix, brancher les octaves sur le nouveau
   banc. Coût : deux moteurs en parallèle.
2. **Hétérodyne per-canal** : dans une forme banc-de-filtres (§3.1), la quinte
   n'est pas plus dure qu'une octave — chaque canal est simplement ×1,4983.
   C'est un argument de plus pour §3.1 contre §3.2.
3. **Renoncer au +5te sur le moteur expérimental** dans un premier temps et le
   traiter comme une extension.

→ **Décision provisoire : (2)** — la forme banc-de-filtres rend la quinte
gratuite, ce qui conforte le choix du §3.

---

## 5. Contraintes non-négociables du projet

À ne jamais casser en explorant :

- **Le dry reste à latence zéro** (sauf `Dry: Detune`, déjà documenté). Le
  nouveau moteur ne concerne que les voix wet.
- **Cœur DSP host-agnostic** : tout nouveau moteur est un header dans `src/`
  inclus tel quel par LV2 et JUCE (cf. `docs/lv2-to-multiplatform.md`). Zéro
  dépendance de format, zéro alloc dans `process()` — tous les buffers en
  membres, comme `stream_vocoder.hpp`.
- **Budget temps-réel par bloc** : le hack de *stagger* (`_hop_phase`) existe
  parce que 8 voix de FFT en lockstep dépassaient la deadline d'un bloc sur
  Pi 5. Un banc de filtres a un profil de charge *lissé* (pas de rafales) —
  c'est un avantage à mesurer, pas à supposer.
- **Commutable via `Focus`** : le moteur expérimental s'ajoute comme un
  troisième chemin, il ne remplace rien tant qu'il n'a pas fait ses preuves à
  l'`audit`.
- **`make audit` est l'arbitre.** Toute affirmation sonore (« propre sur
  accord », « n'affecte pas les notes tenues ») doit devenir un test offline
  dans `tools/`, sur le modèle de `tools/detune_test.cpp`, `shift_test.cpp`, etc.

---

## 6. Plan expérimental

**Spike 1 — banc de filtres Flanagan, octaves seules.**
Prototype d'un `StreamFilterbank` (3 octaves, un oscillateur par canal),
branché derrière le même ring partagé que les deux moteurs existants, exposé en
troisième option de `Focus`. Cibles mesurées :

1. Sub sur accord **sans** le ripple +4,8 dB du granulaire.
2. Latence aiguë quasi nulle, latence grave bornée et *documentée* (§1.2).
3. **Test d'arpège** : un nouveau test dans `tools/` qui joue une note, la tient,
   puis en attaque une seconde, et vérifie que la première ne bouge pas
   (énergie / phase stables sur la bande de la première note). C'est la
   traduction mesurable du §1.1 — le critère qui justifie toute cette branche.

**Spike 2 (réserve) — sliding DFT**, si le banc de filtres est trop cher : même
objectif §1.1 par migration incrémentale du vocodeur.

**Jalons** : chaque spike passe par `make audit` vert avant tout commit de son.
Aucune fusion vers `master` tant que le moteur expérimental n'égale pas le
vocodeur sur accord **et** ne bat pas le block-FFT sur le test d'arpège.

---

## 7. Ce qu'on abandonne explicitement (par rapport à la piste initiale)

- **Banc de 32–64 bandes uniformes** : équivaut à une STFT basse résolution
  (bins de 750 Hz à 64 canaux) — incapable de séparer un accord grave, et c'est
  réinventer en moins bon la FFT existante. Remplacé par un banc **octave /
  constant-Q** (résolution qui suit la fréquence).
- **« Réalignement de phase » comme étape séparée** : suppose un design qui
  *casse* la phase pour la rattraper. Les formes retenues (§3.1, §3.3) ne la
  cassent jamais — la cohérence est structurelle, comme dans le vocodeur
  per-peak actuel.
- **Shift indépendant par bande fixe** : un pic adaptatif (ou un canal à
  fréquence instantanée suivie) bat une bande fixe, car les partiels ne tombent
  pas sur des centres de bande fixes.

Ce qu'on **garde** de la piste initiale : la **détection + réinjection des
transitoires** (le plus gros gain perceptif, et l'`OnsetDetector` existe déjà),
et l'idée de **latence dépendante de la fréquence**, ré-exprimée proprement
en §1.2.

---

## 8. Résultats du Spike 1 (banc de filtres hétérodyne constant-Q)

Implémenté dans `src/stream_filterbank.hpp`, mesuré par `tools/filterbank_test.cpp`
et `tools/arpeggio_test.cpp` (tous deux branchés dans `make audit`). Banc
constant-Q, 45 Hz–9 kHz, 6 canaux/octave, Q=6 (~46 canaux) ; par canal :
démodulation hétérodyne → baseband → passe-bas 1 pôle complexe → fréquence
instantanée → resynthèse à `ratio×`.

**Ce qui marche.** La transposition est juste et propre : un sinus à 220 Hz
ressort à 110 / 440 Hz, dominant, sans fuite mesurable à la fréquence d'entrée
ni à l'octave voisine. La latence est bien dépendante de la fréquence (grave
~40 ms, médium ~2 ms, aigu sub-ms), comme visé au §1.2.

**Le piège trouvé — et c'est le vrai apport du spike.** La resynthèse *naïve*
(sommer **tous** les canaux qui se recouvrent) échoue lourdement :

| Métrique | Overlap naïf | Peak-pick | Granulaire | Vocodeur (plancher) |
|---|---|---|---|---|
| Ripple sub sur accord (150 ms) | +7,3 dB | **+3,4 dB** | +4,5 dB | +0,0 dB |
| Attaque perturbe note tenue (§1.1) | 6,1 dB | **1,2 dB** | — | 0,2 dB* |

Cause : un partiel est porté par ~CPO/2 canaux à la fois ; l'estimation de
fréquence par canal est **non linéaire** (`arg` d'un produit), donc la fuite
d'une *autre* note corrompt différemment chaque canal, qui **décorrèlent** et
s'annulent en sommant. Résultat contre-intuitif : une nouvelle attaque *baissait*
la note tenue de 6 dB — l'exact opposé du §1.1. Mon hypothèse « A vit dans ses
canaux, B dans d'autres » était **trop naïve pour un banc à recouvrement**.

**Le correctif appliqué : un oscillateur par partiel.** N'émettre que les canaux
maximum-locaux (|zk| ≥ voisins) effondre chaque partiel sur son canal dominant,
dont l'estimation de fréquence est peu perturbée par les notes lointaines. Gain
net : ripple divisé par ~2 et **il bat désormais le granulaire** (3,9 vs 5,0 dB) ;
perturbation d'arpège divisée par ~5 (6,1 → 1,2 dB).

**Ce qui reste (→ Spike 2).** Le banc peak-pické **ne rejoint pas encore le
plancher du vocodeur**. Le résidu est le **flicker** : un partiel saute entre
deux canaux max-locaux adjacents quand son amplitude oscille, chaque saut étant
un petit pas d'amplitude/phase. Il faut du **suivi de partiels** (hystérésis /
continuité inter-trame — Puckette, sinusoidal modeling) pour le tuer.

\* *Caveat honnête, gardé visible dans le test :* sur un accord de **deux** tons
propres, la re-partition du vocodeur est stable, donc il score bien ici — cette
métrique est un plancher pour le banc, **pas encore le discriminant décisif**.
Reproduire le vrai défaut « la résonance des notes tenues bouge » exige un
matériau plus dense (beaucoup de partiels proches qui re-partitionnent quand B
arrive). À construire au Spike 2.

**Verdict.** Architecture prometteuse et alignée avec les trois observations :
transposition propre, latence dépendante de la fréquence, **bat le granulaire
sur accord**, structure per-canal. Mais atteindre la propreté du vocodeur
demande le suivi de partiels — c'est le cœur du Spike 2, pas un réglage.

---

## 9. Résultats du Spike 2 (suivi par passage de phase)

Objectif : tuer le flicker résiduel du Spike 1. Chemin parcouru, mesuré à
chaque pas (`filterbank_test`, `arpeggio_test`) :

| Étape | Arpège (§1.1) | Ripple accord | Verdict |
|---|---|---|---|
| Spike 1 (un osc/partiel) | 1,2 dB | +3,4 dB | point de départ |
| + hystérésis + passage de phase | **0,04 dB** | +3,4 dB | meilleur que le vocodeur… |
| …mais **fragile** | — | — | deadlock / latch-up (voir ci-dessous) |
| **Retenu : passage de phase seul** | **0,45 dB** | +3,4 dB | robuste, qualité-vocodeur |

**Ce qui a marché — le passage de phase.** Quand le canal émetteur d'un partiel
change (le partiel dérive d'un canal au suivant), le nouveau canal **hérite le
θ du canal sortant**. Les deux suivent la même fréquence, donc une fois alignés
ils restent alignés : le croisement est sans couture. À lui seul il fait passer
l'arpège de 1,2 à **0,45 dB** — au niveau du vocodeur (0,16 dB sur ce test
facile), et l'exact opposé des −6 dB du banc naïf.

**Ce qui a été essayé et rejeté — l'hystérésis.** Ajoutée par-dessus, elle
gagnait encore (0,04 dB) mais **toute** formulation sûre cassait ailleurs :
- marge (1±H) des deux côtés → un ton pile entre deux canaux égaux ne dépasse
  *ni* l'un *ni* l'autre de la marge → **aucun** n'émet → note **silencée** ;
- boost de rétention du canal émetteur contre ses voisins bruts → deux canaux
  adjacents se boostent mutuellement → **latch-up** de grappes entières
  (amplitude ×6, mesurée).
La sélection retenue — max local strict-à-gauche/≥-à-droite (un seul gagnant sur
tout plateau) **sans** hystérésis — est robuste sur n'importe quel profil de
|z|. Le passage de phase fait le lissage que l'hystérésis visait, sans ses
modes de défaillance.

**Ce qui reste — plafond du ripple d'accord (→ Spike 3).** Toujours +3,4 dB sur
l'accord. Cause **diagnostiquée** (et non plus supposée) : un partiel **faible
voisin d'un fort** est masqué par la jupe du fort dans un banc log clairsemé ;
son canal n'est max local que par intermittence, donc il émet irrégulièrement.
Mesuré : les trois partiels de l'accord ressortent à **0,19 / 0,05 / 0,04** au
lieu d'égaux. Ni l'affinage du maillage (CPO 12 : arpège *dégradé* à 1,9 dB pour
un maigre gain d'accord) ni une marge ne corrigent ça — il faut du vrai **suivi
de partiels** (amplitude de pic par interpolation parabolique + appariement
naissance/mort) ou plus de résolution sans la latence.

---

## 10. Le test à l'oreille casse le peak-picking — et pointe la vraie voie

Rendu WAV d'un vrai passage joué (`tools/render_wav.cpp`, arpège fingerstyle +
accord). Verdict de l'oreille : **râpeux, type bit-crusher, transposition
infidèle**. Les métriques dB (ripple, arpège) ne l'avaient pas vu — elles
mesurent la stabilité d'**amplitude**, pas la fidélité du **timbre**.

**Diagnostic spectral** (note 110 Hz riche → sub 55 Hz, amplitude par
harmonique) :

| Harmonique | attendu 1/h | **peak-pick** | **overlap complet** |
|---|---|---|---|
| 1 (55 Hz) | 1.00 | 0.28 | 0.98 |
| 2 (110) | 0.50 | 0.22 | 0.57 |
| 3 (165) | 0.33 | **0.011** | 0.15 |
| 4 (220) | 0.25 | 0.049 | 0.10 |
| 5 (275) | 0.20 | **0.004** | 0.11 |
| 7 (385) | 0.14 | **0.002** | 0.037 |

Le peak-picking dans un banc log clairsemé **perce des trous** dans la série
harmonique (impaires 30–70× trop faibles) → timbre détruit, son creux et dur.
La reconstruction **overlap complète** garde toutes les harmoniques (juste un
léger tilt aigu, corrigeable par EQ fixe).

**La tension, précise :**
- *peak-pick* → amplitude stable (arpège 0,3 dB) **mais timbre détruit** ;
- *overlap* → **timbre fidèle** mais les canaux d'un même partiel **décorrèlent**
  sous la fuite d'autres notes (les −6 dB d'arpège du Spike 1 naïf).

**Le correctif connu** (→ Spike 3) : émettre **tous** les canaux (fidélité) mais
**verrouiller la phase** de chaque canal sur le pic de sa région —
θ_k = θ_pic + (arg z_k − arg z_pic). C'est exactement le *identity phase
locking* de Laroche-Dolson que fait déjà `stream_vocoder.hpp`, mais appliqué en
**continu** (par échantillon) au lieu de par trame FFT. Les canaux d'un partiel
restent alors cohérents (pas de décorrélation) **et** toutes les harmoniques
sont reconstruites (pas de trous).

### Spike 3 — résultat : le verrouillage continu échoue, on choisit l'overlap

**Ce qui a été tenté.** Overlap + verrouillage d'identité continu :
θ_k = θ_pic + (arg z_k − arg z_pic), le pic accumulant ratio×sa fréquence.

**Pourquoi ça échoue.** Mesuré tout de suite sur la fidélité de timbre : h2
s'effondre à 0,02 (au lieu de 0,5). La cause est subtile et instructive — c'est
la différence entre un phase vocoder **à trames** et une version **continue**.
Le baseband de chaque canal tourne à (f − f_k) : la différence de phase
d'analyse (arg z_k − arg z_pic) **dérive** dans le temps au rythme (f_pic − f_k).
En verrouillant sur la valeur instantanée, le membre oscille à
ratio·f + (f_pic − f_k) au lieu de ratio·f : **il se désaccorde et s'annule**.
Le vocodeur à trames y échappe parce qu'il re-photographie le lobe à chaque
trame et n'intègre jamais la phase des membres. En continu, il faudrait figer
l'offset (forme du lobe) sans le laisser dériver — **problème ouvert**.

**Ce qu'on retient.** L'engine tourne désormais en **overlap simple** (chaque
canal à sa propre fréquence, plancher adaptatif + lissage). Bilan honnête :

| Approche | Timbre (harmoniques) | Cohérence poly (arpège) |
|---|---|---|
| peak-pick + handoff (Spike 2) | **trous** (impaires 30-70× bas) | **0,45 dB** ✓ |
| verrouillage continu (Spike 3) | pire (h2 s'annule) | — |
| **overlap simple (retenu)** | **fidèle** (toutes présentes) ✓ | 6 dB (régresse) |

Aucun point ne gagne les deux axes. L'oreille ayant rejeté le bit-crush du
peak-pick, on **priorise le timbre** : overlap. La fidélité de timbre devient un
**gate** dans `filterbank_test` (les 8 harmoniques doivent survivre, sans trou) ;
ripple accord et arpège passent en **report-only** (mesurés, documentés, non
assertés) le temps que la tension soit ouverte.

### Conclusion de lucidité (importante)

Trois spikes ont établi un fait net : **la transposition polyphonique fidèle ET
cohérente à basse latence est un vrai problème de recherche.** Le banc de
filtres donne la **basse latence dépendante de la fréquence** (acquis) et, en
overlap, un **timbre fidèle** (acquis) — mais la cohérence polyphonique demande
un verrouillage de phase que, sans trame, on ne sait pas faire sans désaccord.
La version à trames qui sait le faire, c'est le **phase vocoder** — et on l'a
déjà (`stream_vocoder.hpp`), au prix de 85 ms.

Deux voies devant nous, à trancher **avec l'utilisateur** :

- **(A) Continuer le banc** : chercher le verrouillage continu sans désaccord
  (offset de lobe figé + ré-appariement propre aux transitions), l'EQ de
  voicing, le test d'arpège dense. Potentiel : fidélité du vocodeur à latence
  variable. Coût : de la recherche, pas des réglages.
- **(B) Rediriger l'effort vers le vocodeur** : il est déjà fidèle et cohérent ;
  attaquer SES faiblesses — latence (bandes hybrides : grave long, aigu court,
  cf. §3.2), lissage d'attaque (le split transitoire du §7, qui reste le plus
  gros gain perceptif inexploité) — au lieu de réinventer son cœur en continu.

Mon avis : (B) a le meilleur rapport résultat/risque à court terme, et le
**split transitoire** (détection + réinjection des attaques, `OnsetDetector`
déjà présent) est le gain le plus tangible vers « ça sonne comme un POG ». Le
banc reste une piste de fond pour la latence variable.

---

## 11. Spike 4 — l'indice « première note nette » et le mur confirmé

Retour d'écoute sur l'overlap : *« son sale, mais la toute première note de
l'arpège est beaucoup plus nette »*. Indice précieux : la première note est
seule et les canaux sont fraîchement réinitialisés (cohérents) ; la saleté
arrive avec la **polyphonie** (un canal voit alors deux partiels, son estimation
de fréquence instantanée est corrompue, θ dérive). L'overlap **est** le shifter
hétérodyne canonique — fidèle — sa seule faille est cette décorrélation.

Quatre leviers testés ce tour-ci, mesurés (timbre = h2, doit valoir ~0.44 ;
cohérence = perturbation d'arpège) :

| Levier | Timbre (h2) | Cohérence (arpège) |
|---|---|---|
| overlap pur (retenu) | **0.44** ✓ | 6,3 dB ✗ |
| verrouillage d'identité continu (§10) | ~0 ✗ | — |
| **PLL de fréquence par membre** (LOCK_C) | ~0.01 ✗ | 0,55 dB ✓ |
| **canaux plus étroits** (Q=12, CPO=12) | 0.048 ✗ | 5,3 dB ✗ |

Le PLL de fréquence **résout la cohérence** (arpège 8 → 0,55 dB) mais annule le
timbre : forcer les membres sur la phase d'identité fait sommer les basebands
bruts Σz_k, qui **s'annulent** (chaque canal est à une phase différente).
Balayage de LOCK_C : **aucun point d'équilibre** — timbre et cohérence
s'échangent directement. Canaux plus étroits : améliorent l'accord mais dégradent
timbre ET latence, sans régler l'arpège.

**Méta-conclusion (solide, à quatre angles).** Sur ce banc, **fidélité de timbre
et cohérence polyphonique sont en opposition directe** avec les resynthèses
qu'on sait construire sans trame. Les seules issues connues — isoler un partiel
par canal (→ résolution fine → latence → le vocodeur) ou re-photographier par
trame (→ le vocodeur) — **ramènent toutes au phase vocoder**. Le banc de filtres
achète la **latence dépendante de la fréquence** ; il ne sait pas acheter la
cohérence sans trame. C'est un résultat de recherche, pas un échec d'exécution.

### Pistes restantes, honnêtes
- **(A′) Granulaire multibande** — piste *vraiment* différente, pas encore
  essayée : découper en sous-bandes étroites et transposer chaque sous-bande par
  **resampling temporel** (granulaire), pas par ré-oscillation. Une sous-bande
  étroite est quasi-sinusoïdale, donc le grain y est propre et il n'y a **aucun
  accumulateur de phase à décorréler**. C'est le §3.2 dyadique repris côté temps.
- **(B) Split transitoire sur le vocodeur** — le plus gros gain perceptif vers
  « POG », `OnsetDetector` déjà là.

Le banc hétérodyne spectral (Spikes 1–4) est archivé comme dead-end documenté
pour la fidélité polyphonique, gardé pour sa leçon sur la latence variable.

---

## 12. Transitoire, tentative 1 : reset de phase dans le vocodeur — négatif

Direction (B) retenue par l'utilisateur : rendre l'attaque nette sur le
vocodeur, qui l'étale sur ~85 ms. Première tentative : **reset de phase à la
Röbel** — sur onset, remettre à zéro les accumulateurs `_rot[]` du vocodeur pour
que la trame reconstruise depuis la phase d'analyse (attaque nette).

**Résultat mesuré : négatif.** Temps de montée 10-90 % de l'attaque (+1 oct,
`build/` hors-arbre) :
- sans reset : 8,6 ms ;
- reset **une** trame bien placée : 7,4 ms (−14 %) ;
- reset appelé **au sample de l'attaque** (comme le ferait le plugin) : 8,6 ms
  (**aucun effet** — consommé sur une trame silencieuse où `_rot` vaut déjà ~0) ;
- reset une trame trop tard : 22,9 ms (**pire**).

Effet minime, erratique, et **critique en timing** : selon la trame exacte sur
laquelle il tombe, il n'a aucun effet, ou dégrade. Le maculage d'attaque du
vocodeur n'est **pas** dominé par `_rot` — il tient à la translation de
fréquence + l'OLA. Zéroter `_rot` n'est pas le bon levier. Le vrai Röbel
demanderait la détection des **bins transitoires** et un traitement de trame
dédié (fenêtre courte pendant l'attaque) — gros travail, gain incertain.
Réverté (pas de code fragile no-op dans un moteur livré).

### Tentative 2 (recommandée) : réinjection de transitoire

Approche robuste et éprouvée pour une attaque nette : ne **pas** essayer de
dé-maculer le vocodeur, mais **réinjecter** l'attaque par un chemin court.
Le transitoire d'un mediator est percussif/large bande — sa hauteur importe
peu — donc :
- détecter l'onset (`OnsetDetector`, déjà là) ;
- prélever une courte bouffée (~5-15 ms) enveloppée de l'entrée, éventuellement
  passe-haut, **retardée** pour s'aligner sur la latence du wet ;
- la sommer au bus wet. Sur un POG, le **dry** joue déjà ce rôle à latence zéro ;
  la réinjection ne sert que les presets **wet-only** (dry coupé).

C'est un changement **niveau plugin** (dry retardé + onset + mixage wet), pas
niveau moteur — à faire dans `pogged_dsp.cpp` avec un test de netteté d'attaque
comme métrique. Prochaine étape à valider.

---

## 13. Latence : STFT multi-résolution (validé à l'oreille) ✓

Retour d'écoute décisif sur un mix Classic POG (dry + octaves, vocodeur) : le
timbre des voix est **propre**, mais le **décalage de 85 ms** entre l'attaque du
dry (latence zéro) et les octaves donne une **mollesse**. C'est un problème de
**latence**, pas de maculage. À l'A/B :
- `N=2048` (42 ms) resserre nettement l'attaque — **préféré** — mais floute un
  peu le grave (jugé « pas si affreux ») ;
- le banc de filtres en mix est **inécoutable** (la décorrélation §8-§11
  s'entend crûment) — piste définitivement écartée pour l'usage.

**Solution retenue et validée : STFT multi-résolution.** Faire tourner DEUX
fenêtres et les croiser en fréquence :
- fenêtre **longue** (4096, ~85 ms) → **grave** (accords/subs résolus) ;
- fenêtre **courte** (2048, ~42 ms) → **aigu + transitoires** (attaque serrée).

`src/stream_vocoder.hpp` a été **templatisé** (`StreamVocoderT<N>`, alias
`StreamVocoder` inchangé) pour que deux fenêtres coexistent. `src/stream_multi
vocoder.hpp` (`MultiVocoder<N_LO,N_HI>`) fait tourner les deux sur le ring
partagé et somme via un crossover **Linkwitz-Riley 4e ordre à 250 Hz** (grave du
chemin long + aigu du chemin court). La **latence dépendante de la fréquence**
tombe gratuitement : le grave paie 85 ms (pardonné là), les attaques passent en
42 ms → les voix collent sous le dry.

**Mesuré** (`tools/multires_test.cpp`, dans `make audit`) : sur un Mi majeur
grave, ripple du sub **multi 3,5 dB ≈ 4096 (3,7)** et **mieux que 2048 (4,6)** —
il garde la résolution grave de la fenêtre longue. Transposition juste à travers
le crossover. **À l'oreille : le mix multi est le meilleur des trois** (attaque
du 2048 + grave du 4096).

### Reste à faire
- **Réglage** : crossover (250 Hz), et l'écart de latence inter-bande (43 ms) —
  transparent à l'oreille pour l'instant ; à ré-écouter sur d'autres matières.
- **Intégration** : brancher `MultiVocoder` dans `pogged_dsp.cpp`. Option simple
  — **remplacer** le chemin vocodeur par le multi-res (Focus reste binaire,
  granulaire/vocodeur), au prix de ~1,5× le coût FFT par voix (2 FFT au lieu
  d'1). À peser pour Pi/MOD ; le Dwarf compile déjà le vocodeur out.
- **Coût CPU** : mesurer le pire bloc (le stagger existant aide, mais il y a 2×
  plus de rafales FFT).
