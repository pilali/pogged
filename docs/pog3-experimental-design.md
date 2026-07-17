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

### Intégré ✓

`MultiVocoder<4096, 2048>` **est** désormais le chemin Focus=vocodeur de
`pogged_dsp.cpp` (alias `PoggedVocoder`) : Focus reste binaire, les voix
passent à 42 ms sous le dry pour les aigus/attaques, 85 ms pour le grave.
Les cibles contraintes gardent leur réglage : `POGGED_PV_N` (Duo X, 2048)
retombe sur la mono-fenêtre historique, `POGGED_NO_VOCODER` (Dwarf) compile
toujours tout le chemin out. Le swell par bin (§14) est hérité par les deux
fenêtres (`set_swell` transmis, même constante en ms donc crossover cohérent) ;
`focus_test` passe à +0,2 dB du plancher idéal (le prix du crossover, asserté
< 1,0) et la tenue de A dans `polyswell_test` va de −0,75 à −1,6 dB (les bins
plus larges de la fenêtre courte laissent le transitoire mordre un peu plus —
sous les −2 dB assertés). **Écoute post-intégration** (mix Classic POG swellé
re-rendu via le multi-res) : verdict utilisateur — *le rendu reste bon* ; ni le
swell ni le timbre n'ont perdu au change de moteur.

**Coût CPU mesuré** (`tools/bench_vocoder.cpp`, hors audit — dépendant de la
machine ; 8 voix, blocs de 128 à 48 kHz) : sur x86, multi-res **2,0× la
moyenne** de la mono-fenêtre (531 vs 269 µs/bloc, ~20 % de deadline) et
**p99 ~22 %** contre ~12 %. Piège trouvé et corrigé : avec le même
`hop_phase`, chaque rafale de la fenêtre longue tombait dans le même bloc
qu'une rafale de la courte (HOP_LO multiple de HOP_HI) → pire bloc à 31 % ;
la fenêtre courte est décalée d'un **demi-hop** (rien ne bouge au son, même
invariant que le stagger) → 22 %.

**Pi 5 mesuré sur l'appareil (pistomp) ✓ — multi-res validé.** Trois passes
par condition, 8 voix (le pire cas — `act[]` coupe les voix inactives) :

| Condition | mono 4096, p99 | **multi-res, p99** |
|---|---|---|
| hôte audio arrêté | 15-17 % | **29-33 %** |
| hôte lancé + jeu guitare | 28-36 % | **50-58 %** |

Mieux que la projection (~45-50 % attendus à vide). La mesure « en charge »
est un **plafond pessimiste** : le bench est un processus ordinaire, son p99
inclut la préemption par le host temps-réel — que le plugin ne subit pas
(il tourne DANS le thread RT du host). La contention réelle restante est la
bande passante mémoire, quelque part entre les deux lignes. À l'écoute sur
l'appareil : « résultat sonore très encourageant, progrès importants ».

### Reste à faire
- **Réglage** : crossover (250 Hz), et l'écart de latence inter-bande (43 ms) —
  transparent à l'oreille pour l'instant ; à ré-écouter sur d'autres matières.

---

## 14. Spike 5 — le swell polyphonique (l'ATTACK du POG3) ✓

Le critère signature restant : *« le POG3 fait un swell sur chaque attaque d'un
arpège **sans altérer le sustain des notes précédentes** »*. Notre ATTACK ne le
faisait pas, structurellement : une **enveloppe scalaire unique** sur tout le
bus wet, retriggée par l'`OnsetDetector`. Sur une attaque pendant un sustain,
elle n'a que deux issues, toutes deux fausses :
- le détecteur **tire** → duck-puis-reswell de **tout** le bus, notes tenues
  comprises (mesuré : la note tenue plonge de **9,4 dB**) ;
- le détecteur **rate** (une note ajoutée sur un sustain ne monte le RMS que de
  ~3-6 dB, sous le seuil) → la nouvelle note **ne swelle pas du tout**.

Aucun réglage de sensibilité ne sort de ce dilemme : il faut une enveloppe
**par bande**, pas par bus. (C'est d'ailleurs un indice architectural de plus
que le POG3 est spectral à l'intérieur.)

**Implémentation — swell par bin dans le vocodeur.** Le vocodeur expose déjà le
spectre de synthèse à chaque trame ; chaque bin reçoit sa propre enveloppe
(`_swl[k]` dans `stream_vocoder.hpp`, `set_swell(atk_ms)`) :
- elle **suit librement vers le bas** (les décroissances naturelles et le
  silence ne sont jamais retenus, et un bin retombé re-swelle à la prochaine
  attaque) ;
- elle **monte avec la constante ATTACK** (90 % en `attack_ms`, exprimée en
  trames ; l'OLA lisse les pas inter-trames) ;
- le gain (réel, ≤ 1) multiplie le bin **sans toucher la phase**.

Une note fraîche naît dans des bins à ~0 → son énergie fade-in sur `attack_ms`.
Les bins d'une note qui sonne sont déjà au niveau → gain 1, **rien ne bouge**.
Le transitoire large bande du médiator qui traverse les bins d'une note tenue
est borné par le ratio ancien/nouveau du bin — une retombée douce, pas un duck.
Ni onset detector ni trigger : le swell est **continu et sans seuil**, donc pas
de sensibilité à régler et pas d'attaque ratée. Désactivé, l'état continue de
suivre le spectre (armer ATTACK en cours de note ne swelle que l'attaque
**suivante**, pas la note déjà entendue). Coût : 2049 `abs()` par trame par
voix — négligeable devant les 2 FFT.

**Câblage** (`pogged_dsp.cpp`) : l'enveloppe globale est calculée avant les
voix et ne s'applique plus qu'au **granulaire** (qui, sans domaine spectral,
garde le duck-reswell POG2 — assumé, c'est le moteur « vintage ») ; le vocodeur
swelle en interne ; `V_DRYD` est exclu (chemin dry : son swell est le bouton
DRY ATTACK, inchangé). `StreamVocoderT` étant templatisé, le `MultiVocoder`
(§13) hérite du mécanisme gratuitement le jour de son intégration.

**Mesuré** (`tools/polyswell_test.cpp`, dans `make audit`) — A 220 Hz tenue,
B 330 Hz attaquée dessus, ATTACK 500 ms, voix +1 oct :

| Moteur | Sustain de A pendant le swell de B | Swell de B |
|---|---|---|
| enveloppe globale (avant) | **−9,4 dB** ✗ | 90 % en 470 ms ✓ |
| **swell par bin (vocodeur)** | **−0,9 dB** ✓ | 90 % en 520 ms ✓ |
| granulaire (POG2, report-only) | −9,0 dB (assumé) | 90 % en 410 ms |

Le critère est asserté à −2 dB. Les 16 tests existants restent verts (le
`swell_test` POG2 du granulaire est inchangé).

**Mix Classic POG (dry + sub1 + up1), même matériau** — le test tourne aussi
sur le mix complet, où chaque exigence du POG3 est visible à la fois :

| Bande | swell par bin (vocodeur) | granulaire (global, report-only) |
|---|---|---|
| A tient, sub 110 Hz | **−1,9 dB** ✓ | −10,8 dB |
| A tient, up 440 Hz | **−0,7 dB** ✓ | −9,1 dB |
| B swelle, sub 165 Hz | 90 % en 540 ms ✓ | (duck global) |
| B swelle, up 660 Hz | 90 % en 570 ms ✓ | (duck global) |
| dry immédiat, 330 Hz | **1,00× dès 40 ms** ✓ | 1,01× (jamais swellé) |

Le dry reste rigoureusement immédiat et non swellé (DRY ATTACK off) pendant
que les deux octaves de B montent chacune sur leurs bins — c'est la scène
POG3 complète. Le sub est la tenue la plus serrée (−1,9 dB) : 110 et 165 Hz
ne sont qu'à ~4,7 bins dans la fenêtre 4096, les jupes des lobes se touchent.

*Deux pièges de mesure documentés dans le test, trouvés en l'étendant au mix :*
le **soft-clip de sortie** (la somme à 5 composantes crête au-dessus du genou
0,7 → la compression monte quand B entre et mime un dip de ~1 dB sur A ;
neutralisé par `out_level` 0,4, les métriques étant des ratios) ; et la
**fuite du Goertzel rectangulaire** (le dry de B à 330 fuit à ~−20 dB dans la
mesure à 440 et bat contre elle ; Goertzel fenêtré Hann, lobes < −31 dB).

### Validé à l'oreille ✓

A/B `render_wav` sur le mix Classic POG (dry + sub1 + up1, arpège mi majeur
soutenu, ATTACK 500 ms) : verdict utilisateur — *le mix global est « très sale »
(les octaves des notes tenues sont hachées par le duck à chaque pluck), le mix
polyphonique « beaucoup plus satisfaisant »*. L'oreille confirme la mesure
(octave de B2 au pluck 3 : 0,075→0,034→0,077→0,020 en global, décroissance
lisse 0,129→0,099→0,095→0,093 en polyphonique). Le matériau est pourtant le
pire cas d'harmoniques partagées (E3 = h2 de E2) — le comportement
« swell de la partie ajoutée » des bins partagés passe l'écoute.

### Reste à faire
- Le granulaire garde l'enveloppe globale : si le swell polyphonique doit un
  jour exister à 3 ms de latence, c'est un banc d'enveloppes temps-réel par
  sous-bande (les gains réels ne décorrèlent pas, contrairement aux phases des
  Spikes 1-4) — piste ouverte, non bloquante.

---

## 15. Le « vibrant » des voix up : crossover référencé entrée + LR8 ✓

Retour d'écoute sur le Pi (§13 intégré) : *voix légèrement « vibrantes »,
surtout +1/+2, un peu audible aussi en dessous*. Cause trouvée, mesurée,
corrigée — en deux couches.

**Couche 1 — le crossover était référencé côté sortie, la résolution vit côté
entrée.** Une voix à `ratio` place un partiel d'entrée f en sortie à ratio·f.
Avec le crossover fixe à 250 Hz en sortie, la voix +1 confiait à la fenêtre
**courte** de la sortie jusqu'à 250 Hz — donc des partiels d'**entrée**
jusqu'à 125 Hz, que ses bins de 23,4 Hz ne séparent pas sur un accord : les
lobes fusionnent, le pic bat, la translation module. Sonde (do3+mi3, 34 Hz
d'écart — un voicing banal — transposés ×2) :

| Moteur | AM par partiel en sortie |
|---|---|
| mono 4096 | **0,00 dB** |
| mono 2048 | 10-33 dB |
| multi, xover 250 sortie | **12-25 dB** ← le « vibrant » entendu |

Les subs étaient épargnés (250 sortie = 500 entrée — large), d'où la
perception « surtout +1/+2 ». Correctif : **xover_sortie = 250 × ratio** pour
les voix montantes (500 Hz pour +1, 1 kHz pour +2, 375 pour la quinte), 250
inchangé ailleurs — la constante qui compte, 250 Hz **côté entrée**, est
partout respectée. `set_xover()` dans `MultiVocoder`, câblé sur le ratio
nominal (Warp/détune bougent le pitch, pas le crossover).

**Couche 2 — la jupe du LR4 laissait fuir le rejet.** Résidu mesuré ~1 dB :
la version courte-fenêtre des partiels sous le crossover — précisément ce que
le split existe pour jeter, jusqu'à 30 dB de warble — ne repassait qu'à
~−22 dB (24 dB/oct). Crossover porté en **Linkwitz-Riley 8ᵉ ordre**
(48 dB/oct, somme toujours allpass-plate, 4 biquads par côté — négligeable
devant les FFT : ratio CPU multi/mono inchangé à 2,0× au bench).

**Mesuré** (`tools/stability_test.cpp`, dans `make audit`) : AM par partiel
×2 **0,19 dB**, ×4 **0,14 dB**, ×0,5 **0,16 dB** (plancher 4096 : 0,00 ;
gates à 0,5 dB ; contre-exemples 33 / 25 dB gardés en report-only). Effets
collatéraux positifs : `focus_test` revient à **+0,0 dB** du plancher idéal
(le +0,2 était cette même fuite) et la tenue de A dans `polyswell_test`
remonte de −1,6 à **−0,8 dB**. Le prix, honnête : le bas-médium des voix up
repaie la fenêtre longue (85 ms sous 500 Hz de sortie pour +1) — la zone où
l'oreille pardonne ; l'attaque aiguë reste à 42 ms.

**Pistes restantes si un résidu s'entend encore à l'écoute** (par ordre) :
suivi de partiels + lissage de fréquence par piste (le jitter d'estimation
sur corde réelle, toutes voix) ; fenêtre à lobes plus bas (Blackman-Harris)
sur le chemin court ; overlap 87,5 % (CPU ×2) ; PGHI (Průša-Holighaus 2017)
en chantier de fond.

---

## 16. Spike 6 — le scintillement : les collisions d'harmoniques, et la fenêtre qui les résout

Retour d'écoute après le §15 : *le scintillement persiste, attack et détune à
zéro — même plus flagrant sans swell* (logique : le swell par bin lisse les
montées, il masquait une partie du défaut). Chasse méthodique, sonde par
sonde :

**1. Une corde seule ne scintille pas.** Corde réaliste synthétique
(inharmonicité de raideur, battements des deux polarisations par mode,
plancher de bruit) : excès d'AM ≤ 0,3 dB sur les trois moteurs. Éliminé :
le jitter d'estimation sur note isolée.

**2. Le scintillement est polyphonique : les COLLISIONS d'harmoniques.**
Tierce majeure la2+do#3 réaliste, ×2, excès d'AM par harmonique vs le shift
idéal (même générateur, f0 doublées) : h4 de la2 est à 24 Hz du h3 de do#3
(2 bins à 4096 — non résolu), h5 de la2 à 4,5 Hz du h4 de do#3. Une paire non
résolue fait osciller l'estimation du pic fusionné au rythme du battement ;
et quand le picker la résout par intermittence, les lobes recouverts sont
**découpés** en régions translatées à des offsets différents. Mesuré :
mono 4096 pire cas **+42 dB**, moyenne **+8 dB**. C'est le scintillement.

**3. Les micro-correctifs ne suffisent pas — mesurés et écartés :**
- *lissage de fréquence par piste* : 42 → 25 dB pire cas — aide, ne tue pas ;
- *absorption des pics proches* (jamais deux régions par lobe) : **pire**
  (+71 dB) — le partiel faible devient stable mais **désaccordé** de
  (ratio−1)·Δf ;
- *persistance de pistes naissance/mort* (topologie stable) : marginal —
  le découpage des lobes recouverts reste.

**4. La seule issue frame-based : résoudre davantage.** C'est Gabor, en
face : | fenêtre | excès moyen | pire cas | latence |
|---|---|---|---|
| 4096 | +8,0 dB | +42 dB | 85 ms |
| **8192** | **+1,2 dB** | +14 dB | 171 ms |
| 16384 | +2,1 dB | +13 dB | 341 ms (le temps étale les battements — PIRE) |

**8192 est l'optimum** : il résout tout ce qu'un accord de guitare produit,
sauf les paires sub-bin (4,5 Hz → il faudrait 850 ms) que l'idéal fait battre
aussi. Et le coût par échantillon ne croît qu'en **log N** : +8 %.

**5. Les collisions vivent à TOUS les registres** (la2 h9 = 2004 Hz contre
do#3 h7 = 1965 Hz…) — le split fréquentiel ne peut donc pas garder une
fenêtre courte quelque part sans y scintiller. Balayage d'architectures
(même matériau) :

| Architecture (xin = crossover d'entrée) | excès moyen | pire cas |
|---|---|---|
| 4096+2048, xin 250 (expédié §15) | +14,7 dB | +77 dB |
| 8192+2048, xin 600 | +5,6 dB | +36 dB |
| 8192+4096, xin 700 | +2,5 dB | +23 dB |
| **8192+4096, xin 1200 (retenu)** | **+1,4 dB** | **+20 dB** |
| mono 8192 (plafond) | +1,2 dB | +14 dB |

**Retenu : `MultiVocoder<8192, 4096>`, crossover d'entrée 1200 Hz**
(`VOC_XOVER_IN`), gardé par `shimmer_test` (gates : moyenne < 2,5, pire < 25)
et `stability_test` mis à jour. FOCUS_XFADE porté à 250 ms (remplissage OLA
du 8192). **Le prix, assumé et à trancher à l'oreille : attaques 42 → 85 ms,
graves/médiums 85 → 171 ms.** C'est l'arbitrage stabilité/mordant — le
scintillement était le défaut signalé, la stabilité gagne ce round.

**CPU** : moyenne +6 % vs 4096+2048 (log N) ; mais le **pire bloc** monte à
~3,2× la mono-fenêtre (une trame 8192 = 2 FFT de 8192 dans un bloc) —
projection Pi 5 ~50 % de deadline à vide, **à re-mesurer sur l'appareil**.
Si trop chaud : FFT réelle (rfft, travail ÷2) est le levier suivant.

### La réconciliation attaque/stabilité — prochain spike
Le split **fréquentiel** ne peut pas donner les deux ; le split **temporel**
si : tout le stationnaire dans les fenêtres longues (propre, stable), et les
transitoires par un chemin court dédié — la **réinjection de transitoire**
du §12 (tentative 2), déclenchée par l'`OnsetDetector` existant, alignée sur
la latence du wet. Sur la pédale, le dry joue déjà ce rôle ; la réinjection
sert les presets wet-only et le mordant des voix elles-mêmes.

---

## 17. L'échelle de fenêtres (8192→2048→1024) : mesurée, et c'est une impasse

Demande utilisateur après le §16 : *le rendu est bien meilleur, la latence
n'est pas acceptable — peut-on découper plus de bandes et étaler les fenêtres
de 8192 à 2048, voire 1024 en haut ?* Implémenté (`MultiVocoder3`, arbre LR8
à deux crossovers, dans `stream_multivocoder.hpp`) et balayé aux deux
métriques (rugosité 5-80 Hz + AM en excès). Trois faits en sont sortis :

**1. La pureté tient, et même s'améliore.** `8192/4096/2048 @ xin 1200/2500` :
AM en excès moyen **+0,97 dB** (2-bandes : +1,4) ; remplacer le sommet par
1024 ne change rien aux métriques (+0,99). x1 doit rester à 1200 (à 700, la
bande 4096 récupère la ceinture de collisions : +4,9 dB moyen, +60 pire).

**2. Le CPU interdit la 3ᵉ bande.** Le coût par échantillon d'une fenêtre est
~constant en N (seul log N joue) : chaque bande AJOUTÉE coûte un moteur
entier. Mesuré : 3 bandes = **+47 % de moyenne** vs 2 bandes, et le pire bloc
**dépasse la deadline** sur x86 (p99 111-122 %) — xruns garantis sur Pi. Le
gain d'écoute (21-42 ms au lieu de 85 au-dessus de 5 kHz de sortie) ne vaut
pas ce prix.

**3. Et surtout : l'échelle ne peut pas atteindre la cible.** Les collisions
qui exigent le 8192 (écarts 5-45 Hz entre partiels de notes différentes)
vivent PARTOUT sous ~1200 Hz d'entrée — y compris la zone des fondamentales
(deux notes de gamme adjacentes qui se chevauchent en arpège : sol3 196 Hz et
la3 220 Hz = 24 Hz d'écart). Descendre une bande rapide dans le corps tonal y
ramène le scintillement ; l'y laisser maintient 171 ms. **Le découpage
fréquentiel est épuisé** : la 2-bandes `8192+4096 @ xin 1200` est son
optimum, et elle reste la forme expédiée. `MultiVocoder3` reste dans l'arbre
comme infrastructure mesurée et documentée.

### La suite — le découpage TEMPOREL, pas fréquentiel
La latence perçue d'une note est celle de son attaque, pas de son corps :
- **Réinjection de transitoire** (§12 tentative 2, prochain spike) : sur
  onset (`OnsetDetector` déjà là), prélever une courte bouffée d'entrée
  enveloppée et la sommer au bus wet — le claquement arrive en ~0 ms, le
  corps tonal fleurit derrière (les presets avec dry ont déjà ce
  comportement via le dry ; ceci sert les wet-only et le mordant des voix).
- **Fenêtres asymétriques** (type AAC-LD) : résolution du côté long, latence
  du côté court — pourrait ramener la ceinture de 171 vers ~100 ms. Spike de
  recherche (le modèle de phase per-peak suppose la fenêtre symétrique).
- **FFT réelle (rfft)** : travail FFT ÷2 — le levier CPU qui redonnerait du
  budget si une bande de plus redevenait désirable.

---

## 18. Réinjection de transitoire — la latence PERÇUE ✓

La sortie de l'impasse du §17 : la latence ressentie d'une note est celle de
son **attaque**, pas de son corps. Implémenté dans `pogged_dsp.cpp` :

- l'entrée passe-haut (1,8 kHz, toujours chaud) est prélevée en **bouffée
  enveloppée** (~12 ms de décroissance) à chaque onset (`OnsetDetector`
  existant) et sommée au bus wet à latence ~nulle ;
- **gaté par le DRY** (un dry présent EST déjà l'attaque à latence zéro —
  la bouffée sert les presets wet-only ; le gate suit `1 − g_dry`, lissé) ;
- **gaté par ATTACK** (un click ruinerait un swell délibéré) ;
- mis à l'échelle de la somme des gains des voix wet.

Le passe-haut rend la bouffée agnostique en hauteur (un transitoire de
médiator est percussif, §12) — pas de conflit avec les octaves qui suivent.

**Mesuré** (`tools/reinject_test.cpp`, dans `make audit`) :

| Cas | Résultat |
|---|---|
| wet-only, pick | attaque entendue à **~0 ms** ; corps tonal à ~172 ms (report) |
| ATTACK 500 ms | 30 premières ms à 0,7 % du niveau — bouffée bien gatée |
| dry présent | fenêtre de pick = dry seul à 0,1 % près — jamais doublé |

Constantes de départ à l'oreille : `BURST_HP_HZ` 1800, `BURST_MS` 12,
`BURST_GAIN` 1,6 — les trois boutons à ajuster à l'écoute sur le Pi. Si le
caractère plaît mais le niveau/couleur non, ce sont eux. Extension possible
plus tard : un port « BITE » exposant BURST_GAIN.

---

## 19. FFT réelle + tables : ×1,65, le budget CPU rendu ✓

L'entrée du vocodeur est réelle : la transformée tourne désormais sur
**N/2 points complexes** (échantillons pairs dans le réel, impairs dans
l'imaginaire, dépliage standard vers le demi-spectre) à l'analyse ET à la
synthèse — le miroir hermitien n'existe plus en mémoire. Les twiddles
`w *= wlen` (dépendance série dans la boucle interne + dérive d'arrondi)
sont remplacés par des **tables précalculées** à l'init.

**Mesuré** (bench 8 voix, blocs 128 @ 48 kHz, x86) :

| Forme | avant | après |
|---|---|---|
| expédiée 8192+4096 | 29 % deadline (p99 ~63 %) | **17,5 % (p99 ~44 %)** |
| 3 bandes §17 | 111-122 % p99 (impossible) | 56-76 % (redevient finançable) |

Audit inchangé au bruit numérique près : le chemin est équivalent.

**Les deux leviers suivants, évalués mais non faits :**
- **NEON** : notre radix-2 sur `std::complex` vectorise mal ; le gain (×2-3
  de plus) demande une réécriture split-radix en tableaux séparés re/im.
  À noter : la cible rpi5 compile avec `-fno-tree-vectorize` (libmvec) —
  toute vectorisation devra être explicite ou le flag affiné.
- **Threads** : possible (une voix par cœur) mais hostile au modèle LV2
  (mod-host possède le thread RT ; un pool interne risque la contention
  avec les autres plugins). Dernier recours seulement — et avec la rfft,
  le budget actuel ne le réclame plus.

---

## 20. Le cahier des charges tranche : le budget de latence, et le programme « stabilité à budget fixé »

A/B utilisateur sur tous les builds de l'arc §13-§19 : **la forme la plus
utilisable est celle de `351591f`** — 4096+2048, crossover fixe à 250 Hz en
sortie, soit 85 ms sous 250 Hz et 42 ms au-dessus. Au-delà de cette latence,
on sort du cahier des charges : la pureté du 8192 (§16) ne vaut pas ses
171 ms sur l'instrument. Décision actée.

**Rebase sur HEAD, pas revert git** : le profil de latence de `351591f` est
recâblé (`MultiVocoder<4096,2048>`, `VOC_XOVER_OUT = 250` fixe pour toutes
les voix, FOCUS_XFADE 150 ms) en **gardant tous les acquis neutres en
latence** : crossover LR8 (§15), swell par bin (§14), réinjection de
transitoire (§18), FFT réelle (§19). Conséquence mesurable immédiate : la
forme expédiée coûte **~13 % de deadline en moyenne** (rfft) au lieu des
~29 % de `351591f`, et l'attaque wet-only arrive à ~0 ms (réinjection),
corps tonal à ~44 ms.

**La dette assumée, chiffrée et suivie en ratchet :** le crossover fixe à
250 sortie remet aux voix up des partiels d'entrée non résolus par la
fenêtre courte. `shimmer_test` passe en **ratchet** (accord réaliste :
moyenne +20,2 dB < 22, pire +75,2 < 82 — tout travail doit faire baisser
ces gates, rien ne peut régresser en silence) ; `stability_test` suit les
cas up en report-only (×2 : 25,7 dB, ×4 : 43,5 dB) et continue d'asserter
le sub (0,16 dB, input-safe).

### Le programme §20 — stabilité du timbre à budget de latence fixé

Par ordre de rapport gain/effort, tout à latence constante :

1. **Fusion gatée par la profondeur de vallée** (nouveau, prometteur) : le
   flip de topologie (§16) vient du picker qui voit un pic ou deux selon la
   phase du battement. Décider par la **vallée entre pics** (peu profonde =
   un lobe fusionné → une région, translation rigide quasi idéale ;
   profonde = résolus → deux régions), avec hystérésis par piste. Contrairement
   à l'absorption brute (mesurée pire, §16), ne désaccorde pas les paires
   résolues.
2. **Lissage de fréquence par piste** (prototypé §16 : 42 → 25 dB sur le
   pire cas à 4096) + **plancher de pics** anti-fantômes. Garde-fou requis :
   le lag sur bend (warp_test l'arbitre).
3. **Overlap 87,5 %** (hop N/8) : trames deux fois plus denses, artefacts de
   trame lissés. CPU ×2 — finançable depuis la rfft (13 → ~26 %). À mesurer.
4. **Spike 7 — résynthèse paramétrique des régions fusionnées** : la vraie
   sortie par le haut. La stabilité n'exige pas de SÉPARER l'énergie (Gabor
   l'interdit), seulement des PARAMÈTRES stables — et l'estimation peut
   utiliser la série temporelle des trames (Prony/ESPRIT d'ordre 2 sur ~6-8
   trames) sans retarder le signal : latence d'estimation ≠ latence du
   signal. Une région détectée bi-tonale est resynthétisée comme deux
   noyaux de Hann aux fréquences cibles estimées, phases par piste.
   Potentiel : la stabilité du 8192 au budget du 4096.
5. **Fenêtres asymétriques** (AAC-LD) : meilleure résolution à latence
   égale côté analyse. Recherche (le modèle de phase per-peak suppose la
   fenêtre symétrique).

### §20 — leviers 1+2 : résultats mesurés (mitigés, l'essentiel résiste)

Balayage levier par levier sur l'accord réaliste, moteur complet
(4096+2048 @ 250) :

| Levier | fenêtre longue seule | moteur complet |
|---|---|---|
| plancher de pics (−50 dB) | neutre | neutre (gardé : robustesse au bruit réel) |
| **fusion gatée par vallée** | **+11,3/+71 dB — NUISIBLE, retirée** | — |
| lissage de fréquence (α=0,20) | **+8,0 → +5,3 / +40 → +24 dB** ✓ | neutre sur l'accord |
| lissage fenêtre courte | (aide isolément) | **nuisible via le crossover** |

Deux leçons :
1. **La fusion échoue pour la même raison que l'absorption du §16**, même
   gatée par la vallée avec hystérésis : le pic survivant alterne au rythme
   du battement quand les amplitudes sont proches, et le partiel faible est
   désaccordé pendant les phases fusionnées.
2. **Le lissage n'est bon que là où les paires sont au moins partiellement
   résolues** (fenêtre longue). Sur les paires profondément fusionnées de la
   fenêtre courte, il produit du stable-mais-désaccordé qui bat contre le
   rendu juste de la fenêtre longue à travers la jupe du crossover — pire
   que le wobble qu'il enlève. Expédié : `tune(0.20, 1.0)` — lissage sur la
   longue seulement (+ garde-fou bend : pas rapide au-delà de 0,8 bin,
   `warp_test` vert).

**Net au budget fixé** : ×4 −6,5 dB, pire cas accord 75,2 → 74,5, bande de
crossover (paire 34 Hz) 37,7 → 27,6, moyenne accord neutre. Ratchet resserré
(pire < 78). Le mécanisme dominant — les paires fusionnées de la fenêtre de
42 ms — résiste aux leviers rapides : les espoirs restants sont l'overlap
87,5 % (levier 3, CPU ×2 finançable) et la résynthèse paramétrique
(levier 4, Spike 7).

---

## 21. Levier 3 — overlap 87,5 %, avec la base d'estimation découplée ✓

Le hop passe de N/4 à N/8 (paramètre template `OS_`, fenêtres et latence
STRICTEMENT inchangées) : l'OLA moyenne 8 rendus par échantillon au lieu de
4, ce qui lisse les artefacts liés aux trames.

**Le piège trouvé en route — et son correctif, qui est l'apport réel du
levier :** densifier les trames raccourcissait la base de temps de
l'estimateur de fréquence (différence de phase entre trames consécutives) →
son wobble sur les paires fusionnées DOUBLAIT — mesuré : la paire de la
bande de crossover régressait de 27,6 à 34,6 dB. Correctif : l'estimateur
mesure désormais l'avance de phase sur **EB = OS/4 hops** (historique de
phases en anneau) — base de temps constante (N/4 d'échantillons) quel que
soit l'overlap. Avec ça :

| Overlap | accord (moyenne/pire) | paire xover | CPU (8 voix, x86) |
|---|---|---|---|
| 75 % (OS=4) | +20,2 / +74,5 dB | 27,6 dB | 13,6 % |
| **87,5 % (OS=8, expédié)** | **+19,6 / +71,9 dB** | **27,1 dB** | **26,5 % (p99 ~36 %)** |
| 93,75 % (OS=16) | +17,8 / +68,2 dB | 27,2 dB | ~53 % — hors budget Pi |

Gain modeste mais net et sans régression. Ratchet resserré (moyenne < 21,
pire < 75). Le CPU expédié revient au niveau pré-rfft du §13, que le Pi
tenait confortablement — à confirmer au bench sur l'appareil.

**Bilan §20-§21 cumulé** (depuis le rebase 351591f-sur-HEAD) : pire cas
accord 75,2 → 71,9 dB, bande de crossover 37,7 → 27,1 dB, ×4 43,5 → 31,2 dB,
moyenne 20,2 → 19,6 dB. Les paires profondément fusionnées de la fenêtre
courte restent le mur : c'est le domaine du Spike 7 (résynthèse
paramétrique, §20 voie 4) — la seule voie restante vers un saut qualitatif
à ce budget de latence.

---

## 22. Spike 7 — résynthèse paramétrique des régions fusionnées ✓

L'idée (§20 voie 4) : la stabilité n'exige pas de SÉPARER l'énergie de deux
partiels fusionnés (Gabor l'interdit au budget de latence §20) — elle exige
des PARAMÈTRES stables, et l'estimation peut regarder la série temporelle
des trames sans retarder le signal. Implémenté dans `stream_vocoder.hpp` :

- **Prony d'ordre 2** (moindres carrés) sur la série des 8 dernières trames
  du spectre dé-alterné au bin du pic — les racines donnent les DEUX
  fréquences au-delà de la résolution de la fenêtre ;
- **amplitudes complexes** par résolution 2×2 sur deux bins contre le noyau
  de Hann analytique (Dirichlet, table à l'init — le même noyau sert à
  l'analyse et à la synthèse, donc aucune normalisation) ;
- **synthèse** : deux noyaux, chacun à ratio×f_j exact avec son propre
  phasor suivi — pas de wobble, pas de désaccord, et l'espacement de sortie
  devient juste (×ratio, ce que la translation rigide ratait) ;
- **repli rigide** dans tous les autres cas.

**Le vrai travail du spike a été les gardes** — chaque itération mesurée :
1. sonde naïve : 603 fausses naissances sur une CORDE SEULE (ajustements de
   bruit, partiel unique réparti sur deux noyaux, harmoniques voisines
   repliées mod OS) → +25 dB de dégâts sur h1 ;
2. **gate de signification** (pic ≥ −30 dB du max de trame) — tue le bruit ;
3. **le test décisif : ordre 2 vs ordre 1** (E2 < 0,1·E1) — « y a-t-il
   vraiment une seconde exponentielle ? » ;
4. **discriminateur d'alias** : le noyau de Hann est NUL aux offsets entiers
   ≥ 2 — un vrai partiel décentré doit laisser de l'énergie au bin p±2 de
   son côté, un fantôme replié en prédit là où il n'y en a pas ;
5. **probation** (3 ajustements consécutifs avant de rendre) + **continuité
   de phase** (phasors nés de `_rot`, réécrits dedans pendant l'engagement)
   — plus de sauts aux transitions ;
6. **hystérésis des gates résiduels** (une paire suivie tolère des gates
   plus lâches ; les singles ne créent jamais de piste → jamais relâchés
   pour eux) — le pic qui dérive sur un bin dominé par un seul partiel ne
   casse plus l'engagement.

**Mesuré** (audit, gates promus) :

| Cas | avant Spike 7 | après |
|---|---|---|
| paire 34 Hz ×2 (fenêtre courte via multi) | 27,2 dB | **0,12 dB** — plancher idéal, ASSERTÉ < 0,5 |
| paire 34 Hz ×4 | 31,2 dB | **0,06 dB** — ASSERTÉ |
| corde seule (sécurité) | +0,02/+0,33 dB | **identique à OFF** |
| accord réaliste (ratchet) | 19,6 / 71,9 dB | **17,5 / 71,0** (gates 19/73) |
| CPU (8 voix) | 26,5 % avg | 26,8 % — négligeable |

L'accord ne gagne que modérément : ses paires y BATTENT (3+ composantes par
partiel de corde réelle) → le modèle à 2 exponentielles replie prudemment —
comportement voulu. Extension naturelle si l'oreille en redemande : ordre 3
sur les régions où l'ordre 2 échoue de peu, et engagement sur les paires
battantes par sous-modèle. `-DPOGGED_PRONY_DEBUG` compile des compteurs de
diagnostic (naissances, rejets par gate).

---

## 23. Spike 8 — le résidu de l'accord : clusters battants, ordre 3, rendu hybride

Cible : la dette restante du ratchet (accord réaliste : 17,5 / 71 dB), là où
chaque partiel est un CLUSTER (porteuse + bandes latérales de polarisation)
et où deux clusters collisionnent (4-6 composantes par région).

**Fait pendant le spike :**
1. **Diagnostic par harmonique ON/OFF** : le §22 est net-positif sur l'accord
   (moyenne 19,6 → 16,8 avec les extensions) mais MIXTE par harmonique — il
   aide massivement les composantes que le rigide massacrait (C#3 h1 :
   36 → 16 dB ; h3 : 45 → 17 ; A2 h9 : 12 → 1) et dégrade localement des
   composantes que le rigide rendait bien (A2 h1 : 7 → 13,5).
2. **Escalade à l'ordre 3** (Durand-Kerner sur le cubique, moindres carrés
   3×3, jusqu'à 3 noyaux par région, acceptation stricte : E3 < 0,10·E1 ET
   E3 < 0,25·E2) — implémentée, sûre, mais quasi inerte sur ce matériau :
   les vrais clusters à 6 composantes ne passent pas non plus l'ordre 3.
3. **Rendu hybride résiduel** : les K noyaux modélisés + le RÉSIDU du modèle
   translaté rigidement (rien ne disparaît plus — l'ancienne version jetait
   les bandes latérales non modélisées, ce qui aplatissait le battement
   naturel). Architecture conservée : c'est la bonne forme générale.
4. **Gates exposés en membres** (`prony_gates`, `prony_maxk`) + garde du
   rayon d'appariement des pistes (2·fpb par composante : plus lâche, les
   pistes de régions voisines se croisent et échangent leurs phasors).
5. **Contrainte tenue partout** : corde seule = OFF à 0,01 dB près, paire
   pure 0,12 dB, sub 0,14 dB, audit 20/20.

**Net : ratchet resserré à 18 / 71** (mesuré 16,8 / 69,0). CPU p99 ~50 %
x86 chargé — dans le budget.

**Le mur restant, précisément identifié** : la paire de FONDAMENTALES
battantes (A2 h1 + C#3 h1, 2,4 bins à 4096, fusionnées à 2048), à cheval sur
le crossover — l'engagement y échange la qualité entre les deux composantes
(l'une gagne 20 dB, l'autre perd 6) et ni l'ordre 3 ni l'hybride ne
départagent. Pistes pour un éventuel Spike 9 : modèle par cluster (ordre 2
par porteuse APRÈS séparation grossière), fenêtres d'estimation par
composante, ou engagement asymétrique (ne rendre paramétriquement QUE la
composante que le rigide rendait mal — décidable par les historiques des
deux rendus).

### §23 — post-mortem : le Spike 8 est retiré, leçon de méthode

Retour d'écoute sur le build Spike 8 : *recul net, scintillement renforcé,
tierce grave très confuse* (l'utilisateur doute ensuite de son test — l'A/B
au casque tranchera). Indépendamment de ce doute, le retrait est justifié
par nos propres mesures : le diagnostic par harmonique montrait des
régressions locales (+7 dB sur la fondamentale de la2) que la MOYENNE du
ratchet masquait, et un delta jamais élucidé entre la réécriture K=2 et
l'original du Spike 7. **Règle actée : aucune modification moteur ne
s'expédie sur une amélioration de moyenne quand des composantes identifiées
régressent — et l'écart inexpliqué entre deux implémentations « équivalentes »
est un motif de non-expédition à lui seul.** Le moteur revient à l'état
Spike 7 (`10b3f89`) ; les acquis du Spike 8 restent documentés ci-dessus
(diagnostic par harmonique, infrastructure ordre 3 et rendu hybride dans
l'historique git, mur de la paire de fondamentales identifié).
