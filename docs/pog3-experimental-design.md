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

### Spike 2 — plan
- **Suivi de partiels** sur le banc : hystérésis de sélection des pics + appariement
  inter-échantillon, pour supprimer le flicker (cible : ripple accord < +1 dB,
  arpège < 0,3 dB).
- **Test d'arpège dense** exposant enfin le défaut du vocodeur (matériau à
  partiels serrés), pour faire d'`arpeggio_test` un vrai discriminant.
- **Aplatissement du banc** (follow-up #1) : trim de gain par canal pour une
  réponse unité plate, prérequis avant tout branchement dans `Focus`.
- Puis seulement : câblage comme 3ᵉ option de `Focus` (TTL LV2 + JUCE + modgui).
