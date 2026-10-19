# DESRU-Enhanced

[English](README.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md)

Utilitaire Windows non officiel pour DOOM Eternal, dérivé de [DESRU 1.6.17 de bowsr](https://github.com/bowsr/DESRU) L'historique Git et les crédits d'origine sont conservés.

## Installation

1. Dans les [Releases](https://github.com/IlyaTsyganov/DESRU-Enhanced/releases/latest), téléchargez **DESRU-Enhanced-v1.1.0-standalone-win-x64.zip** depuis Assets. Les archives « Source code » sont destinées au développement.
2. Sous Windows 10/11 x64, fermez les autres instances de DESRU et extrayez l'archive complète dans un dossier accessible en écriture, avec tous ses sous-dossiers. Sauvegardez votre ancienne installation et vos réglages avant de remplacer des fichiers.
3. Lancez **DESRU.exe**, acceptez la demande de droits administrateur et sélectionnez le dossier du jeu si nécessaire.
4. Gardez DESRU ouvert et lancez DOOM Eternal normalement. Les deux programmes peuvent démarrer dans n'importe quel ordre ; aucun lanceur intermédiaire n'est nécessaire.

L'archive autonome contient le runtime de bureau .NET 6.0.36, les dépendances, polices, macro freescroll, meath00k et module visuel natif. Aucune installation préalable de DESRU ou de .NET n'est nécessaire. Aucun exécutable du jeu ni niveau n'est fourni. Les mises à jour automatiques du DESRU d'origine sont désactivées par défaut pour conserver cette version ; installez manuellement les prochaines versions de ce dépôt.

## Changements par rapport au DESRU d'origine

- Compteurs de vitesse intégrés au HUD, y compris en plein écran exclusif : vitesse horizontale actuelle et vitesse au dernier atterrissage, avec la brève impulsion. Positions et couleurs indépendantes, taille commune, affichage d'atterrissage facultatif.
- Réglage de l'opacité des deux couches identifiées de la teinte de faible santé, sans modifier santé ou armure.
- Activation/désactivation des cheats et de meath00k pendant la partie. La désactivation restaure les cvars suivies à leurs valeurs d'avant la session et désactive les cheats du joueur gérés par DESRU. L'ancien avertissement de redémarrage disparaît lorsque les deux sont désactivés.
- Affichage des cvars protégées modifiées et des modes noclip, notarget et god actifs. Démarrage dans les deux ordres ; masquage du nom du pilote adapté aux chaînes AMD.
- Correction de la réactivation de la règle pare-feu et de l'indicateur F, à partir de l'état réel de la règle.
- Lighting Fix : case mémorisée dans Options, état dans Info Panel et indicateur X. Activation à chaud, également avec un lancement direct du jeu.
- Collision Viewer commandé par la console : déclencheurs cyan, collisions du niveau bloquant le joueur vertes, murs invisibles pouvant apparaître/disparaître orange. Pas de case ni d'indicateur V ; commandes accessibles avec les cheats.
- Objets dessinés entièrement dès qu'une partie de leurs limites entre dans le rayon. Suppression de l'ancien plafond de géométrie. Calcul en arrière-plan des surfaces internes à retirer et des contours d'intersection visibles, même sans remplissage. Filtrage des formes connues ne bloquant pas le joueur.
- Disposition compacte Options/Trainer ; aide On-Screen Display et Info Panel actualisée ; rubrique Other avec toutes les commandes DESRU, molette et barre de défilement sombre.

Les fonctions d'origine (macro, raccourcis, mise à l'échelle, trainer…) restent disponibles. Vérifiez les règles de votre catégorie de speedrun : ce dépôt ne constitue pas une homologation.

## Commandes de console

Toute commande sans argument affiche sa valeur actuelle et son aide.

| Collision Viewer | Fonction |
| --- | --- |
| `desru_volumes_show 0/1` | Masquer/afficher tous les volumes ; 0 au début de chaque session. |
| `desru_volumes_fill 0/1` | Contours seuls / avec remplissage ; défaut 1. |
| `desru_triggers_show 0/1` | Masquer/afficher les déclencheurs. |
| `desru_collision_show 0/1` | Masquer/afficher les collisions du niveau. |
| `desru_invisible_walls_show 0/1` | Masquer/afficher les murs invisibles pouvant apparaître/disparaître. |
| `desru_volumes_render_distance <mètres>` | Rayon d'affichage ; défaut 40 ; par exemple 73.5 ; nombre fini positif ou nul. |

Activez les cheats puis saisissez `desru_volumes_show 1`. Désactiver les cheats masque le viewer et verrouille ses commandes. Remplissage, catégories et distance sont mémorisés. Les contours d'intersection apparaissent avec la scène optimisée après le calcul en arrière-plan.

| Compteurs / teinte | Fonction |
| --- | --- |
| `desru_speedometer_show 0/1` | Masquer/afficher les compteurs. |
| `desru_speedometer_showlanding 0/1` | Affichage de l'atterrissage ; nécessite l'affichage principal. |
| `desru_speedometer_x_pos <pixels> [current\|landing]` | Position depuis le bord gauche. |
| `desru_speedometer_y_pos <pixels> [current\|landing]` | Position depuis le bord supérieur. |
| `desru_speedometer_size <0.1–10>` | Taille des deux compteurs. |
| `desru_speedometer_color <R> <G> <B> [current\|landing]` | Couleur RVB ; canaux de 0 à 255. |
| `desru_lowhptintopacity <0–100>` | Opacité de la teinte de faible santé ; défaut 100. |

Ces commandes ne nécessitent pas les cheats. Vitesses en mètres/seconde, à deux décimales ; `--` indique qu'aucun atterrissage n'a été enregistré. Coordonnées en pixels physiques depuis le coin supérieur gauche. Omettez current/landing pour modifier les deux positions ou couleurs. Exemples : `desru_speedometer_x_pos 100 landing`, `desru_speedometer_color 255 255 255 current`. Réglages sauvegardés pendant l'exécution. Par défaut : deux compteurs blancs activés, current (0, 480), landing (100, 480), taille 3.5 ; les réglages existants restent prioritaires.

## Compatibilité et compilation

Le viewer n'est pas limité artificiellement à une version du jeu ; la résolution des structures du moteur doit toutefois réussir. HUD/teinte ont été testés sur 1.1 et Steam 6.66 Rev 3.2 ; viewer/lighting sur 1.0 Release. Toutes les combinaisons version/pilote ne sont pas vérifiées. Un léger scintillement de certaines surfaces peut subsister ; une grande distance consomme plus de ressources.

Compilation Windows : Visual Studio MSBuild, SDK .NET bureau et outils C++ (CMake, Ninja, Windows SDK). En-têtes Vulkan inclus. MSBuild de Visual Studio est nécessaire pour la bibliothèque COM du pare-feu. Utilisez `/p:SkipBundleBuildEvents=true` afin d'éviter les anciens événements de remplacement du dossier de sortie. Commandes et préparation des ressources : [guide autonome](docs/STANDALONE.md), [README anglais](README.md). Documentation technique : [compteurs](docs/SPEEDOMETERS.md), [visuels](docs/VISUAL-FEATURES.md), [cheats](docs/LIVE-CHEATS.md), [pare-feu](docs/FIREWALL.md), [README original](docs/UPSTREAM-README.md).

## Crédits et licence

DESRU original : **bowsr** ; fork : **IlyaTsyganov**. Intégration adaptée du [projet de billymooreq](https://github.com/billymooreq/DoomEternal-v1.1--RTXfix-DrlaCollisionViewer). Le viewer provient du travail d'IlyaTsyganov, utilisé avec son autorisation et non sous une licence MIT générale. meath00k : brongo et contributeurs. Voir [CREDITS](CREDITS.md). DESRU conserve GPL-3.0 ; les composants tiers conservent leurs propres conditions. Non officiel, sans approbation d'id Software/Bethesda.
