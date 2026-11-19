# Guide d'utilisation — état actuel de la préversion

> **Aucun parcours de partage hôte/invité n'est actuellement livrable.** Le backend
> navigateur réel WebView2 est indisponible ; la capture et le rendu des pixels chez
> un invité ne sont pas implémentés. Ne suivez pas les anciens écrans/étapes décrits
> dans les documents historiques comme s'ils constituaient une fonctionnalité active.

## Fonctions utilisables dans cette préversion

- Le moniteur local observe les services et ports visibles sous le compte courant.
- Le poste desktop peut être lancé pour évaluer son interface et ses diagnostics.
- Le mode `--smoke-test` est réservé à la validation technique et au rendu dummy ;
  il ne représente pas une session de partage.
- Le mode de démonstration synthétique, lorsqu'il est activé explicitement, n'affiche
  que des données artificielles et ne prouve aucun chargement de page réelle.

Pour connaître les options réellement offertes par les binaires de cette build,
utiliser `devlink-monitor --help` et `devlink-desktop --help`. Ne saisissez pas de
code d'invitation réel : cette préversion ne doit pas être exposée à des invités.

## Fonctions futures / non qualifiées

Les parcours de sélection/partage de service, approbation d'invité, navigateur
isolé, chat de session, retour d'entrée, transfert de contrôle et révocation ne
constituent pas un parcours complet qualifié. Les modèles de session et composants
de contrôle présents dans le dépôt ne suffisent pas à attester leur fonctionnement
avec un navigateur réel sur le LAN.

Le partage ne devra être annoncé qu'après qualification du Browser Worker, du
pipeline média, des permissions A/B/C, de TLS sur la plateforme cible et d'un test
physique avec au moins deux machines. Voir `docs/reports/FINAL_COMPLETION_AUDIT.md`.
