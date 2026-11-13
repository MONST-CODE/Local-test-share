#!/usr/bin/env bash
# PLPT's authoritative Linux build. No generated project, build engine or helper runtime.
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
cd -- "$ROOT"
ACTION=${1:-debug}; if (( $# )); then shift; fi
MODE=debug; VERBOSE=0; HEADLESS=0; CORE_ONLY=0; FETCH=0; NATIVE_UI=0
if [[ $ACTION == test && ${1:-} =~ ^(debug|release|asan)$ ]]; then MODE=$1; shift; fi
for arg in "$@"; do
    case "$arg" in
        --verbose) VERBOSE=1;;
        --headless) HEADLESS=1;;
        --core-only) CORE_ONLY=1;;
        --fetch) FETCH=1;;
        --native-ui) NATIVE_UI=1;;
        *) printf 'ERROR: unknown argument: %s\n' "$arg" >&2; exit 2;;
    esac
done
case "$ACTION" in debug|release|test|asan|fuzz|clean|package|doctor|deps|selftest|help) ;; *) echo "ERROR: unknown operation: $ACTION" >&2; exit 2;; esac
if [[ $ACTION == help ]]; then
    echo 'Usage: ./build.sh debug|release|test [debug|release|asan]|asan|fuzz|clean|package|doctor|deps|selftest [--verbose] [--headless] [--core-only] [--native-ui]'
    echo 'deps --fetch explicitly downloads only pinned archives. Other operations never download.'
    echo 'Default desktop: X11/software. --headless explicitly selects dummy-only SDL (not a desktop release).'
    exit 0
fi
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'ERROR: native Linux x86-64 required'; exit 2; }
[[ -f engine/core/types.hpp && ! -L out && ! -L out/linux ]] || { echo 'ERROR: unsafe output root or wrong checkout'; exit 2; }
OUT="$ROOT/out/linux"; DEPS="$OUT/dependencies"
mkdir -p "$OUT/logs"
LOG="$OUT/logs/$(date -u +%Y%m%dT%H%M%SZ)-${ACTION}-${MODE}-$$.log"
: > "$LOG"
LOCK=0
cleanup() { local rc=$?; if (( LOCK )); then rmdir "$OUT/.build-lock" 2>/dev/null || true; fi; exit "$rc"; }
trap cleanup EXIT
say() { printf '%s\n' "$*" | tee -a "$LOG"; }
notice() { if [[ ${GITHUB_ACTIONS:-} == true ]]; then printf '::notice title=PLPT Linux::%s\n' "$*"; fi; }
fail() { say "FAILURE: $*"; exit 1; }
command_text() { printf '%q ' "$@"; }
# Preserve BOTH compiler/test status and logging failures; never let tee mask an error.
run() {
    local text statuses
    text=$(command_text "$@"); printf '+ %s\n' "$text" >> "$LOG"
    if (( VERBOSE )); then printf '+ %s\n' "$text"; fi
    if "$@" 2>&1 | tee -a "$LOG"; then
        return 0
    else
        statuses=("${PIPESTATUS[@]}")
    fi
    if (( statuses[1] != 0 )); then echo 'ERROR: cannot preserve command log' >&2; return "${statuses[1]}"; fi
    if (( statuses[0] != 0 )); then say "Command exit ${statuses[0]}: $text"; fi
    return "${statuses[0]}"
}
error() {
    local rc=$?; say "FAILURE exit=$rc operation=$ACTION (see $LOG)"
    if [[ ${GITHUB_ACTIONS:-} == true && ${PLPT_EXPECT_FAILURE:-} != 1 ]]; then
        echo "::error title=PLPT Linux build::Operation $ACTION failed, exit $rc. See preserved log."
        local details
        details=$(grep -E 'error:|undefined reference|Command exit|FAILURE|FAIL ' "$LOG" | tail -4 || true)
        notice "failure context: ${details//$'\n'/%0A}"
    fi
    exit "$rc"
}
trap error ERR
mkdir "$OUT/.build-lock" 2>/dev/null || fail "another build owns $OUT/.build-lock; remove only after verifying no build is running"
LOCK=1
say "PLPT direct Linux operation=$ACTION mode=$MODE; log=$LOG"
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
    say "SHA256 OK $got $1"; notice "dependency SHA256 $got $(basename "$1")"
}
dependencies() {
    (( CORE_ONLY )) && return 0
    verify "$SDL_ARCHIVE" "$SDL_SHA"; verify "$IMGUI_ARCHIVE" "$IMGUI_SHA"
}
fetch_one() {
    local url=$1 file=$2 hash=$3
    if [[ -f $file ]]; then verify "$file" "$hash"; return; fi
    run curl --fail --location --show-error --proto '=https' --tlsv1.2 --output "$file.part" "$url"
    verify "$file.part" "$hash"; run mv -- "$file.part" "$file"
}
CXX=; CC=
toolchain() {
    CXX=$(command -v -- "${PLPT_CXX:-clang++}") || fail 'clang++ missing; install LLVM or set PLPT_CXX to its executable'
    CC=$(command -v -- "${PLPT_CC:-$(dirname "$CXX")/clang}") || fail 'matching clang C compiler missing (SDL C sources)'
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
    run "$OUT/doctor/probe"
}
# Explicit PLPT targets; models are linked ONLY to unit tests, never the monitor/UI.
CORE_SOURCES=(engine/core/random.cpp engine/diagnostics/audit.cpp
    engine/discovery/process_tree.cpp engine/discovery/terminal_classifier.cpp
    engine/discovery/monitor.cpp engine/service_registry/registry.cpp
    engine/port_allocator/port_allocator.cpp engine/platform/linux/linux_platform.cpp)
MODEL_SOURCES=(engine/session/session.cpp engine/messaging/messaging.cpp
    engine/protocol/envelope.cpp engine/resource_manager/resource.cpp)
IMGUI_SOURCES=(imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp
    backends/imgui_impl_sdl3.cpp backends/imgui_impl_sdlrenderer3.cpp)
HEADERS=(
    engine/browser/browser.hpp
    engine/core/clock.hpp
    engine/core/random.hpp
    engine/core/result.hpp
    engine/core/text.hpp
    engine/core/types.hpp
    engine/diagnostics/audit.hpp
    engine/discovery/monitor.hpp
    engine/discovery/process_tree.hpp
    engine/discovery/terminal_classifier.hpp
    engine/messaging/messaging.hpp
    engine/platform/native.hpp
    engine/platform/platform.hpp
    engine/port_allocator/port_allocator.hpp
    engine/protocol/envelope.hpp
    engine/resource_manager/resource.hpp
    engine/service_registry/registry.hpp
    engine/session/session.hpp
    engine/transport/transport.hpp
    engine/platform/linux/linux_platform.hpp)
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
    src/video/x11/edid-parse.c src/video/x11/xsettings-client.c)
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
configure_flags() {
    MODEFLAGS=(-O0 -g); SAN=()
    if [[ $MODE == release ]]; then MODEFLAGS=(-O2 -DNDEBUG); fi
    if [[ $MODE == asan ]]; then SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all); fi
    VERBOSE_FLAG=(); if (( VERBOSE )); then VERBOSE_FLAG=(-v); fi
    # Optional developer flags are split as arguments, NEVER eval'ed as shell code.
    read -r -a EXTRA_CXX <<< "${PLPT_EXTRA_CXXFLAGS:-}"
    read -r -a EXTRA_LINK <<< "${PLPT_EXTRA_LDFLAGS:-}"
    CXXFLAGS=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -Wshadow -pthread -I "$ROOT" "${MODEFLAGS[@]}" "${SAN[@]}" "${EXTRA_CXX[@]}")
}
compile_cpp() {
    local source=$1 object=$2; shift 2
    mkdir -p "$(dirname "$object")"; say "Compilation: $source"
    run "$CXX" "${VERBOSE_FLAG[@]}" "${CXXFLAGS[@]}" "$@" -MMD -MP -MF "$object.d" -c "$source" -o "$object"
    [[ -f $object ]] || fail "compiler returned success without object: $object"
}
link_cpp() {
    local output=$1; shift
    say "Linking: $output"
    run "$CXX" "${VERBOSE_FLAG[@]}" "${MODEFLAGS[@]}" "${SAN[@]}" -pthread "$@" "${EXTRA_LINK[@]}" -o "$output"
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
    SDL_OBJECTS=()
    for source in "${sources[@]}"; do
        object="$DIR/objects/sdl/${source//\//_}.o"
        say "Compilation SDL C: $source"
        run "$CC" "${VERBOSE_FLAG[@]}" -std=c11 -D_GNU_SOURCE -fPIC -pthread -I "$DIR/config" -I "$SDL_ROOT/include" -I "$SDL_ROOT/src" "${MODEFLAGS[@]}" "${SAN[@]}" -MMD -MP -MF "$object.d" -c "$SDL_ROOT/$source" -o "$object"
        [[ -f $object ]] || fail "missing SDL object $object"
        SDL_OBJECTS+=("$object")
    done
    say 'Linking native SDL shared library (software renderer)'
    local strict=(); if [[ $MODE != asan ]]; then strict=(-Wl,--no-undefined); fi
    run "$CC" "${VERBOSE_FLAG[@]}" -shared "${SAN[@]}" -Wl,-soname,libSDL3.so.0 "${strict[@]}" "${SDL_OBJECTS[@]}" "${libs[@]}" -o "$BIN/libSDL3.so.0"
    [[ -f $BIN/libSDL3.so.0 ]] || fail 'SDL link did not create library'
}
build_project() {
    local tests=$1 source object started=$SECONDS
    # Clear first, including old executables, even when validation/compilation later fails.
    fresh "$DIR"; BIN="$DIR/bin"; run mkdir -p "$BIN" "$DIR/objects"
    toolchain; dependencies; configure_flags
    say "Compilation started: $MODE, clean deterministic objects (no cache)"
    COMMON_OBJECTS=(); MODEL_OBJECTS=(); GUI_OBJECTS=()
    for source in "${CORE_SOURCES[@]}"; do
        object="$DIR/objects/${source//\//_}.o"; compile_cpp "$source" "$object"; COMMON_OBJECTS+=("$object")
    done
    compile_cpp app/host_shell/main.cpp "$DIR/objects/cli.o"
    link_cpp "$BIN/plpt-monitor" "${COMMON_OBJECTS[@]}" "$DIR/objects/cli.o"
    if (( ! CORE_ONLY )); then
        build_sdl
        for source in "${IMGUI_SOURCES[@]}"; do
            object="$DIR/objects/imgui_${source//\//_}.o"
            # Vendor warning policy differs from PLPT; errors remain visible.
            mkdir -p "$(dirname "$object")"; say "Compilation ImGui: $source"
            run "$CXX" "${VERBOSE_FLAG[@]}" -std=c++20 "${MODEFLAGS[@]}" "${SAN[@]}" -I "$IMGUI_ROOT" -I "$IMGUI_ROOT/backends" -I "$SDL_ROOT/include" -MMD -MP -MF "$object.d" -c "$IMGUI_ROOT/$source" -o "$object"
            [[ -f $object ]] || fail "missing ImGui object $object"; GUI_OBJECTS+=("$object")
        done
        compile_cpp app/desktop_ui/main.cpp "$DIR/objects/desktop.o" -isystem "$IMGUI_ROOT" -isystem "$IMGUI_ROOT/backends" -isystem "$SDL_ROOT/include"
        link_cpp "$BIN/plpt-desktop" "${COMMON_OBJECTS[@]}" "${GUI_OBJECTS[@]}" "$DIR/objects/desktop.o" "$BIN/libSDL3.so.0" '-Wl,-rpath,$ORIGIN'
    fi
    if (( tests )); then
        for source in "${MODEL_SOURCES[@]}"; do
            object="$DIR/objects/${source//\//_}.o"; compile_cpp "$source" "$object"; MODEL_OBJECTS+=("$object")
        done
        compile_cpp tests/unit/test_main.cpp "$DIR/objects/unit.o"
        link_cpp "$BIN/plpt-unit-tests" "${COMMON_OBJECTS[@]}" "${MODEL_OBJECTS[@]}" "$DIR/objects/unit.o"
        compile_cpp tests/integration/platform_tests.cpp "$DIR/objects/platform.o"
        link_cpp "$BIN/plpt-platform-tests" "${COMMON_OBJECTS[@]}" "$DIR/objects/platform.o"
        for source in "${HEADERS[@]}"; do
            printf '#include "%s"\n' "$source" > "$DIR/header-probe.cpp"
            say "Independent header: $source"
            run "$CXX" "${VERBOSE_FLAG[@]}" "${CXXFLAGS[@]}" -fsyntax-only "$DIR/header-probe.cpp"
        done
        say "PASS ${#HEADERS[@]} independent native headers"; notice "PASS ${#HEADERS[@]} independent native headers"
    fi
    say "BUILD SUCCESS mode=$MODE elapsed_seconds=$((SECONDS-started)) (compilation including validation/extraction; tests not included)"
    notice "BUILD SUCCESS mode=$MODE elapsed_seconds=$((SECONDS-started)) headless=$HEADLESS core_only=$CORE_ONLY"
    printf 'mode=%s\ncompiler=%s\nheadless=%s\n' "$MODE" "$CXX" "$HEADLESS" > "$DIR/BUILD_SUCCEEDED"
}
PASS=0; SKIP=0
expect() {
    local name=$1 expected=$2 pattern=$3; shift 3
    local capture="$DIR/$name.log" rc=0
    say "Test started: $name (expected exit=$expected)"; printf '+ ' >> "$LOG"; command_text "$@" >> "$LOG"; printf '\n' >> "$LOG"
    "$@" > "$capture" 2>&1 || rc=$?
    cat "$capture" | tee -a "$LOG"
    if [[ $name == ipv6 && $rc == 77 ]]; then
        SKIP=$((SKIP+1)); say 'SKIP ipv6: unavailable, NOT counted as pass'; notice 'SKIP ipv6 (qualification incomplete)'; return 0
    fi
    if [[ $rc != "$expected" ]] || ! grep -Eq -- "$pattern" "$capture"; then say "FAIL test=$name expected=$expected actual=$rc command=$(command_text "$@")"; if (( rc != 0 )); then return "$rc"; else return 1; fi; fi
    if grep -Eq 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$capture"; then fail "sanitizer diagnostic in $name (even if exit matched)"; fi
    PASS=$((PASS+1)); say "PASS $name: exit=$rc"
    printf 'PASS %s exit=%s\n' "$name" "$rc" >> "$DIR/test-summary.txt"
    grep -E 'desktop-runtime:|cases,.*checks' "$capture" >> "$DIR/test-summary.txt" || true
}
tests() {
    PASS=0; SKIP=0
    printf 'Native Linux tests mode=%s\n' "$MODE" > "$DIR/test-summary.txt"
    expect unit 0 '0 failures' "$BIN/plpt-unit-tests"
    expect platform 0 '0 failures' "$BIN/plpt-platform-tests"
    expect ipv6 0 '0 failures' "$BIN/plpt-platform-tests" --ipv6
    expect cli-help 0 'no probes, no server, no sharing' "$BIN/plpt-monitor" --help
    expect cli-version 0 'no sharing' "$BIN/plpt-monitor" --version
    expect cli-unknown 2 'Usage:' "$BIN/plpt-monitor" --not-a-plpt-option
    expect cli-extra 2 'Usage:' "$BIN/plpt-monitor" --version extra
    expect cli-ambiguous 2 'Usage:' "$BIN/plpt-monitor" --help --version
    if (( ! CORE_ONLY )); then
        expect ui-smoke 0 'desktop-smoke:' env SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$BIN/plpt-desktop" --smoke-test --screenshot "$DIR/desktop.bmp"
        expect ui-unknown 2 'Usage:' "$BIN/plpt-desktop" --not-a-plpt-option
        expect ui-missing-argument 2 'Usage:' "$BIN/plpt-desktop" --smoke-test --screenshot
        expect ui-capture-requires-smoke 2 --smoke-test "$BIN/plpt-desktop" --screenshot never-created.bmp
        expect ui-driver-error 1 SDL_Init: env SDL_VIDEODRIVER=plpt-invalid-video-driver "$BIN/plpt-desktop" --smoke-test
        expect ui-file-error 1 SDL_IOFromFile: env SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$BIN/plpt-desktop" --smoke-test --screenshot "$BIN/plpt-desktop/not-a-directory.bmp"
        expect ui-disk-full 1 'SDL_(SaveBMP_IO|CloseIO):' env SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$BIN/plpt-desktop" --smoke-test --screenshot /dev/full
        if (( NATIVE_UI )); then
            (( ! HEADLESS )) || fail '--native-ui cannot qualify a --headless build'
            expect ui-native-x11 0 'video=x11 renderer=software' env SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software "$BIN/plpt-desktop" --smoke-test --screenshot "$DIR/desktop-native.bmp"
        else say 'NOT RUN: native X11 window launch (use --native-ui with DISPLAY; dummy is not native display validation)'; fi
    fi
    say "TEST SUMMARY passed=$PASS skipped=$SKIP mode=$MODE"
    printf 'TEST SUMMARY passed=%s skipped=%s mode=%s\n' "$PASS" "$SKIP" "$MODE" >> "$DIR/test-summary.txt"
    local summary; summary=$(cat "$DIR/test-summary.txt"); notice "${summary//$'\n'/%0A}"
    (( SKIP == 0 )) || return 77
}
fuzz() {
    MODE=asan; CORE_ONLY=1; DIR="$OUT/tests/fuzz"; fresh "$DIR"; BIN="$DIR/bin"; mkdir -p "$BIN" "$DIR/objects" "$DIR/corpus"
    toolchain; configure_flags
    compile_cpp engine/protocol/envelope.cpp "$DIR/objects/envelope.o" -fsanitize=fuzzer-no-link
    compile_cpp tests/fuzz/envelope_fuzz.cpp "$DIR/objects/fuzz.o" -fsanitize=fuzzer-no-link
    link_cpp "$BIN/plpt-fuzz-envelope" -fsanitize=fuzzer "$DIR/objects/envelope.o" "$DIR/objects/fuzz.o"
    run cp tests/fuzz/corpus/hello tests/fuzz/corpus/chat "$DIR/corpus/"
    run "$BIN/plpt-fuzz-envelope" -runs=20000 -seed=20261113 -max_len=4096 -timeout=2 "-artifact_prefix=$DIR/" "$DIR/corpus"
    notice 'PASS native libFuzzer + ASan/UBSan: 20000 executions'
}
package() {
    (( ! HEADLESS && ! CORE_ONLY )) || fail 'package requires the full native desktop, not a headless/core-only profile'
    fresh "$OUT/packages"
    MODE=release; DIR="$OUT/release"; build_project 1; tests
    local stage="$OUT/packages/stage" archive="$OUT/packages/PLPT-0.1.0-preview-linux-x64.tar.gz"
    fresh "$stage"; mkdir -p "$stage/PLPT/bin" "$stage/PLPT/docs" "$stage/PLPT/licenses"
    run cp "$BIN/plpt-monitor" "$BIN/plpt-desktop" "$BIN/libSDL3.so.0" "$stage/PLPT/bin/"
    run cp README.md docs/BUILD.md docs/SECURITY.md docs/QUICKSTART.md "$stage/PLPT/docs/"
    run cp third_party/DEPENDENCIES.md "$stage/PLPT/docs/"
    run cp third_party/licenses/SDL3.txt third_party/licenses/ImGui.txt third_party/licenses/Bundled-notices.txt "$stage/PLPT/licenses/"
    run tar -czf "$archive" -C "$stage" PLPT
    run sha256sum "$archive"
    local check="$OUT/packages/check"; fresh "$check"; run tar -xzf "$archive" -C "$check"
    run cmp "$BIN/plpt-monitor" "$check/PLPT/bin/plpt-monitor"
    run cmp "$BIN/plpt-desktop" "$check/PLPT/bin/plpt-desktop"
    run cmp "$BIN/libSDL3.so.0" "$check/PLPT/bin/libSDL3.so.0"
    local license
    for license in SDL3.txt ImGui.txt Bundled-notices.txt; do run cmp "third_party/licenses/$license" "$check/PLPT/licenses/$license"; done
    run "$check/PLPT/bin/plpt-monitor" --version
    run env SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software "$check/PLPT/bin/plpt-desktop" --smoke-test
    run ldd "$check/PLPT/bin/plpt-desktop"
    say "PACKAGE SUCCESS $archive (development archive, host glibc/libstdc++/X11 required)"; notice 'PASS native Release package extracted and executed'
}
selftest() {
    # Exercise this very script as a child, after releasing our lock. No helper build engine.
    rmdir "$OUT/.build-lock"; LOCK=0
    local rc=0
    PLPT_EXPECT_FAILURE=1 PLPT_CXX=/does/not/exist "$ROOT/build.sh" doctor --core-only > "$OUT/logs/selftest-missing-compiler.log" 2>&1 || rc=$?
    (( rc != 0 )) || fail 'missing compiler falsely succeeded'
    rc=0
    PLPT_EXPECT_FAILURE=1 PLPT_EXTRA_CXXFLAGS=-fplpt-deliberate-invalid-option "$ROOT/build.sh" debug --core-only > "$OUT/logs/selftest-compile-failure.log" 2>&1 || rc=$?
    (( rc != 0 )) && [[ ! -e $OUT/debug/BUILD_SUCCEEDED && ! -e $OUT/debug/bin/plpt-monitor ]] || fail 'failed compile left a stale successful build'
    rc=0
    PLPT_EXPECT_FAILURE=1 PLPT_EXTRA_LDFLAGS=-Wl,--plpt-deliberate-invalid-option "$ROOT/build.sh" debug --core-only > "$OUT/logs/selftest-link-failure.log" 2>&1 || rc=$?
    (( rc != 0 )) && [[ ! -e $OUT/debug/BUILD_SUCCEEDED && ! -e $OUT/debug/bin/plpt-monitor ]] || fail 'failed link left a stale successful build'
    # Exercise the same hash verifier without ever modifying the verified originals.
    DIR="$OUT/tests/negative"; fresh "$DIR"
    printf 'corrupt archive for negative test\n' > "$DIR/corrupt.tar.gz"
    rc=0
    (verify "$DIR/corrupt.tar.gz" "$IMGUI_SHA") > "$OUT/logs/selftest-hash-failure.log" 2>&1 || rc=$?
    (( rc != 0 )) && grep -q 'SHA-256 mismatch' "$OUT/logs/selftest-hash-failure.log" || fail 'bad archive was not rejected'
    rc=0
    expect intentional-exit 0 'should never pass' "$OUT/doctor/probe" --fail || rc=$?
    (( rc == 23 )) || fail "test exit code was not preserved (expected 23, got $rc)"
    say 'SELFTEST PASS: missing compiler, compilation, linking and archive hash failures propagate; no stale successful output'
    notice 'SELFTEST PASS: compiler/compile/link/hash failure propagation'
}
case "$ACTION" in
    deps) plain_directory "$DEPS"; mkdir -p "$DEPS"; if (( FETCH )); then
        fetch_one "https://codeload.github.com/libsdl-org/SDL/tar.gz/$SDL_COMMIT" "$SDL_ARCHIVE" "$SDL_SHA"
        fetch_one "https://codeload.github.com/ocornut/imgui/tar.gz/$IMGUI_COMMIT" "$IMGUI_ARCHIVE" "$IMGUI_SHA"
        fi; dependencies;;
    doctor) toolchain; dependencies;;
    debug|release) MODE=$ACTION; DIR="$OUT/$MODE"; build_project 0;;
    test) DIR="$OUT/tests/$MODE"; build_project 1; tests;;
    asan) MODE=asan; DIR="$OUT/tests/asan"; build_project 1; tests;;
    fuzz) fuzz;;
    package) package;;
    selftest) selftest;;
    clean) for name in debug release tests packages doctor; do plain_directory "$OUT/$name"; if [[ -e $OUT/$name ]]; then run rm -rf -- "$OUT/$name"; fi; done; say 'CLEAN SUCCESS: compiled outputs removed; verified archives and logs retained';;
esac
say "SUCCESS operation=$ACTION log=$LOG"
