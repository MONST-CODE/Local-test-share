# Installation — préversion de développement DevLink

## Avertissement de portée

La branche actuelle n'est pas une release LAN qualifiée. Le partage réel
(WebView2, images chiffrées, entrées, modes A/B/C, messagerie et signalements)
fonctionne sous Windows 10/11 x64 et est testé sur **une** machine ; il n'a pas
encore été exécuté entre deux ordinateurs. Aucune archive Release candidate n'est
publiée : ne déployez pas une build de développement comme plateforme de partage.

Prérequis d'exécution : Windows 10/11 x64 ; runtime Microsoft Edge WebView2 sur
l'hôte (présent par défaut sur Windows 11). Les DLL nécessaires (SDL3, OpenSSL 3,
WebView2Loader, runtime UCRT64) sont copiées à côté des exécutables par le build.

## Build de développeur

Les prérequis et les commandes officielles sont dans `docs/BUILD.md` :

- Windows x64 : MSYS2 UCRT64 Clang++ natif, exécuté sur Windows ;
- Linux x64 : Clang++ >=18 et dépendances native/SDL indiquées par `build.sh`.

Les résultats GCC sont uniquement exploratoires. Une compilation Linux ne qualifie
ni les exécutables Windows, ni WebView2, ni TLS Windows, ni le DPI physique.

## Packages

Le script `package` teste la build, crée une archive, l'extrait dans un dossier
propre, réexécute les binaires hors MSYS2 puis publie l'archive vérifiée sous
`releases/` avec une empreinte SHA-256. Il n'a pas été exécuté pour cette version
(aucune publication sans qualification LAN préalable). Voir
`docs/reports/IMPLEMENTATION_STATUS.md`.

Aucun prérequis matériel pour cinq invités, aucun support Windows/Linux complet,
aucun runtime redistribué et aucune compatibilité réseau ne sont garantis par ce
guide tant que la qualification correspondante n'a pas été publiée.

## Supprimer une build locale

Fermer les processus DevLink lancés localement, puis supprimer uniquement le
répertoire d'extraction créé par l'utilisateur. Ne supprimez pas `releases/` ni les
données d'un autre build sans décision explicite. Les profils navigateur de
partage sont temporaires (InPrivate) et détruits à l'arrêt de la session.
