# Installation — préversion de développement DevLink

## Avertissement de portée

La branche actuelle n'est pas une release de partage LAN. Le Browser Worker WebView2
réel, la capture de page, le pipeline de pixels et les interactions invité ne sont
pas disponibles. Aucune archive Release candidate validée n'est attestée par l'audit
courant ; ne déployez pas les builds de diagnostic auprès d'utilisateurs comme
plateforme de partage.

## Build de développeur

Les prérequis et les commandes officielles sont dans `docs/BUILD.md` :

- Windows x64 : MSYS2 UCRT64 Clang++ natif, exécuté sur Windows ;
- Linux x64 : Clang++ >=18 et dépendances native/SDL indiquées par `build.sh`.

Les résultats GCC sont uniquement exploratoires. Une compilation Linux ne qualifie
ni les exécutables Windows, ni WebView2, ni TLS Windows, ni le DPI physique.

## Packages

Le script `package` est prévu pour tester la build, créer une archive, l'extraire
dans un dossier propre, réexécuter les binaires puis publier l'archive vérifiée sous
`releases/` avec une empreinte SHA-256. Ce processus n'a pas été exécuté ici avec la
chaîne officielle Clang/Windows. Vérifiez le rapport d'audit avant d'utiliser les
archives d'un autre build.

Aucun prérequis matériel pour cinq invités, aucun support Windows/Linux complet,
aucun runtime redistribué et aucune compatibilité réseau ne sont garantis par ce
guide tant que la qualification correspondante n'a pas été publiée.

## Supprimer une build locale

Fermer les processus DevLink lancés localement, puis supprimer uniquement le
répertoire d'extraction créé par l'utilisateur. Ne supprimez pas `releases/` ni les
données d'un autre build sans décision explicite. Aucun profil navigateur de partage
n'est actuellement géré par un Browser Worker réel.
