#!/usr/bin/env bash
# DevLink's authoritative Linux build. No generated project, build engine or helper runtime.
#
# Affichage (meme modele que build.bat) : une etape numerotee par phase reelle
# ([n/N]), une ligne de progression reecrite sur place dans un terminal, aucune
# sortie brute d'outil. Chaque commande garde sa capture complete sous
# out/linux/logs ; un echec affiche la sortie de la commande fautive, telle
# quelle. --verbose retablit la sortie integrale ; --plain ou NO_COLOR retirent
# couleurs et reecriture. Hors terminal (CI, tube), la sortie reste lineaire.
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
cd -- "$ROOT"
ACTION=${1:-debug}; if (( $# )); then shift; fi
MODE=debug; VERBOSE=0; HEADLESS=0; CORE_ONLY=0; FETCH=0; NATIVE_UI=0; PLAIN=0
if [[ $ACTION == test && ${1:-} =~ ^(debug|release|asan)$ ]]; then MODE=$1; shift; fi
for arg in "$@"; do
    case "$arg" in
        --verbose) VERBOSE=1;;
        --headless) HEADLESS=1;;
        --core-only) CORE_ONLY=1;;
        --fetch) FETCH=1;;
        --native-ui) NATIVE_UI=1;;
        --plain) PLAIN=1;;
        *) printf 'ERROR: unknown argument: %s (see ./build.sh help)\n' "$arg" >&2; exit 2;;
    esac
done
case "$ACTION" in release|package) MODE=release;; asan|fuzz) MODE=asan;; esac
case "$ACTION" in debug|release|test|asan|fuzz|clean|package|doctor|deps|selftest|help) ;; *) echo "ERROR: unknown operation: $ACTION (see ./build.sh help)" >&2; exit 2;; esac

# --- Interface console -------------------------------------------------------
# fd 3 = console d'origine. run() se tait sur la console, mais restitue la sortie
# de la commande quand l'appelant l'a redirigee (run cmd > fichier).
exec 3>&1
COLOR=0; LIVE=0
if [[ -t 1 && -z ${NO_COLOR:-} && ${DEVLINK_PLAIN:-} != 1 && $PLAIN == 0 ]]; then COLOR=1; fi
if (( COLOR && ! VERBOSE )) && [[ ${GITHUB_ACTIONS:-} != true ]]; then LIVE=1; fi
C0=; C_B1=; C_B2=; C_B3=; C_B4=; C_B5=; C_B6=; C_ACC=; C_CREAM=; C_MUTED=; C_DIM=; C_OK=; C_WARN=; C_ERR=; C_BOLD=; EL=; COL=; COL_ROW=
if (( COLOR )); then
    # Degrade marron de la marque (assets/branding/BRAND_GUIDELINES.md, themes.cpp).
    C0=$'\033[0m'; C_BOLD=$'\033[1m'
    C_B1=$'\033[38;2;222;188;152m'; C_B2=$'\033[38;2;206;166;128m'; C_B3=$'\033[38;2;196;154;120m'
    C_B4=$'\033[38;2;180;138;104m'; C_B5=$'\033[38;2;164;122;89m'; C_B6=$'\033[38;2;129;92;67m'
    C_ACC=$'\033[38;2;196;154;120m'; C_CREAM=$'\033[38;2;245;233;220m'; C_MUTED=$'\033[38;2;187;169;154m'
    C_DIM=$'\033[38;2;140;122;108m'; C_OK=$'\033[38;2;104;163;87m'; C_WARN=$'\033[38;2;217;155;67m'
    C_ERR=$'\033[38;2;199;84;80m'; EL=$'\033[2K\r'; COL=$'\033[62G'; COL_ROW=$'\033[26G'
fi
# Memes glyphes que build.bat (presents dans les polices console courantes).
G_OK='√'; G_FAIL='×'; G_SKIP='○'; G_RUN='•'; G_DOT='·'
RULE='────────────────────────────────────────────────────────────────────────'
STEP=0; TOTAL=0; STEP_OPEN=0; STEP_LABEL=; STEP_START=0; STEP_DUR=; FAILED_STEP=
UNIT_INDEX=0; UNIT_TOTAL=0; STEP_ITEM=; RUN_INDEX=0; RUN_QUIET=0
SUMMARY_ARTIFACT=; SUMMARY_ARTIFACT_LABEL='Binaires   '; NOTES=(); ROWS=(); CLI_PASS=0; UI_PASS=0; UI_NOTE=
now_cs() {
    if [[ -n ${EPOCHREALTIME:-} ]]; then local t=${EPOCHREALTIME/[.,]/}; printf '%s' $(( 10#$t / 10000 )); else printf '%s' $(( SECONDS * 100 )); fi
}
fmt_duration() {
    local cs=$1 s=$(( $1 / 100 ))
    if (( s < 60 )); then printf '%d,%d s' "$s" $(( (cs % 100) / 10 )); else printf '%d min %02d s' $(( s / 60 )) $(( s % 60 )); fi
}
OP_START=$(now_cs)
relpath() { printf '%s' "${1#"$ROOT"/}"; }
banner() {
    local file="$ROOT/tools/build/devlink-banner.txt" line index=0
    local colors=("$C_B1" "$C_B2" "$C_B3" "$C_B4" "$C_B5" "$C_B6")
    printf '\n' >&3
    if [[ -f $file ]]; then
        while IFS= read -r line || [[ -n $line ]]; do
            [[ -z $line || $line == ';'* ]] && continue
            printf '  %s%s%s\n' "${colors[$(( index < 5 ? index : 5 ))]}" "$line" "$C0" >&3
            index=$((index+1))
        done < "$file"
    else
        printf '  %s%sD E V L I N K%s\n' "$C_B3" "$C_BOLD" "$C0" >&3
    fi
    printf '\n  %s%sDevLink %s%s  %s%s%s  %sbuild Linux x64%s  %s%s%s  %sclang++%s\n' "$C_CREAM" "$C_BOLD" "${PRODUCT_VERSION:-?}" "$C0" \
        "$C_DIM" "$G_DOT" "$C0" "$C_MUTED" "$C0" "$C_DIM" "$G_DOT" "$C0" "$C_MUTED" "$C0" >&3
    if [[ $ACTION != help ]]; then
        printf '  %sOpération%s  %s%s%s   %s%s%s   %sJournal%s  %s%s%s\n' "$C_MUTED" "$C0" "$C_CREAM" "$OP_TEXT" "$C0" \
            "$C_DIM" "$G_DOT" "$C0" "$C_MUTED" "$C0" "$C_DIM" "$(relpath "$LOG")" "$C0" >&3
    fi
    printf '  %s%s%s\n\n' "$C_DIM" "$RULE" "$C0" >&3
}
step() {
    STEP=$((STEP+1)); STEP_LABEL=$1; STEP_OPEN=1; UNIT_INDEX=0; UNIT_TOTAL=0; STEP_ITEM=; STEP_START=$(now_cs)
    printf '\n==== [%s/%s] %s\n' "$STEP" "$TOTAL" "$1" >> "$LOG"
    if (( LIVE )); then printf '%s  %s%s%s  %s[%s/%s]%s %s%s%s' "$EL" "$C_DIM" "$G_RUN" "$C0" "$C_ACC" "$STEP" "$TOTAL" "$C0" "$C_CREAM" "$1" "$C0" >&3
    else printf '  %s[%s/%s]%s %s\n' "$C_ACC" "$STEP" "$TOTAL" "$C0" "$1" >&3; fi
}
step_progress() {
    # $1 = element courant ; rang/total lus dans UNIT_INDEX/UNIT_TOTAL. Interactif seulement.
    (( LIVE )) || return 0
    local where="$UNIT_INDEX/$UNIT_TOTAL $G_DOT $1"; (( UNIT_TOTAL > 0 )) || where=$1
    printf '%s  %s%s%s  %s[%s/%s]%s %s%s%s%s%s%s%s' "$EL" "$C_DIM" "$G_RUN" "$C0" "$C_ACC" "$STEP" "$TOTAL" "$C0" \
        "$C_CREAM" "$STEP_LABEL" "$C0" "$COL" "$C_DIM" "$where" "$C0" >&3
}
step_elapsed() { STEP_DUR=$(fmt_duration $(( $(now_cs) - STEP_START ))); }
step_done() {
    step_elapsed; STEP_OPEN=0
    local detail=$STEP_DUR; [[ -z ${1:-} ]] || detail="$1 $G_DOT $STEP_DUR"
    printf '==== OK [%s/%s] %s - %s\n' "$STEP" "$TOTAL" "$STEP_LABEL" "$detail" >> "$LOG"
    if (( LIVE )); then printf '%s  %s%s%s  %s[%s/%s]%s %s%s%s%s%s\n' "$EL" "$C_OK" "$G_OK" "$C0" "$C_ACC" "$STEP" "$TOTAL" "$C0" "$STEP_LABEL" "$COL" "$C_DIM" "$detail" "$C0" >&3
    else printf '       %s%s%s %s\n' "$C_OK" "$G_OK" "$C0" "$detail" >&3; fi
}
step_skip() {
    # Etape non executable ici : jamais presentee comme reussie.
    step_elapsed; STEP_OPEN=0
    printf '==== SKIP [%s/%s] %s - %s\n' "$STEP" "$TOTAL" "$STEP_LABEL" "$1" >> "$LOG"
    if (( LIVE )); then printf '%s  %s%s%s  %s[%s/%s]%s %s%s%s%s%s\n' "$EL" "$C_WARN" "$G_SKIP" "$C0" "$C_ACC" "$STEP" "$TOTAL" "$C0" "$STEP_LABEL" "$COL" "$C_WARN" "$1" "$C0" >&3
    else printf '       %s%s%s %s\n' "$C_WARN" "$G_SKIP" "$C0" "$1" >&3; fi
}
step_fail() {
    (( STEP_OPEN )) || return 0
    step_elapsed; STEP_OPEN=0; FAILED_STEP="[$STEP/$TOTAL] $STEP_LABEL"
    printf '==== FAIL [%s/%s] %s - %s\n' "$STEP" "$TOTAL" "$STEP_LABEL" "$STEP_DUR" >> "$LOG"
    if (( LIVE )); then printf '%s  %s%s%s  %s[%s/%s]%s %s%s%séchec %s %s%s\n' "$EL" "$C_ERR" "$G_FAIL" "$C0" "$C_ACC" "$STEP" "$TOTAL" "$C0" "$STEP_LABEL" "$COL" "$C_ERR" "$G_DOT" "$STEP_DUR" "$C0" >&3
    else printf '       %s%s échec%s %s %s\n' "$C_ERR" "$G_FAIL" "$C0" "$G_DOT" "$STEP_DUR" >&3; fi
}
row() {
    # Ligne de resultat sous une etape : $1 = ok|skip|note, $2 = nom, $3 = detail.
    local glyph="$C_OK$G_OK$C0"
    case $1 in skip) glyph="$C_WARN$G_SKIP$C0";; note) glyph="$C_DIM$G_DOT$C0";; esac
    if (( COLOR )); then printf '         %s %s%s%s%s%s%s%s\n' "$glyph" "$C_CREAM" "$2" "$C0" "$COL_ROW" "$C_DIM" "$3" "$C0" >&3
    else printf '         %s %s - %s\n' "$glyph" "$2" "$3" >&3; fi
}
note_later() { printf 'NOTE: %s\n' "$1" >> "$LOG"; NOTES+=("$1"); }
print_excerpt() {
    # $1 = fichier, $2 = lignes maximum, $3 = capture complete. Lignes BRUTES : un
    # "fichier:ligne:col: error:" reste reconnu par les editeurs.
    local total; total=$(wc -l < "$1"); total=${total// /}
    (( total > 0 )) || return 0
    printf '    %s┌─ sortie%s\n' "$C_DIM" "$C0" >&3
    head -n "$2" -- "$1" >&3
    if (( total > $2 )); then printf '    %s└─ %s ligne(s) de plus : %s%s\n' "$C_DIM" $(( total - $2 )) "$(relpath "$3")" "$C0" >&3
    else printf '    %s└─ capture complète : %s%s\n' "$C_DIM" "$(relpath "$3")" "$C0" >&3; fi
}
summary() {
    local rc=$1 duration; duration=$(fmt_duration $(( $(now_cs) - OP_START )))
    printf '\n  %s%s%s\n' "$C_DIM" "$RULE" "$C0" >&3
    if (( rc == 0 )); then
        printf '  %s%s%s Réussite%s  %s%s%s  %s%s %s/%s étapes %s %s%s\n' "$C_OK" "$C_BOLD" "$G_OK" "$C0" "$C_CREAM" "$OP_TEXT" "$C0" "$C_DIM" "$G_DOT" "$STEP" "$TOTAL" "$G_DOT" "$duration" "$C0" >&3
    elif (( rc == 77 )); then
        printf '  %s%s%s Terminé avec des étapes ignorées%s  %s%s%s  %s%s code 77 %s %s%s\n' "$C_WARN" "$C_BOLD" "$G_SKIP" "$C0" "$C_CREAM" "$OP_TEXT" "$C0" "$C_DIM" "$G_DOT" "$G_DOT" "$duration" "$C0" >&3
        printf '    %sUn SKIP n'"'"'est jamais compté comme une réussite : la qualification reste incomplète.%s\n' "$C_MUTED" "$C0" >&3
    else
        printf '  %s%s%s Échec%s  %s%s%s  %s%s code %s %s %s%s\n' "$C_ERR" "$C_BOLD" "$G_FAIL" "$C0" "$C_CREAM" "$OP_TEXT" "$C0" "$C_DIM" "$G_DOT" "$rc" "$G_DOT" "$duration" "$C0" >&3
        [[ -z $FAILED_STEP ]] || printf '    %sÉtape%s      %s\n' "$C_MUTED" "$C0" "$FAILED_STEP" >&3
    fi
    [[ -z $SUMMARY_ARTIFACT ]] || printf '    %s%s%s%s%s%s\n' "$C_MUTED" "$SUMMARY_ARTIFACT_LABEL" "$C0" "$C_CREAM" "$(relpath "$SUMMARY_ARTIFACT")" "$C0" >&3
    printf '    %sJournal    %s%s%s%s\n' "$C_MUTED" "$C0" "$C_DIM" "$(relpath "$LOG")" "$C0" >&3
    local note; for note in "${NOTES[@]}"; do printf '    %s%s %s%s\n' "$C_DIM" "$G_DOT" "$note" "$C0" >&3; done
    if (( rc != 0 && rc != 77 && ! VERBOSE )); then printf '    %sRelancez avec --verbose pour afficher chaque commande et sa sortie.%s\n' "$C_DIM" "$C0" >&3; fi
    printf '\n' >&3
}
OP_TEXT=$ACTION; [[ $ACTION != test ]] || OP_TEXT="test $MODE"
(( ! CORE_ONLY )) || OP_TEXT+=' --core-only'; (( ! HEADLESS )) || OP_TEXT+=' --headless'; (( ! NATIVE_UI )) || OP_TEXT+=' --native-ui'
PRODUCT_VERSION=$(sed -n 's/^inline constexpr std::string_view kProductVersion = "\([^"]*\)";$/\1/p' "$ROOT/engine/core/product.hpp" 2>/dev/null || true)
if [[ $ACTION == help ]]; then
    banner
    printf '  %sUsage%s  ./build.sh <opération> [options]\n\n  %sOpérations%s\n' "$C_CREAM" "$C0" "$C_ACC" "$C0"
    printf '    %-22s%s\n' 'debug' 'Compile la configuration Debug (opération par défaut)' \
        'release' 'Compile la configuration Release optimisée' \
        'test [debug|release|asan]' '' \
        '' 'Compile depuis zéro puis exécute toutes les suites de tests' \
        'asan' 'Tests avec ASan + UBSan' \
        'fuzz' "Fuzzing libFuzzer de l'enveloppe" \
        'package' 'Release + tests + archive publiée dans releases/' \
        'doctor' 'Diagnostique le compilateur, le linker et les dépendances' \
        'deps [--fetch]' 'Vérifie les archives épinglées ; --fetch les télécharge' \
        'clean' 'Supprime les sorties compilées (releases/ et journaux conservés)' \
        'selftest' 'Auto-test des garde-fous du script (échecs volontaires)' \
        'help' 'Affiche cette aide'
    printf '\n  %sOptions%s\n' "$C_ACC" "$C0"
    printf '    %-22s%s\n' '--verbose' 'Affiche chaque commande et sa sortie complète (diagnostic)' \
        '--plain' 'Sortie sans couleur ni réécriture de ligne (aussi : NO_COLOR=1)' \
        '--headless' 'SDL dummy seulement : profil de test, pas une build desktop' \
        '--native-ui' 'Tests : lance aussi une vraie fenêtre X11' \
        '--core-only' 'Moteur et moniteur seulement, sans interface SDL3/ImGui' \
        '--fetch' 'deps : autorise le téléchargement des archives épinglées'
    printf '\n  %sSeul deps --fetch télécharge. Défaut desktop : X11/logiciel. Un SKIP (code 77) n'"'"'est jamais compté comme réussi.%s\n\n' "$C_DIM" "$C0"
    exit 0
fi
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'ERROR: native Linux x86-64 required'; exit 2; }
[[ -f engine/core/types.hpp && ! -L out && ! -L out/linux ]] || { echo 'ERROR: unsafe output root or wrong checkout'; exit 2; }
OUT="$ROOT/out/linux"; DEPS="$OUT/dependencies"
for generated in logs dependencies debug release tests packages doctor; do
    [[ ! -L $OUT/$generated ]] || { echo "ERROR: symlink output directory refused: $OUT/$generated" >&2; exit 2; }
done
mkdir -p "$OUT/logs"
LOG="$OUT/logs/$(date -u +%Y%m%dT%H%M%SZ)-${ACTION}-${MODE}-$$.log"
: > "$LOG"
LOCK=0
cleanup() {
    local rc=$?
    trap - ERR
    if (( LOCK )); then rmdir "$OUT/.build-lock" 2>/dev/null || true; fi
    if (( STEP_OPEN )); then if (( rc == 77 )); then step_skip 'non disponible ici : SKIP, code 77'; elif (( rc != 0 )); then step_fail; fi; fi
    if (( rc == 0 )); then printf 'SUCCESS operation=%s\n' "$ACTION" >> "$LOG"; else printf 'FAILURE operation=%s exit=%s\n' "$ACTION" "$rc" >> "$LOG"; fi
    summary "$rc"
    exit "$rc"
}
trap cleanup EXIT
# say : journal toujours, console seulement en --verbose. info : console et journal.
say() { printf '%s\n' "$*" >> "$LOG"; if (( VERBOSE )); then printf '%s\n' "$*" >&3; fi; }
notice() { if [[ ${GITHUB_ACTIONS:-} == true ]]; then printf '::notice title=DevLink Linux::%s\n' "$*" >&3; fi; }
fail() {
    printf 'FAILURE: %s\n' "$*" >> "$LOG"
    # Sortie redirigee par l'appelant (selftest : grep du motif) : elle recoit aussi le motif.
    if [[ ! /proc/self/fd/1 -ef /proc/self/fd/3 ]]; then printf 'FAILURE: %s\n' "$*"; fi
    if [[ ${DEVLINK_QUIET_PROBLEM:-} != 1 ]]; then step_fail; printf '    %s%s %s%s\n' "$C_ERR" "$G_FAIL" "$*" "$C0" >&3; fi
    exit 1
}
command_text() { printf '%q ' "$@"; }
# Capture complete de chaque commande (journal + fichier dedie), statut jamais
# masque par un tube. Console : rien, sauf --verbose ou echec (run_failed).
run() {
    local text capture rc=0
    text=$(command_text "$@"); printf '+ %s\n' "$text" >> "$LOG"
    if (( VERBOSE )); then printf '+ %s\n' "$text" >&3; fi
    RUN_INDEX=$((RUN_INDEX+1)); capture="$LOG.command-$RUN_INDEX.txt"
    "$@" > "$capture" 2>&1 || rc=$?
    cat -- "$capture" >> "$LOG" || { echo 'ERROR: cannot preserve command log' >&2; return 1; }
    if [[ ! /proc/self/fd/1 -ef /proc/self/fd/3 ]]; then cat -- "$capture"
    elif (( VERBOSE )); then cat -- "$capture" >&3; fi
    if (( rc != 0 )); then
        printf 'Command exit %s: %s\n' "$rc" "$text" >> "$LOG"
        if [[ ${DEVLINK_EXPECT_FAILURE:-${PLPT_EXPECT_FAILURE:-}} != 1 ]] && (( ! RUN_QUIET )); then
            step_fail
            local item=; [[ -z $STEP_ITEM ]] || item=" $G_DOT $STEP_ITEM"
            printf '    %s%s %s a échoué%s %s(code %s)%s%s\n' "$C_ERR" "$G_FAIL" "$(basename -- "$1")" "$C0" "$C_DIM" "$rc" "$item" "$C0" >&3
            print_excerpt "$capture" 60 "$capture"
        fi
    fi
    return "$rc"
}
error() {
    local rc=$?
    printf 'FAILURE exit=%s operation=%s (see %s)\n' "$rc" "$ACTION" "$LOG" >> "$LOG"
    if [[ ${GITHUB_ACTIONS:-} == true && ${DEVLINK_EXPECT_FAILURE:-${PLPT_EXPECT_FAILURE:-}} != 1 ]]; then
        echo "::error title=DevLink Linux build::Operation $ACTION failed, exit $rc. See preserved log." >&3
        local details
        details=$(grep -E 'error:|undefined reference|Command exit|FAILURE|FAIL ' "$LOG" | tail -4 || true)
        notice "failure context: ${details//$'\n'/%0A}"
    fi
    exit "$rc"
}
trap error ERR
banner
mkdir "$OUT/.build-lock" 2>/dev/null || {
    printf '    %s%s un autre build détient %s%s\n' "$C_ERR" "$G_FAIL" "$(relpath "$OUT/.build-lock")" "$C0" >&3
    printf '    %sVérifiez qu'"'"'aucun build ne tourne, puis : rmdir %q%s\n' "$C_MUTED" "$OUT/.build-lock" "$C0" >&3
    exit 1
}
LOCK=1
say "DevLink direct Linux operation=$ACTION mode=$MODE; log=$LOG"
plain_directory() {
    local path=$1
    [[ $path == "$OUT/"* && $path != "$OUT/" ]] || fail "refusing unsafe output path: $path"
    local current=$path
    while [[ $current != "$OUT" ]]; do
        [[ ! -L $current ]] || fail "refusing symlink output component: $current"
        current=$(dirname "$current")
    done
}
fresh() { plain_directory "$1"; if [[ -e $1 ]]; then run rm -rf -- "$1"; fi; run mkdir -p -- "$1"; }
# Archives are verified on EVERY operation and re-extracted in each fresh build.
SDL_COMMIT=f00dc919ca35b9756316e86c4777392de442f9b2
IMGUI_COMMIT=c1aaac65e896a45f81f47a4d68f9959e0f86c859
SDL_SHA=e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0
IMGUI_SHA=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114
SDL_ARCHIVE="$DEPS/SDL-$SDL_COMMIT.tar.gz"
IMGUI_ARCHIVE="$DEPS/imgui-$IMGUI_COMMIT.tar.gz"
verify() {
    [[ -f $1 && ! -L $1 ]] || fail "missing archive $1; run ./build.sh deps --fetch or provision that exact verified archive"
    local got; got=$(sha256sum "$1"); got=${got%% *}
    [[ $got == "$2" ]] || fail "SHA-256 mismatch: $1 expected=$2 actual=$got (not extracted)"
    say "SHA256 OK $got $1"
    if [[ $ACTION == deps && $1 != *.part ]]; then notice "dependency SHA256 $got $(basename "$1")"; fi
}
dependencies() {
    (( CORE_ONLY )) && return 0
    verify "$SDL_ARCHIVE" "$SDL_SHA"; verify "$IMGUI_ARCHIVE" "$IMGUI_SHA"
}
fetch_one() {
    local url=$1 file=$2 hash=$3
    if [[ -f $file ]]; then verify "$file" "$hash"; return; fi
    [[ ! -L $file.part ]] || fail "symlink partial archive refused: $file.part"
    run curl --fail --location --show-error --proto '=https' --tlsv1.2 --output "$file.part" "$url"
    verify "$file.part" "$hash"; run mv -- "$file.part" "$file"
}
CXX=; CC=
# Version publique unique (mandat §16) : relue depuis engine/core/product.hpp pour
# que l'archive, le staging et les rapports ne puissent pas diverger du --version.
product_version() {
    local version
    version=$(sed -n 's/^inline constexpr std::string_view kProductVersion = "\([^"]*\)";$/\1/p' "$ROOT/engine/core/product.hpp")
    [[ -n $version ]] || fail 'kProductVersion introuvable dans engine/core/product.hpp'
    printf '%s' "$version"
}
toolchain() {
    # Knobs developpeur : DEVLINK_* sont les noms publics ; PLPT_* restent honores
    # une release le temps de la migration (un avertissement le signale).
    if [[ -z ${DEVLINK_CXX:-} && -n ${PLPT_CXX:-} ]]; then say 'PLPT_CXX est deprecie : utiliser DEVLINK_CXX'; fi
    if [[ -z ${DEVLINK_CC:-} && -n ${PLPT_CC:-} ]]; then say 'PLPT_CC est deprecie : utiliser DEVLINK_CC'; fi
    CXX=$(command -v -- "${DEVLINK_CXX:-${PLPT_CXX:-clang++}}") || fail 'clang++ missing; install LLVM or set DEVLINK_CXX to its executable'
    CC=$(command -v -- "${DEVLINK_CC:-${PLPT_CC:-$(dirname "$CXX")/clang}}") || fail 'matching clang C compiler missing (SDL C sources)'
    [[ -x $CXX && -x $CC ]] || fail 'selected compiler is not executable'
    local version target cversion
    version=$("$CXX" --version); target=$("$CXX" -dumpmachine); cversion=$("$CC" --version)
    [[ $version == *'clang version '* && $cversion == *'clang version '* ]] || fail 'only LLVM clang++ / clang are permitted'
    local major cmajor; major=$("$CXX" -dumpversion); major=${major%%.*}; cmajor=$("$CC" -dumpversion); cmajor=${cmajor%%.*}
    [[ $major =~ ^[0-9]+$ && $major -ge 18 && $major == "$cmajor" ]] || fail 'matching LLVM clang/clang++ >=18 required'
    [[ $target == x86_64-*-linux* && $("$CC" -dumpmachine) == "$target" ]] || fail "native Linux x86-64 compilers required; got $target"
    say "CXX=$CXX"; say "CC=$CC"; say "$version"; notice "compiler ${version%%$'\n'*}; target=$target"
    say "native linker: $("$CXX" -print-prog-name=ld)"
    run "$CXX" --version
    if (( ! CORE_ONLY && ! HEADLESS )); then
        [[ -f /usr/include/X11/Xlib.h && -f /usr/include/X11/extensions/Xext.h ]] || fail 'X11 development headers missing; install libx11-dev libxext-dev, or explicitly use --headless for tests only'
    fi
    run mkdir -p "$OUT/doctor"
    run "$CXX" -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread tests/build/toolchain_probe.cpp -o "$OUT/doctor/probe"
    [[ -x $OUT/doctor/probe ]] || fail 'toolchain probe binary missing after successful compiler return'
    run "$OUT/doctor/probe" > "$OUT/doctor/probe.log"
    cat "$OUT/doctor/probe.log" >> "$LOG"
    if [[ $ACTION == doctor ]]; then
        local info="$OUT/doctor/environment.txt" linker_cmd
        linker_cmd=$("$CXX" -print-prog-name=ld)
        run uname -srvmo > "$info"
        run getconf GNU_LIBC_VERSION >> "$info"
        run "$linker_cmd" --version >> "$info"
        cat "$OUT/doctor/probe.log" >> "$info"
        if (( ! CORE_ONLY && ! HEADLESS )) && command -v dpkg-query >/dev/null; then
            run dpkg-query -W '-f=${Package} ${Version}\n' libc6 libstdc++6 libx11-6 libxext6 >> "$info"
        fi
        cat "$info" >> "$LOG"
        local metadata; metadata=$(cat "$info"); notice "native environment: ${metadata//$'\n'/%0A}"
    fi
    local vnum=${version#*clang version }; vnum=${vnum%%[ $'\n']*}
    TOOLCHAIN_SUMMARY="clang $vnum $G_DOT $target"
}
# Explicit DevLink targets. The CLI monitor stays model-free; the desktop control
# post and the tests link the session models and the real LAN networking.
CORE_SOURCES=(engine/core/random.cpp engine/core/digest.cpp engine/core/mac.cpp engine/core/secrets.cpp
    engine/core/format.cpp engine/diagnostics/audit.cpp
    engine/discovery/process_tree.cpp engine/discovery/terminal_classifier.cpp
    engine/discovery/monitor.cpp engine/service_registry/registry.cpp
    engine/port_allocator/port_allocator.cpp engine/platform/socket.cpp
    engine/platform/linux/linux_platform.cpp)
MODEL_SOURCES=(engine/session/session.cpp engine/messaging/messaging.cpp
    engine/protocol/envelope.cpp engine/resource_manager/resource.cpp)
# Real networking: control plane (host/guest), discovery, data plane (proxy).
# Transport versionné (mandat §24, ADR-016) : TCP clair qui se déclare clair et
# TLS 1.3 PSK avec authentification mutuelle. Depuis le basculement (étape 5)
# il est EN PRODUCTION : l'hôte et l'invité l'utilisent pour le canal de
# contrôle, donc il suit le régime des autres sources réseau.
NET_SOURCES=(engine/net/wire.cpp engine/net/discovery.cpp engine/net/host_server.cpp
    engine/net/guest_client.cpp engine/net/proxy.cpp
    engine/net/transport.cpp engine/net/transport_tls.cpp)
# Moteur navigateur reel : la fabrique selectionne WebView2 quand il existe et
# gere le repli. Lie au poste de commande et aux tests, jamais au moniteur CLI.
BROWSER_SOURCES=(engine/browser/webview2_backend.cpp)
# Design system DevLink : themes.cpp porte les palettes DevDark/DevLight et
# l'application du style ImGui. Sans cette unite, ThemeManager ne se lie pas.
UI_SOURCES=(app/desktop_ui/design_system/themes.cpp
    app/desktop_ui/design_system/typography.cpp
    app/desktop_ui/design_system/window_icon_data.cpp)
IMGUI_SOURCES=(imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp
    backends/imgui_impl_sdl3.cpp backends/imgui_impl_sdlrenderer3.cpp)
HEADERS=(
    engine/browser/browser.hpp
    engine/browser/remote_frame.hpp
    engine/media/frame_queue.hpp
    engine/browser/url_origin.hpp
    engine/browser/webview2_backend.hpp
    engine/core/clock.hpp
    engine/core/digest.hpp
    engine/core/format.hpp
    engine/core/mac.hpp
    engine/core/random.hpp
    engine/core/result.hpp
    engine/core/secrets.hpp
    engine/core/text.hpp
    engine/core/types.hpp
    engine/diagnostics/audit.hpp
    engine/discovery/monitor.hpp
    engine/discovery/process_tree.hpp
    engine/discovery/terminal_classifier.hpp
    engine/messaging/messaging.hpp
    engine/net/chat.hpp
    engine/net/discovery.hpp
    engine/net/guest_client.hpp
    engine/net/host_server.hpp
    engine/net/proxy.hpp
    engine/net/transport.hpp
    engine/net/wire.hpp
    engine/platform/connection_pool.hpp
    engine/platform/native.hpp
    engine/platform/platform.hpp
    engine/platform/socket.hpp
    engine/port_allocator/port_allocator.hpp
    engine/protocol/envelope.hpp
    engine/resource_manager/resource.hpp
    engine/service_registry/registry.hpp
    engine/session/session.hpp
    engine/platform/linux/linux_platform.hpp)
# En-tetes du poste de commande : ils incluent <imgui.h>, donc leur sonde
# d'independance recoit les memes chemins -isystem que sa compilation reelle.
UI_HEADERS=(
    app/desktop_ui/design_system/tokens.hpp
    app/desktop_ui/design_system/themes.hpp
    app/desktop_ui/design_system/typography.hpp
    app/desktop_ui/design_system/icons.hpp
    app/desktop_ui/design_system/components.hpp
    app/desktop_ui/design_system/layout.hpp
    app/desktop_ui/design_system/window_icon_data.hpp)
# SDL has no official Linux binary development archive for 3.4.20. These are
# explicit sources of our small POSIX + X11/software (or explicit dummy) profile.
# No configure script, glob-generated target, or upstream build generator is used.
SDL_SOURCES=(
    src/SDL.c src/SDL_assert.c src/SDL_error.c src/SDL_guid.c src/SDL_hints.c
    src/SDL_hashtable.c src/SDL_list.c src/SDL_log.c src/SDL_properties.c src/SDL_utils.c
    src/atomic/SDL_atomic.c src/atomic/SDL_spinlock.c
    src/audio/SDL_audio.c src/audio/SDL_audiocvt.c src/audio/SDL_audiodev.c
    src/audio/SDL_audioqueue.c src/audio/SDL_audioresample.c src/audio/SDL_audiotypecvt.c
    src/audio/SDL_mixer.c src/audio/SDL_wave.c src/audio/dummy/SDL_dummyaudio.c
    src/camera/SDL_camera.c src/camera/dummy/SDL_camera_dummy.c
    src/core/SDL_core_unsupported.c src/core/unix/SDL_appid.c src/core/unix/SDL_poll.c
    src/core/unix/SDL_gtk.c src/core/unix/SDL_fribidi.c src/core/unix/SDL_libthai.c
    src/core/linux/SDL_threadprio.c src/cpuinfo/SDL_cpuinfo.c src/dynapi/SDL_dynapi.c
    src/events/SDL_categories.c src/events/SDL_clipboardevents.c src/events/SDL_displayevents.c
    src/events/SDL_dropevents.c src/events/SDL_events.c src/events/SDL_eventwatch.c
    src/events/SDL_keyboard.c src/events/SDL_keymap.c src/events/SDL_keysym_to_keycode.c
    src/events/SDL_keysym_to_scancode.c src/events/SDL_mouse.c src/events/SDL_pen.c
    src/events/SDL_quit.c src/events/SDL_scancode_tables.c src/events/SDL_touch.c
    src/events/SDL_windowevents.c src/events/imKStoUCS.c
    src/filesystem/SDL_filesystem.c src/filesystem/unix/SDL_sysfilesystem.c
    src/filesystem/posix/SDL_sysfsops.c src/gpu/SDL_gpu.c
    src/haptic/SDL_haptic.c src/haptic/dummy/SDL_syshaptic.c src/hidapi/SDL_hidapi.c
    src/io/SDL_asyncio.c src/io/SDL_iostream.c src/io/generic/SDL_asyncio_generic.c
    src/joystick/SDL_joystick.c src/joystick/SDL_gamepad.c src/joystick/controller_type.c
    src/joystick/SDL_steam_virtual_gamepad.c src/joystick/dummy/SDL_sysjoystick.c
    src/loadso/dlopen/SDL_sysloadso.c src/locale/SDL_locale.c src/locale/unix/SDL_syslocale.c
    src/main/SDL_main_callbacks.c src/main/SDL_runapp.c src/main/generic/SDL_sysmain_callbacks.c
    src/misc/SDL_url.c src/misc/dummy/SDL_sysurl.c src/power/SDL_power.c
    src/process/SDL_process.c src/process/dummy/SDL_dummyprocess.c
    src/render/SDL_render.c src/render/SDL_render_unsupported.c src/render/SDL_yuv_sw.c
    src/render/software/SDL_blendfillrect.c src/render/software/SDL_blendline.c
    src/render/software/SDL_blendpoint.c src/render/software/SDL_drawline.c
    src/render/software/SDL_drawpoint.c src/render/software/SDL_render_sw.c
    src/render/software/SDL_triangle.c
    src/sensor/SDL_sensor.c src/sensor/dummy/SDL_dummysensor.c
    src/stdlib/SDL_crc16.c src/stdlib/SDL_crc32.c src/stdlib/SDL_getenv.c
    src/stdlib/SDL_iconv.c src/stdlib/SDL_malloc.c src/stdlib/SDL_memcpy.c
    src/stdlib/SDL_memmove.c src/stdlib/SDL_memset.c src/stdlib/SDL_mslibc.c
    src/stdlib/SDL_murmur3.c src/stdlib/SDL_qsort.c src/stdlib/SDL_random.c
    src/stdlib/SDL_stdlib.c src/stdlib/SDL_string.c src/stdlib/SDL_strtokr.c
    src/storage/SDL_storage.c src/storage/generic/SDL_genericstorage.c
    src/storage/steam/SDL_steamstorage.c src/thread/SDL_thread.c
    src/thread/pthread/SDL_syscond.c src/thread/pthread/SDL_sysmutex.c
    src/thread/pthread/SDL_sysrwlock.c src/thread/pthread/SDL_syssem.c
    src/thread/pthread/SDL_systhread.c src/thread/pthread/SDL_systls.c
    src/time/SDL_time.c src/time/unix/SDL_systime.c src/timer/SDL_timer.c
    src/timer/unix/SDL_systimer.c src/tray/SDL_tray_utils.c src/tray/dummy/SDL_tray.c
    src/dialog/SDL_dialog.c src/dialog/SDL_dialog_utils.c src/dialog/dummy/SDL_dummydialog.c
    src/video/SDL_RLEaccel.c src/video/SDL_blit.c src/video/SDL_blit_0.c
    src/video/SDL_blit_1.c src/video/SDL_blit_A.c src/video/SDL_blit_N.c
    src/video/SDL_blit_auto.c src/video/SDL_blit_copy.c src/video/SDL_blit_slow.c
    src/video/SDL_bmp.c src/video/SDL_clipboard.c src/video/SDL_egl.c
    src/video/SDL_fillrect.c src/video/SDL_pixels.c src/video/SDL_rect.c
    src/video/SDL_rotate.c src/video/SDL_stb.c src/video/SDL_stretch.c
    src/video/SDL_surface.c src/video/SDL_video.c src/video/SDL_video_unsupported.c
    src/video/SDL_vulkan_utils.c src/video/SDL_yuv.c
    src/video/yuv2rgb/yuv_rgb_lsx.c src/video/yuv2rgb/yuv_rgb_sse.c src/video/yuv2rgb/yuv_rgb_std.c
    src/video/dummy/SDL_nullevents.c src/video/dummy/SDL_nullframebuffer.c src/video/dummy/SDL_nullvideo.c
    src/libm/e_atan2.c src/libm/e_exp.c src/libm/e_fmod.c src/libm/e_log.c
    src/libm/e_log10.c src/libm/e_pow.c src/libm/e_rem_pio2.c src/libm/e_sqrt.c
    src/libm/k_cos.c src/libm/k_rem_pio2.c src/libm/k_sin.c src/libm/k_tan.c
    src/libm/s_atan.c src/libm/s_copysign.c src/libm/s_cos.c src/libm/s_fabs.c
    src/libm/s_floor.c src/libm/s_isinf.c src/libm/s_isinff.c src/libm/s_isnan.c
    src/libm/s_isnanf.c src/libm/s_modf.c src/libm/s_scalbn.c src/libm/s_sin.c src/libm/s_tan.c)
SDL_X11_SOURCES=(src/video/x11/SDL_x11clipboard.c src/video/x11/SDL_x11dyn.c
    src/video/x11/SDL_x11events.c src/video/x11/SDL_x11framebuffer.c src/video/x11/SDL_x11keyboard.c
    src/video/x11/SDL_x11messagebox.c src/video/x11/SDL_x11modes.c src/video/x11/SDL_x11mouse.c
    src/video/x11/SDL_x11opengl.c src/video/x11/SDL_x11opengles.c src/video/x11/SDL_x11pen.c
    src/video/x11/SDL_x11settings.c src/video/x11/SDL_x11shape.c src/video/x11/SDL_x11touch.c
    src/video/x11/SDL_x11video.c src/video/x11/SDL_x11vulkan.c src/video/x11/SDL_x11window.c
    src/video/x11/SDL_x11xfixes.c src/video/x11/SDL_x11xinput2.c src/video/x11/SDL_x11xsync.c
    src/video/x11/SDL_x11toolkit.c src/video/x11/SDL_x11xtest.c
    src/video/x11/edid-parse.c src/video/x11/xsettings-client.c
    src/dialog/unix/SDL_zenitymessagebox.c)
# Native configuration: audited literals for Linux x86-64/glibc, not generated by a build engine.
sdl_config() {
    mkdir -p "$DIR/config"
    cat > "$DIR/config/SDL_build_config.h" <<'CONFIG'
#ifndef SDL_build_config_h_
#define SDL_build_config_h_
#include <SDL3/SDL_platform_defines.h>
#define HAVE_LIBC 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_FLOAT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MATH_H 1
#define HAVE_CTYPE_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_POLL_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_ALIGNED_ALLOC 1
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOD 1
#define HAVE_VSSCANF 1
#define HAVE_VSNPRINTF 1
#define HAVE_GETENV 1
#define HAVE_SETENV 1
#define HAVE_UNSETENV 1
#define HAVE_PUTENV 1
#define HAVE_ABS 1
#define HAVE_ISATTY 1
#define HAVE_GETRESUID 1
#define HAVE_GETRESGID 1
#define HAVE_FOPEN64 1
#define HAVE_FSEEKO64 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_NANOSLEEP 1
#define HAVE_SYSCONF 1
#define HAVE_GCC_ATOMICS 1
#define HAVE_GCC_SYNC_LOCK_TEST_AND_SET 1
#define HAVE_PTHREAD_SETNAME_NP 1
#define SDL_THREAD_PTHREAD 1
#define SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1
#define SDL_TIMER_UNIX 1
#define SDL_LOADSO_DLOPEN 1
#define SDL_FILESYSTEM_UNIX 1
#define SDL_FSOPS_POSIX 1
#define SDL_TIME_UNIX 1
#define SDL_VIDEO_DRIVER_DUMMY 1
#define SDL_VIDEO_RENDER_SW 1
#define SDL_AUDIO_DISABLED 1
#define SDL_AUDIO_DRIVER_DUMMY 1
#define SDL_CAMERA_DISABLED 1
#define SDL_CAMERA_DRIVER_DUMMY 1
#define SDL_HAPTIC_DISABLED 1
#define SDL_JOYSTICK_DISABLED 1
#define SDL_SENSOR_DISABLED 1
#define SDL_HIDAPI_DISABLED 1
#define SDL_GPU_DISABLED 1
#define SDL_PROCESS_DUMMY 1
#define SDL_DIALOG_DUMMY 1
#define SDL_TRAY_DUMMY 1
#endif
CONFIG
    if (( ! HEADLESS )); then
        # The supported modern Xlib headers declare these types/APIs, even without XInput2.
        printf '\n#define SDL_VIDEO_DRIVER_X11 1\n#define SDL_VIDEO_DRIVER_X11_SUPPORTS_GENERIC_EVENTS 1\n#define SDL_VIDEO_DRIVER_X11_HAS_XKBLIB 1\n#define HAVE_XINTERNATOMS 1\n' >> "$DIR/config/SDL_build_config.h"
    fi
    printf '#define SDL_REVISION "%s"\n' "$SDL_COMMIT" > "$DIR/config/SDL_revision.h"
}
DIR=; BIN=; SDL_ROOT=; IMGUI_ROOT=; CXXFLAGS=(); MODEFLAGS=(); SAN=(); VERBOSE_FLAG=(); EXTRA_CXX=(); EXTRA_LINK=()
COMMON_OBJECTS=(); MODEL_OBJECTS=(); GUI_OBJECTS=(); SDL_OBJECTS=()
NET_OBJECTS=(); BROWSER_OBJECTS=(); UI_OBJECTS=()
configure_flags() {
    MODEFLAGS=(-O0 -g); SAN=()
    if [[ $MODE == release ]]; then MODEFLAGS=(-O2 -DNDEBUG); fi
    if [[ $MODE == asan ]]; then SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all); fi
    VERBOSE_FLAG=(); if (( VERBOSE )); then VERBOSE_FLAG=(-v); fi
    # Optional developer flags are split as arguments, NEVER eval'ed as shell code.
    read -r -a EXTRA_CXX <<< "${DEVLINK_EXTRA_CXXFLAGS:-${PLPT_EXTRA_CXXFLAGS:-}}"
    read -r -a EXTRA_LINK <<< "${DEVLINK_EXTRA_LDFLAGS:-${PLPT_EXTRA_LDFLAGS:-}}"
    CXXFLAGS=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -Wshadow -pthread -I "$ROOT" "${MODEFLAGS[@]}" "${SAN[@]}" "${EXTRA_CXX[@]}")
}
compile_cpp() {
    local source=$1 object=$2; shift 2
    mkdir -p "$(dirname "$object")"; say "Compilation: $source"
    UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=$(basename -- "$source"); step_progress "$STEP_ITEM"
    run "$CXX" "${VERBOSE_FLAG[@]}" "${CXXFLAGS[@]}" "$@" -MMD -MP -MF "$object.d" -c "$source" -o "$object"
    [[ -f $object ]] || fail "compiler returned success without object: $object"
}
link_cpp() {
    local output=$1; shift
    say "Linking: $output"
    UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=$(basename -- "$output"); step_progress "$STEP_ITEM"
    # -ldl : engine/core/digest.cpp charge un fournisseur OpenSSL par dlopen quand
    # AF_ALG est indisponible. -pthread : std::jthread des services réseau.
    run "$CXX" "${VERBOSE_FLAG[@]}" "${MODEFLAGS[@]}" "${SAN[@]}" -pthread "$@" "${EXTRA_LINK[@]}" -ldl -o "$output"
    [[ -x $output ]] || fail "linker returned success without executable: $output"
}
build_sdl() {
    local source object
    run mkdir -p "$DIR/vendor/sdl" "$DIR/vendor/imgui" "$DIR/objects/sdl"
    run tar -xzf "$SDL_ARCHIVE" -C "$DIR/vendor/sdl" --strip-components=1
    run tar -xzf "$IMGUI_ARCHIVE" -C "$DIR/vendor/imgui" --strip-components=1
    SDL_ROOT="$DIR/vendor/sdl"; IMGUI_ROOT="$DIR/vendor/imgui"; sdl_config
    local sources=("${SDL_SOURCES[@]}") libs=(-ldl -lm -pthread)
    if (( ! HEADLESS )); then sources+=("${SDL_X11_SOURCES[@]}"); libs+=(-lX11 -lXext); fi
    UNIT_INDEX=0; UNIT_TOTAL=$(( ${#sources[@]} + 1 ))
    SDL_OBJECTS=()
    for source in "${sources[@]}"; do
        object="$DIR/objects/sdl/${source//\//_}.o"
        say "Compilation SDL C: $source"
        UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=$(basename -- "$source"); step_progress "$STEP_ITEM"
        run "$CC" "${VERBOSE_FLAG[@]}" -std=c11 -D_GNU_SOURCE -fPIC -pthread -I "$DIR/config" -I "$SDL_ROOT/include" -I "$SDL_ROOT/src" "${MODEFLAGS[@]}" "${SAN[@]}" -MMD -MP -MF "$object.d" -c "$SDL_ROOT/$source" -o "$object"
        [[ -f $object ]] || fail "missing SDL object $object"
        SDL_OBJECTS+=("$object")
    done
    say 'Linking native SDL shared library (software renderer)'
    UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=libSDL3.so.0; step_progress "$STEP_ITEM"
    local strict=(); if [[ $MODE != asan ]]; then strict=(-Wl,--no-undefined); fi
    run "$CC" "${VERBOSE_FLAG[@]}" -shared "${SAN[@]}" -Wl,-soname,libSDL3.so.0 "${strict[@]}" "${SDL_OBJECTS[@]}" "${libs[@]}" -o "$BIN/libSDL3.so.0"
    [[ -f $BIN/libSDL3.so.0 ]] || fail 'SDL link did not create library'
}
build_project() {
    local tests=$1 source object started=$SECONDS
    # Clear first, including old executables, even when validation/compilation later fails.
    fresh "$DIR"; BIN="$DIR/bin"; run mkdir -p "$BIN" "$DIR/objects"
    step 'Contrôle de la chaîne de compilation'
    toolchain
    step_done "$TOOLCHAIN_SUMMARY"
    step 'Vérification des dépendances épinglées'
    dependencies; configure_flags
    if (( CORE_ONLY )); then step_skip 'non requis avec --core-only'; else step_done "SDL3 ${SDL_COMMIT:0:7} $G_DOT ImGui ${IMGUI_COMMIT:0:7} $G_DOT SHA-256 OK"; fi
    say "Compilation started: $MODE, clean deterministic objects (no cache)"
    COMMON_OBJECTS=(); MODEL_OBJECTS=(); NET_OBJECTS=(); GUI_OBJECTS=()
    BROWSER_OBJECTS=(); UI_OBJECTS=()
    step 'Compilation du moteur DevLink'
    UNIT_TOTAL=${#CORE_SOURCES[@]}
    if (( ! CORE_ONLY )) || (( tests )); then
        UNIT_TOTAL=$(( UNIT_TOTAL + ${#MODEL_SOURCES[@]} + ${#NET_SOURCES[@]} + ${#BROWSER_SOURCES[@]} ))
    fi
    for source in "${CORE_SOURCES[@]}"; do
        object="$DIR/objects/${source//\//_}.o"; compile_cpp "$source" "$object"; COMMON_OBJECTS+=("$object")
    done
    # Modèles et réseau compilés une seule fois : le poste de contrôle et les tests
    # en ont besoin, le moniteur CLI reste lié sans eux.
    if (( ! CORE_ONLY )) || (( tests )); then
        for source in "${MODEL_SOURCES[@]}" "${NET_SOURCES[@]}"; do
            object="$DIR/objects/${source//\//_}.o"; compile_cpp "$source" "$object"
            case $source in engine/net/*) NET_OBJECTS+=("$object");; *) MODEL_OBJECTS+=("$object");; esac
        done
    fi
    # Moteur navigateur : necessaire au poste de commande et aux tests unitaires
    # qui exercent la selection de backend et l'isolation des profils.
    if (( ! CORE_ONLY )) || (( tests )); then
        for source in "${BROWSER_SOURCES[@]}"; do
            object="$DIR/objects/${source//\//_}.o"; compile_cpp "$source" "$object"
            BROWSER_OBJECTS+=("$object")
        done
    fi
    step_done "$UNIT_TOTAL fichiers"
    step 'Compilation et liaison des outils en ligne de commande'
    UNIT_TOTAL=4
    compile_cpp app/host_shell/main.cpp "$DIR/objects/cli.o"
    link_cpp "$BIN/devlink-monitor" "${COMMON_OBJECTS[@]}" "$DIR/objects/cli.o"
    compile_cpp app/demo_service/main.cpp "$DIR/objects/demo.o"
    link_cpp "$BIN/devlink-demo-service" "${COMMON_OBJECTS[@]}" "$DIR/objects/demo.o"
    step_done "devlink-monitor $G_DOT devlink-demo-service"
    step 'Compilation de SDL3 depuis ses sources épinglées'
    if (( CORE_ONLY )); then
        step_skip 'non requis avec --core-only'
    else
        build_sdl
        if (( HEADLESS )); then step_done "${#SDL_OBJECTS[@]} fichiers C $G_DOT profil dummy"; else step_done "${#SDL_OBJECTS[@]} fichiers C $G_DOT X11 et rendu logiciel"; fi
    fi
    step 'Compilation et liaison du poste desktop'
    if (( CORE_ONLY )); then
        step_skip 'non requis avec --core-only'
    else
        UNIT_TOTAL=$(( ${#IMGUI_SOURCES[@]} + ${#UI_SOURCES[@]} + 2 ))
        for source in "${IMGUI_SOURCES[@]}"; do
            object="$DIR/objects/imgui_${source//\//_}.o"
            # Vendor warning policy differs from DevLink; errors remain visible.
            mkdir -p "$(dirname "$object")"; say "Compilation ImGui: $source"
            UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=$(basename -- "$source"); step_progress "$STEP_ITEM"
            run "$CXX" "${VERBOSE_FLAG[@]}" -std=c++20 "${MODEFLAGS[@]}" "${SAN[@]}" -I "$IMGUI_ROOT" -I "$IMGUI_ROOT/backends" -I "$SDL_ROOT/include" -MMD -MP -MF "$object.d" -c "$IMGUI_ROOT/$source" -o "$object"
            [[ -f $object ]] || fail "missing ImGui object $object"; GUI_OBJECTS+=("$object")
        done
        for source in "${UI_SOURCES[@]}"; do
            object="$DIR/objects/${source//\//_}.o"
            compile_cpp "$source" "$object" -isystem "$IMGUI_ROOT" -isystem "$IMGUI_ROOT/backends" -isystem "$SDL_ROOT/include"
            UI_OBJECTS+=("$object")
        done
        compile_cpp app/desktop_ui/main.cpp "$DIR/objects/desktop.o" -isystem "$IMGUI_ROOT" -isystem "$IMGUI_ROOT/backends" -isystem "$SDL_ROOT/include"
        link_cpp "$BIN/devlink-desktop" "${COMMON_OBJECTS[@]}" "${MODEL_OBJECTS[@]}" "${NET_OBJECTS[@]}" "${BROWSER_OBJECTS[@]}" "${GUI_OBJECTS[@]}" "${UI_OBJECTS[@]}" "$DIR/objects/desktop.o" "$BIN/libSDL3.so.0" '-Wl,-rpath,$ORIGIN'
        step_done "devlink-desktop $G_DOT $UNIT_TOTAL unités"
    fi
    if (( tests )); then
        step 'Compilation et liaison des suites de tests'
        UNIT_TOTAL=4
        compile_cpp tests/unit/test_main.cpp "$DIR/objects/unit.o"
        link_cpp "$BIN/devlink-unit-tests" "${COMMON_OBJECTS[@]}" "${MODEL_OBJECTS[@]}" "${NET_OBJECTS[@]}" "${BROWSER_OBJECTS[@]}" "$DIR/objects/unit.o"
        compile_cpp tests/integration/platform_tests.cpp "$DIR/objects/platform.o"
        link_cpp "$BIN/devlink-platform-tests" "${COMMON_OBJECTS[@]}" "${MODEL_OBJECTS[@]}" "${NET_OBJECTS[@]}" "${BROWSER_OBJECTS[@]}" "$DIR/objects/platform.o"
        step_done 'devlink-unit-tests · devlink-platform-tests'
        step "Contrôle d'indépendance des en-têtes"
        UNIT_TOTAL=${#HEADERS[@]}; (( CORE_ONLY )) || UNIT_TOTAL=$(( UNIT_TOTAL + ${#UI_HEADERS[@]} ))
        for source in "${HEADERS[@]}"; do
            printf '#include "%s"\n' "$source" > "$DIR/header-probe.cpp"
            say "Independent header: $source"
            UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=$(basename -- "$source"); step_progress "$STEP_ITEM"
            run "$CXX" "${VERBOSE_FLAG[@]}" "${CXXFLAGS[@]}" -fsyntax-only "$DIR/header-probe.cpp"
        done
        say "PASS ${#HEADERS[@]} independent native headers"; notice "PASS ${#HEADERS[@]} independent native headers"
        local ui_probed=0
        # Les en-tetes d'interface ne sont sondes que si ImGui est reellement
        # extrait : en profil core-only, le poste de commande n'est pas construit.
        if (( ! CORE_ONLY )); then
            for source in "${UI_HEADERS[@]}"; do
                printf '#include "%s"\n' "$source" > "$DIR/ui-header-probe.cpp"
                say "Independent UI header: $source"
                UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=$(basename -- "$source"); step_progress "$STEP_ITEM"
                run "$CXX" "${VERBOSE_FLAG[@]}" "${CXXFLAGS[@]}" -isystem "$IMGUI_ROOT" -isystem "$IMGUI_ROOT/backends" -isystem "$SDL_ROOT/include" -fsyntax-only "$DIR/ui-header-probe.cpp"
            done
            ui_probed=${#UI_HEADERS[@]}
            say "PASS ${#UI_HEADERS[@]} independent UI headers"; notice "PASS ${#UI_HEADERS[@]} independent UI headers"
        fi
        step_done "${#HEADERS[@]} moteur + $ui_probed interface"
    fi
    say "BUILD SUCCESS mode=$MODE elapsed_seconds=$((SECONDS-started)) (compilation including validation/extraction; tests not included)"
    notice "BUILD SUCCESS mode=$MODE elapsed_seconds=$((SECONDS-started)) headless=$HEADLESS core_only=$CORE_ONLY"
    printf 'mode=%s\ncompiler=%s\nheadless=%s\n' "$MODE" "$CXX" "$HEADLESS" > "$DIR/BUILD_SUCCEEDED"
}
PASS=0; SKIP=0
suite_detail() {
    # Bilan du harnais : "suite: N cases, M checks, 0 failures (...)".
    local line; line=$(grep -Em1 '^[a-z0-9-]+: [0-9]+ cases, [0-9]+ checks' "$1" || true)
    if [[ -z $line ]]; then printf 'code %s attendu' "$2"; return 0; fi
    local _name cases _w checks _rest; read -r _name cases _w checks _rest <<< "${line//,/}"
    printf '%s cas %s %s vérifications %s 0 échec' "$cases" "$G_DOT" "$checks" "$G_DOT"
}
test_excerpt() {
    grep -E 'FAIL|EXCEPTION|error:|EVIDENCE|SKIP|cases,|Usage:|Sanitizer' "$1" > "$1.excerpt" || true
    print_excerpt "$1.excerpt" 30 "$1"
}
expect() {
    local name=$1 expected=$2 pattern=$3; shift 3
    local capture="$DIR/$name.log" rc=0
    UNIT_INDEX=$((UNIT_INDEX+1)); STEP_ITEM=$name; step_progress "$name"
    printf 'Test started: %s (expected exit=%s)\n+ ' "$name" "$expected" >> "$LOG"; command_text "$@" >> "$LOG"; printf '\n' >> "$LOG"
    "$@" > "$capture" 2>&1 || rc=$?
    cat -- "$capture" >> "$LOG"
    if (( VERBOSE )); then cat -- "$capture" >&3; fi
    if [[ $name == ipv6 && $rc == 77 ]]; then
        SKIP=$((SKIP+1)); say 'SKIP ipv6: unavailable, NOT counted as pass'; notice 'SKIP ipv6 (qualification incomplete)'
        ROWS+=("skip|ipv6|IPv6 indisponible : SKIP code 77, jamais compté réussi"); return 0
    fi
    if [[ $rc != "$expected" ]] || ! grep -Eq -- "$pattern" "$capture"; then
        say "FAIL test=$name expected=$expected actual=$rc command=$(command_text "$@")"
        step_fail
        printf '    %s%s Test %s : code %s, attendu %s%s\n' "$C_ERR" "$G_FAIL" "$name" "$rc" "$expected" "$C0" >&3
        test_excerpt "$capture"
        local details; details=$(head -24 "$capture"; printf '\n--- tail ---\n'; tail -8 "$capture")
        notice "failed test $name: ${details//$'\n'/%0A}"
        if (( rc != 0 )); then return "$rc"; else return 1; fi
    fi
    if grep -Eq 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$capture"; then test_excerpt "$capture"; fail "sanitizer diagnostic in $name (even if exit matched)"; fi
    PASS=$((PASS+1)); say "PASS $name: exit=$rc"
    printf 'PASS %s exit=%s\n' "$name" "$rc" >> "$DIR/test-summary.txt"
    grep -E 'desktop-runtime:|cases,.*checks' "$capture" >> "$DIR/test-summary.txt" || true
    case $name in
        cli-*) CLI_PASS=$((CLI_PASS+1));;
        ui-*) UI_PASS=$((UI_PASS+1));;
        *) ROWS+=("ok|$name|$(suite_detail "$capture" "$rc")");;
    esac
}
tests() {
    step 'Exécution des tests'
    PASS=0; SKIP=0; CLI_PASS=0; UI_PASS=0; ROWS=()
    # 4 suites + 5 controles CLI ; + 7 controles UI (+1 fenetre X11 native).
    UNIT_TOTAL=9; (( CORE_ONLY )) || UNIT_TOTAL=16; (( CORE_ONLY || ! NATIVE_UI )) || UNIT_TOTAL=17
    printf 'Native Linux tests mode=%s\n' "$MODE" > "$DIR/test-summary.txt"
    expect unit 0 '0 failures' "$BIN/devlink-unit-tests"
    expect platform 0 '0 failures' "$BIN/devlink-platform-tests"
    expect ipv6 0 '0 failures' "$BIN/devlink-platform-tests" --ipv6
    expect demo-service 0 'SAME_HOST_INTEGRATION PASS' python3 tests/integration/demo_service_test.py "$BIN/devlink-demo-service"
    expect cli-help 0 'no probes, no server, no sharing' "$BIN/devlink-monitor" --help
    expect cli-version 0 'no sharing' "$BIN/devlink-monitor" --version
    expect cli-unknown 2 'Usage:' "$BIN/devlink-monitor" --not-a-devlink-option
    expect cli-extra 2 'Usage:' "$BIN/devlink-monitor" --version extra
    expect cli-ambiguous 2 'Usage:' "$BIN/devlink-monitor" --help --version
    if (( ! CORE_ONLY )); then
        expect ui-smoke 0 'desktop-smoke:' env SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$BIN/devlink-desktop" --smoke-test --screenshot "$DIR/desktop.bmp"
        expect ui-unknown 2 'Usage:' "$BIN/devlink-desktop" --not-a-devlink-option
        expect ui-missing-argument 2 'Usage:' "$BIN/devlink-desktop" --smoke-test --screenshot
        expect ui-capture-requires-smoke 2 --smoke-test "$BIN/devlink-desktop" --screenshot never-created.bmp
        expect ui-driver-error 1 SDL_Init: env SDL_VIDEODRIVER=devlink-invalid-video-driver "$BIN/devlink-desktop" --smoke-test
        expect ui-file-error 1 SDL_IOFromFile: env SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$BIN/devlink-desktop" --smoke-test --screenshot "$BIN/devlink-desktop/not-a-directory.bmp"
        expect ui-disk-full 1 'SDL_(SaveBMP_IO|CloseIO):' env SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$BIN/devlink-desktop" --smoke-test --screenshot /dev/full
        UI_NOTE='pilote dummy, fenêtre native avec --native-ui'
        if (( NATIVE_UI )); then
            (( ! HEADLESS )) || fail '--native-ui cannot qualify a --headless build'
            expect ui-native-x11 0 'video=x11 renderer=software' env SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software "$BIN/devlink-desktop" --smoke-test --screenshot "$DIR/desktop-native.bmp"
            UI_NOTE='fenêtre X11 native incluse'
        else say 'NOT RUN: native X11 window launch (use --native-ui with DISPLAY; dummy is not native display validation)'; fi
    fi
    say "TEST SUMMARY passed=$PASS skipped=$SKIP mode=$MODE"
    printf 'TEST SUMMARY passed=%s skipped=%s mode=%s\n' "$PASS" "$SKIP" "$MODE" >> "$DIR/test-summary.txt"
    local summary_text; summary_text=$(cat "$DIR/test-summary.txt"); notice "${summary_text//$'\n'/%0A}"
    local detail="$PASS réussis"; (( SKIP == 0 )) || detail+=" $G_DOT $SKIP ignoré(s)"
    step_done "$detail"
    local entry kind name text
    for entry in "${ROWS[@]+"${ROWS[@]}"}"; do IFS='|' read -r kind name text <<< "$entry"; row "$kind" "$name" "$text"; done
    row ok cli "$CLI_PASS contrôles de la ligne de commande du moniteur"
    (( CORE_ONLY )) || row ok ui "$UI_PASS contrôles du poste desktop $G_DOT $UI_NOTE"
    (( SKIP == 0 )) || return 77
}
fuzz() {
    MODE=asan; CORE_ONLY=1; DIR="$OUT/tests/fuzz"; fresh "$DIR"; BIN="$DIR/bin"; mkdir -p "$BIN" "$DIR/objects" "$DIR/corpus"
    step 'Contrôle de la chaîne de compilation'
    toolchain; configure_flags
    step_done "$TOOLCHAIN_SUMMARY"
    step 'Compilation de la cible de fuzzing'
    UNIT_TOTAL=2
    compile_cpp engine/protocol/envelope.cpp "$DIR/objects/envelope.o" -fsanitize=fuzzer-no-link
    compile_cpp tests/fuzz/envelope_fuzz.cpp "$DIR/objects/fuzz.o" -fsanitize=fuzzer-no-link
    step_done '2 fichiers'
    step 'Édition des liens du fuzzer'
    UNIT_TOTAL=1
    link_cpp "$BIN/devlink-fuzz-envelope" -fsanitize=fuzzer "$DIR/objects/envelope.o" "$DIR/objects/fuzz.o"
    step_done "libFuzzer $G_DOT ASan $G_DOT UBSan"
    step "Fuzzing de l'enveloppe : 20 000 exécutions"
    run cp tests/fuzz/corpus/hello tests/fuzz/corpus/chat "$DIR/corpus/"
    run "$BIN/devlink-fuzz-envelope" -runs=20000 -seed=20261113 -max_len=4096 -timeout=2 "-artifact_prefix=$DIR/" "$DIR/corpus"
    notice 'PASS native libFuzzer + ASan/UBSan: 20000 executions'
    step_done "aucun plantage $G_DOT graine 20261113"
}
# Publication d'une archive vers releases/ (mandat §8, etape 8) :
# - ecriture vers <archive>.partial, JAMAIS directement au nom final ;
# - integrite lue de bout en bout AVANT publication (listing de l'archive) ;
# - SHA-256 calculee puis publiee dans <archive>.sha256 (verifiable par
#   « cd releases && sha256sum -c <archive>.sha256 ») ;
# - renommage atomique : un lecteur ne voit jamais une archive tronquee ;
# - une archive deja publiee dont l'empreinte differe n'est JAMAIS ecrasee
#   silencieusement : echec explicite avec les deux empreintes ;
# - la cible doit se trouver SOUS releases/, refuse ailleurs.
publish_release() {
    local source="$1" target="$2"
    [[ -f $source && ! -L $source ]] || fail "publish_release: archive source absente : $source"
    [[ $target == "$ROOT"/releases/* ]] || fail "publish_release: cible hors de releases/ : $target"
    mkdir -p -- "$(dirname -- "$target")"
    local partial="$target.partial" sidecar="$target.sha256" sidecar_partial="$target.sha256.partial"
    rm -f -- "$partial" "$sidecar_partial"
    if ! run cp -- "$source" "$partial"; then rm -f -- "$partial"; fail "publish_release: copie impossible vers $partial"; fi
    case "$target" in
        *.tar.gz) if ! run tar -tzf "$partial" > /dev/null; then rm -f -- "$partial"; fail "publish_release: archive illisible (tar) : $(basename -- "$target")"; fi;;
        *.zip)    if ! run unzip -tq "$partial" > /dev/null; then rm -f -- "$partial"; fail "publish_release: archive illisible (zip) : $(basename -- "$target")"; fi;;
        *) rm -f -- "$partial"; fail "publish_release: format d'archive non reconnu : $target";;
    esac
    local hash; hash=$(sha256sum "$partial"); hash=${hash%% *}
    printf '%s  %s\n' "$hash" "$(basename -- "$target")" > "$sidecar_partial"
    if [[ -e $target ]]; then
        local existing; existing=$(sha256sum "$target"); existing=${existing%% *}
        if [[ $existing != "$hash" ]]; then
            rm -f -- "$partial" "$sidecar_partial"
            fail "publish_release: $(basename -- "$target") existe deja avec une empreinte differente — aucun ecrasement silencieux (existant=$existing nouveau=$hash)"
        fi
        rm -f -- "$partial"
        run mv -- "$sidecar_partial" "$sidecar"
        say "RELEASE INCHANGEE : $target (empreinte identique, deja publiee)"
        return 0
    fi
    run mv -- "$partial" "$target"        # renommage atomique, meme systeme de fichiers
    run mv -- "$sidecar_partial" "$sidecar"
    if ! (cd -- "$(dirname -- "$target")" && run sha256sum -c -- "$(basename -- "$sidecar")"); then
        fail "publish_release: verification de l'empreinte publiee en echec : $target"
    fi
    [[ ! -e $partial && ! -e $sidecar_partial ]] || fail "publish_release: fichiers partiels restants apres publication"
    say "RELEASE PUBLIEE : $target"
    notice "PACKAGE SHA256 $hash $(basename -- "$target")"
}

package() {
    (( ! HEADLESS && ! CORE_ONLY )) || fail 'package requires the full native desktop, not a headless/core-only profile'
    fresh "$OUT/packages"
    MODE=release; DIR="$OUT/release"; build_project 1; tests
    local version stage archive
    step 'Assemblage du paquet'
    version=$(product_version)
    stage="$OUT/packages/stage"; archive="$OUT/packages/DevLink-$version-linux-x64.tar.gz"
    fresh "$stage"; mkdir -p "$stage/DevLink/bin" "$stage/DevLink/docs/reports" "$stage/DevLink/licenses"
    run cp packaging/README.md "$stage/DevLink/README.md"
    run cp docs/reports/BUILD_DEPENDENCIES.md "$stage/DevLink/docs/reports/"
    run cp "$BIN/devlink-monitor" "$BIN/devlink-desktop" "$BIN/devlink-demo-service" "$BIN/libSDL3.so.0" "$stage/DevLink/bin/"
    run cp docs/BUILD.md docs/SECURITY.md docs/QUICKSTART.md "$stage/DevLink/docs/"
    run cp third_party/DEPENDENCIES.md "$stage/DevLink/docs/"
    run cp third_party/licenses/SDL3.txt third_party/licenses/ImGui.txt third_party/licenses/Bundled-notices.txt "$stage/DevLink/licenses/"
    run tar -czf "$archive" -C "$stage" DevLink
    step_done "$(basename -- "$archive") $G_DOT $(( $(stat -c %s "$archive") / 1024 )) Kio"
    step 'Publication atomique dans releases'
    # Etape 8 (mandat) : publication ATOMIQUE vers releases/ — ecriture .partial,
    # integrite et SHA-256 verifiees AVANT le renommage, jamais d'ecrasement muet.
    local published="$ROOT/releases/DevLink-$version-linux-x64.tar.gz"
    publish_release "$archive" "$published"
    step_done "SHA-256 $(cut -c1-16 "$published.sha256")"
    step "Vérification de l'archive publiée"
    local check="$OUT/packages/check"; fresh "$check"
    # L'extraction de preuve porte sur l'archive PUBLIEE : c'est celle-la qu'un
    # tiers telecharge, pas l'etape intermediaire sous out/.
    run tar -xzf "$published" -C "$check"
    run cmp "$BIN/devlink-monitor" "$check/DevLink/bin/devlink-monitor"
    run cmp "$BIN/devlink-desktop" "$check/DevLink/bin/devlink-desktop"
    run cmp "$BIN/libSDL3.so.0" "$check/DevLink/bin/libSDL3.so.0"
    run cmp packaging/README.md "$check/DevLink/README.md"
    run cmp docs/reports/BUILD_DEPENDENCIES.md "$check/DevLink/docs/reports/BUILD_DEPENDENCIES.md"
    local license
    for license in SDL3.txt ImGui.txt Bundled-notices.txt; do run cmp "third_party/licenses/$license" "$check/DevLink/licenses/$license"; done
    run "$check/DevLink/bin/devlink-monitor" --version
    run env SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software "$check/DevLink/bin/devlink-desktop" --smoke-test
    run ldd "$check/DevLink/bin/devlink-desktop" > "$OUT/packages/runtime-dependencies.txt"
    cat "$OUT/packages/runtime-dependencies.txt" >> "$LOG"
    local runtime_info; runtime_info=$(cat "$OUT/packages/runtime-dependencies.txt")
    notice "runtime dependencies: ${runtime_info//$'\n'/%0A}"
    say "PACKAGE SUCCESS $published (empreinte publiee : $published.sha256 ; development archive, host glibc/libstdc++/X11 required)"; notice 'PASS native Release package extracted and executed'
    step_done "octets identiques $G_DOT exécution $G_DOT ldd consigné"
    SUMMARY_ARTIFACT=$published; SUMMARY_ARTIFACT_LABEL='Archive    '
    note_later 'Archive de développement : glibc, libstdc++ et X11 de l'"'"'hôte requis.'
}
selftest() {
    # Exercise this very script as a child, after releasing our lock. No helper build engine.
    rmdir "$OUT/.build-lock"; LOCK=0
    local rc=0
    export DEVLINK_PLAIN=1
    step "Refus d'un compilateur absent"
    DEVLINK_EXPECT_FAILURE=1 DEVLINK_CXX=/does/not/exist "$ROOT/build.sh" doctor --core-only > "$OUT/logs/selftest-missing-compiler.log" 2>&1 || rc=$?
    (( rc != 0 )) || fail 'missing compiler falsely succeeded'
    step_done "refusé, code $rc"
    step 'Échec de compilation propagé, sans reliquat'
    rc=0
    DEVLINK_EXPECT_FAILURE=1 DEVLINK_EXTRA_CXXFLAGS=-fdevlink-deliberate-invalid-option "$ROOT/build.sh" debug --core-only > "$OUT/logs/selftest-compile-failure.log" 2>&1 || rc=$?
    (( rc != 0 )) && [[ ! -e $OUT/debug/BUILD_SUCCEEDED && ! -e $OUT/debug/bin/devlink-monitor ]] || fail 'failed compile left a stale successful build'
    step_done "code $rc $G_DOT aucune sortie réussie"
    step 'Échec de liaison propagé, sans reliquat'
    rc=0
    DEVLINK_EXPECT_FAILURE=1 DEVLINK_EXTRA_LDFLAGS=-Wl,--devlink-deliberate-invalid-option "$ROOT/build.sh" debug --core-only > "$OUT/logs/selftest-link-failure.log" 2>&1 || rc=$?
    (( rc != 0 )) && [[ ! -e $OUT/debug/BUILD_SUCCEEDED && ! -e $OUT/debug/bin/devlink-monitor ]] || fail 'failed link left a stale successful build'
    step_done "code $rc $G_DOT aucune sortie réussie"
    step "Refus d'une archive corrompue et code de sortie 23"
    # Exercise the same hash verifier without ever modifying the verified originals.
    DIR="$OUT/tests/negative"; fresh "$DIR"
    printf 'corrupt archive for negative test\n' > "$DIR/corrupt.tar.gz"
    rc=0
    (DEVLINK_QUIET_PROBLEM=1; verify "$DIR/corrupt.tar.gz" "$IMGUI_SHA") > "$OUT/logs/selftest-hash-failure.log" 2>&1 || rc=$?
    (( rc != 0 )) && grep -q 'SHA-256 mismatch' "$OUT/logs/selftest-hash-failure.log" || fail 'bad archive was not rejected'
    rc=0
    "$OUT/doctor/probe" --fail > "$DIR/intentional-exit.log" 2>&1 || rc=$?
    (( rc == 23 )) || fail "probe exit code was not preserved (expected 23, got $rc)"
    grep -q 'intentional test failure: exit 23' "$DIR/intentional-exit.log" || fail 'intentional failure probe output was not preserved'
    say 'PASS intentional-exit-propagation: exit=23 preserved by the probe'
    step_done "empreinte refusée $G_DOT code 23 conservé"
    step 'Ressources ICO et PE sur fixtures'
    UNIT_TOTAL=8
    # L'icône de marque est un vrai ICO multi-tailles : contrôle positif…
    expect brand-ico 0 'all required Windows sizes present' "$OUT/doctor/probe" --ico "$ROOT/assets/branding/devlink.ico" --expect-entries 7
    # …et négatif : un fichier renommé .ico doit être rejeté, jamais accepté.
    printf 'ceci n est pas une icone\n' > "$DIR/fake.ico"
    expect renamed-ico 1 'ICO validation failed' "$OUT/doctor/probe" --ico "$DIR/fake.ico" --expect-entries 7
    # Les lecteurs PE sont testes sur des fixtures synthetiques cote Linux : le
    # parseur x86-64 et ses controles negatifs sont exercables sans linker Windows.
    command -v python3 >/dev/null || fail 'selftest PE: python3 est requis pour fabriquer les fixtures deterministes'
    run python3 "$ROOT/tests/build/synthetic_pe.py" --out-dir "$DIR/synthetic-pe"
    expect pe-icons-synthetic 0 'PASS RT_GROUP_ICON' "$OUT/doctor/probe" --pe-icons "$DIR/synthetic-pe/icons-good.exe" --expect-icons 7
    expect pe-manifest-synthetic 0 'PASS resource #1 version=0.2.0.0' "$OUT/doctor/probe" --manifest "$DIR/synthetic-pe/icons-good.exe" --expect-version 0.2.0.0
    expect pe-manifest-only-synthetic 0 'PASS resource #1 version=0.2.0.0' "$OUT/doctor/probe" --manifest "$DIR/synthetic-pe/manifest-only.exe" --expect-version 0.2.0.0
    expect pe-icons-bad-group 1 'PE icon validation failed' "$OUT/doctor/probe" --pe-icons "$DIR/synthetic-pe/icons-bad-group.exe" --expect-icons 7
    expect pe-manifest-missing 1 'PE manifest validation failed' "$OUT/doctor/probe" --manifest "$DIR/synthetic-pe/no-manifest.exe" --expect-version 0.2.0.0
    expect pe-manifest-wrong-version 1 'PE manifest validation failed' "$OUT/doctor/probe" --manifest "$DIR/synthetic-pe/wrong-version.exe" --expect-version 0.2.0.0
    step_done '8 contrôles positifs et négatifs'
    step 'Refus de session sans runtime TLS'
    UNIT_TOTAL=1
    # Transport TLS : bibliothèque volontairement introuvable => la fabrique doit
    # le dire (Unsupported) et les cas TLS se déclarer SKIP, jamais PASS cachés.
    local tls_platform_bin="$OUT/tests/${MODE:-debug}/bin/devlink-platform-tests"
    [[ -x $tls_platform_bin ]] || fail "selftest TLS: $tls_platform_bin absent; execute './build.sh test ${MODE:-debug}' first"
    expect tls-unavailable-proof 0 'tls-unavailable-proof' env DEVLINK_TLS_LIBRARY_PATH=/does/not/exist/libssl.so "$tls_platform_bin"
    # Le même enregistrement doit aussi prouver le REFUS de démarrer un partage
    # reel sans runtime TLS : aucune degradation silencieuse en clair (ADR-016).
    grep -Eq '\[PASS\] host_refuses_session_without_tls_runtime' "$DIR/tls-unavailable-proof.log" \
        || fail 'selftest TLS: refus de session sans runtime non prouve'
    grep -Eq 'host-refusal-proof: ' "$DIR/tls-unavailable-proof.log" \
        || fail 'selftest TLS: raison du refus absente de la sortie'
    grep -Eq 'host-cleared-proof: .*clair' "$DIR/tls-unavailable-proof.log" \
        || fail 'selftest TLS: chemin clair assume non etiquette'
    say 'PASS tls-host-refusal-proof'
    step_done 'refus prouvé, aucun repli en clair'
    step 'Publication atomique et clean préservant releases'
    notice 'PASS tls-host-refusal-proof'
    # Publication releases/ (mandat §8, etape 8) : ecriture .partial puis
    # renommage atomique, SHA-256 publiee et revérifiable, aucun ecrasement
    # silencieux, aucune archive illisible acceptee. Preuve sur une archive
    # synthetique : la mecanique est la MEME fonction que le package reel.
    local rel="$ROOT/releases"
    fresh "$DIR/release-proof"; printf 'archive de preuve DevLink\n' > "$DIR/release-proof/payload.txt"
    run tar -czf "$DIR/release-proof/fixture.tar.gz" -C "$DIR/release-proof" payload.txt
    publish_release "$DIR/release-proof/fixture.tar.gz" "$rel/DevLink-selftest-fixture.tar.gz"
    [[ -f $rel/DevLink-selftest-fixture.tar.gz ]] || fail 'release proof: archive publiee absente'
    [[ ! -e $rel/DevLink-selftest-fixture.tar.gz.partial ]] || fail 'release proof: fichier .partial reste apres publication'
    [[ -f $rel/DevLink-selftest-fixture.tar.gz.sha256 ]] || fail 'release proof: empreinte SHA-256 non publiee'
    (cd -- "$rel" && run sha256sum -c -- DevLink-selftest-fixture.tar.gz.sha256) || fail 'release proof: empreinte publiee invérifiable'
    # Negatif : un contenu DIFFERENT sous le meme nom est refuse et l'original
    # reste intact (controle : son empreinte publiee verifie toujours).
    printf 'archive de preuve modifiee\n' > "$DIR/release-proof/payload.txt"
    run tar -czf "$DIR/release-proof/changed.tar.gz" -C "$DIR/release-proof" payload.txt
    rc=0; ( DEVLINK_EXPECT_FAILURE=1; DEVLINK_QUIET_PROBLEM=1; publish_release "$DIR/release-proof/changed.tar.gz" "$rel/DevLink-selftest-fixture.tar.gz" ) || rc=$?
    (( rc != 0 )) || fail 'release proof: ecrasement silencieux accepte (pire defaut possible)'
    (cd -- "$rel" && run sha256sum -c -- DevLink-selftest-fixture.tar.gz.sha256) || fail 'release proof: original altere par une tentative refusee'
    # Negatif : une archive illisible n'entre JAMAIS dans releases/, sans residu.
    printf 'ceci n est pas une archive\n' > "$DIR/release-proof/broken.tar.gz"
    rc=0; ( DEVLINK_EXPECT_FAILURE=1; DEVLINK_QUIET_PROBLEM=1; publish_release "$DIR/release-proof/broken.tar.gz" "$rel/DevLink-selftest-broken.tar.gz" ) || rc=$?
    (( rc != 0 )) || fail 'release proof: archive illisible publiee'
    [[ ! -e $rel/DevLink-selftest-broken.tar.gz && ! -e $rel/DevLink-selftest-broken.tar.gz.sha256 ]] \
        || fail 'release proof: residus de publication refusee dans releases/'
    [[ -z $(find "$rel" -name '*.partial' -print -quit) ]] || fail 'release proof: fichier .partial abandonne dans releases/'
    say 'PASS release-publish-proof'
    notice 'PASS release-publish-proof'
    # clean ne touche JAMAIS releases/ : preuve NON VIDE — quelque chose est bien
    # nettoye sous out/ (sinon la preservation serait demontree par rien), puis
    # les artefacts publies survivent au nettoyage.
    [[ -e $OUT/tests/negative ]] || fail 'clean proof: fixture de nettoyage absante, preuve impossible'
    DEVLINK_EXPECT_FAILURE=0 "$ROOT/build.sh" clean > "$OUT/logs/selftest-clean.log" 2>&1 || fail 'clean proof: ./build.sh clean a echoue'
    [[ ! -e $OUT/tests/negative ]] || fail 'clean proof: clean na rien nettoye — preuve de preservation vide'
    [[ -f $rel/DevLink-selftest-fixture.tar.gz && -f $rel/DevLink-selftest-fixture.tar.gz.sha256 ]] \
        || fail 'clean proof: releases/ touche par clean (artefacts detruits)'
    rm -f -- "$rel"/DevLink-selftest-fixture.tar.gz "$rel"/DevLink-selftest-fixture.tar.gz.sha256
    say 'PASS clean-preserves-releases-proof'
    notice 'PASS clean-preserves-releases-proof'
    say 'SELFTEST PASS: toolchain, compile/link, ICO/PE resources, archive hash and release publishing checks; clean never touches releases/; no stale successful output'
    step_done 'sans écrasement ni résidu ; releases/ intact'
    notice 'SELFTEST PASS: compiler/compile/link/hash/release-publish failure propagation'
}
case "$ACTION" in
    deps)
        TOTAL=2
        step 'Récupération des archives épinglées'
        plain_directory "$DEPS"; mkdir -p "$DEPS"
        if (( FETCH )); then
            fetch_one "https://codeload.github.com/libsdl-org/SDL/tar.gz/$SDL_COMMIT" "$SDL_ARCHIVE" "$SDL_SHA"
            fetch_one "https://codeload.github.com/ocornut/imgui/tar.gz/$IMGUI_COMMIT" "$IMGUI_ARCHIVE" "$IMGUI_SHA"
            step_done 'téléchargement HTTPS autorisé par --fetch'
        else step_done 'archives locales'; fi
        step 'Vérification des dépendances épinglées'
        dependencies
        if (( CORE_ONLY )); then step_skip 'non requis avec --core-only'; else step_done "SDL3 ${SDL_COMMIT:0:7} $G_DOT ImGui ${IMGUI_COMMIT:0:7} $G_DOT SHA-256 OK"; fi;;
    doctor)
        TOTAL=2
        step 'Contrôle de la chaîne de compilation'
        toolchain
        step_done "$TOOLCHAIN_SUMMARY"
        row note 'système' "$(uname -sr) $G_DOT $(getconf GNU_LIBC_VERSION 2>/dev/null || echo 'glibc inconnue')"
        step 'Vérification des dépendances épinglées'
        dependencies
        if (( CORE_ONLY )); then step_skip 'non requis avec --core-only'; else step_done "SDL3 ${SDL_COMMIT:0:7} $G_DOT ImGui ${IMGUI_COMMIT:0:7} $G_DOT SHA-256 OK"; fi;;
    debug|release) MODE=$ACTION; DIR="$OUT/$MODE"; TOTAL=6; build_project 0; SUMMARY_ARTIFACT=$BIN;;
    test) DIR="$OUT/tests/$MODE"; TOTAL=9; build_project 1; SUMMARY_ARTIFACT=$BIN; tests;;
    asan) MODE=asan; DIR="$OUT/tests/asan"; TOTAL=9; build_project 1; SUMMARY_ARTIFACT=$BIN; tests;;
    fuzz) TOTAL=4; fuzz;;
    package) TOTAL=12; package;;
    selftest) TOTAL=7; selftest;;
    clean)
        TOTAL=1
        step 'Suppression des sorties compilées'
        for name in debug release tests packages doctor; do plain_directory "$OUT/$name"; if [[ -e $OUT/$name ]]; then run rm -rf -- "$OUT/$name"; fi; done
        say 'CLEAN SUCCESS: compiled outputs removed; releases/ untouched; logs and dependencies retained'
        step_done 'releases/, journaux et dépendances conservés';;
esac
say "SUCCESS operation=$ACTION log=$LOG"
