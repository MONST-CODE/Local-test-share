@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem DevLink authoritative Windows build: MSYS2 UCRT64 clang++.exe ONLY.
rem
rem Encodage : UTF-8 sans BOM, fins de ligne CRLF (.gitattributes). La console
rem passe en page de codes 65001 pendant l'operation (accents, bandeau, glyphes),
rem puis la page d'origine est restauree a la sortie, y compris en cas d'echec.
rem
rem Affichage : une etape numerotee par phase reelle ([n/N]), une ligne de
rem progression reecrite sur place (VT), aucune sortie brute d'outil. Chaque
rem commande garde sa capture complete dans out\windows\logs ; un echec affiche
rem la sortie de la commande fautive, telle quelle (le problem matcher $gcc de
rem VS Code continue de la reconnaitre). --verbose retablit la sortie integrale.
cd /d "%~dp0"
set "ROOT=%CD%"
set "ACTION=%~1"
if not defined ACTION set "ACTION=debug"
if not "%~1"=="" shift
set "MODE=debug"
set "VERBOSE=0"
set "CORE_ONLY=0"
set "FETCH=0"
set "NATIVE_UI=0"
set "PLAIN=0"
:arguments
if "%~1"=="" goto arguments_done
if /I "%~1"=="--verbose" (set "VERBOSE=1"& shift& goto arguments)
if /I "%~1"=="--core-only" (set "CORE_ONLY=1"& shift& goto arguments)
if /I "%~1"=="--fetch" (set "FETCH=1"& shift& goto arguments)
if /I "%~1"=="--native-ui" (set "NATIVE_UI=1"& shift& goto arguments)
if /I "%~1"=="--plain" (set "PLAIN=1"& shift& goto arguments)
if /I "%ACTION%"=="test" if /I "%~1"=="debug" (set "MODE=debug"& shift& goto arguments)
if /I "%ACTION%"=="test" if /I "%~1"=="release" (set "MODE=release"& shift& goto arguments)
if /I "%ACTION%"=="test" if /I "%~1"=="asan" (set "MODE=asan"& shift& goto arguments)
echo ERROR: unknown argument %~1. See: build.bat help
exit /b 2
:arguments_done
if /I "%ACTION%"=="release" set "MODE=release"
if /I "%ACTION%"=="package" set "MODE=release"
if /I "%ACTION%"=="asan" set "MODE=asan"
if /I "%ACTION%"=="fuzz" set "MODE=fuzz"
set "VALID="
for %%A in (debug release test asan fuzz clean package doctor deps selftest ide-check help) do if /I "%ACTION%"=="%%A" set "VALID=1"
if not defined VALID (echo ERROR: unknown operation %ACTION%. See: build.bat help& exit /b 2)
set "LOG="
set "LOCK=0"
set "FINALRC=0"
call :ui_init
call :product_identity >nul 2>nul
if /I "%ACTION%"=="help" goto help
if /I not "%PROCESSOR_ARCHITECTURE%"=="AMD64" (call :problem "native Windows x64 command prompt required"& set "FINALRC=2"& goto finish)
if not exist "engine\core\types.hpp" (call :problem "wrong checkout"& set "FINALRC=2"& goto finish)
set "OUT=%ROOT%\out\windows"
set "DEPS=%OUT%\dependencies"
for %%D in ("%ROOT%\out" "%OUT%" "%OUT%\logs" "%OUT%\dependencies" "%OUT%\debug" "%OUT%\release" "%OUT%\tests" "%OUT%\packages" "%OUT%\doctor") do (
  call :plain_directory "%%~D" || (set "FINALRC=1"& goto finish)
)
if not exist "%OUT%\logs" mkdir "%OUT%\logs"
if not exist "%OUT%\logs" (call :problem "cannot create logs"& set "FINALRC=1"& goto finish)
set "LOG=%OUT%\logs\%ACTION%-%MODE%-%RANDOM%-%RANDOM%.log"
set "CMDLOG="
set "DEVLINK_COMMAND_INDEX=0"
>"%LOG%" echo DevLink direct Windows operation=%ACTION% mode=%MODE% started=%DATE% %TIME%
call :banner
mkdir "%OUT%\.build-lock" >nul 2>nul
if not "%errorlevel%"=="0" (call :lock_busy& set "FINALRC=1"& goto finish)
set "LOCK=1"
set "SDL_SHA=4a44381560c433ff42da3a6aff788fba2bba84a44b7f63ea396dec9c1cf79f78"
set "SDL_PACKAGE=mingw-w64-ucrt-x86_64-sdl3-3.4.20-1-any.pkg.tar.zst"
set "IMGUI_SHA=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114"
set "SDL_ARCHIVE=%DEPS%\%SDL_PACKAGE%"
set "IMGUI_ARCHIVE=%DEPS%\imgui-c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz"
rem Explicit project sources. The CLI monitor stays model-free; the desktop
rem control post and the tests link the session models and the LAN networking.
set "CORE_SOURCES=engine/core/random.cpp engine/core/digest.cpp engine/core/mac.cpp engine/core/secrets.cpp engine/core/format.cpp engine/diagnostics/audit.cpp engine/discovery/process_tree.cpp engine/discovery/terminal_classifier.cpp engine/discovery/monitor.cpp engine/service_registry/registry.cpp engine/port_allocator/port_allocator.cpp engine/platform/socket.cpp engine/platform/windows/windows_platform.cpp"
set "MODEL_SOURCES=engine/session/session.cpp engine/messaging/messaging.cpp engine/protocol/envelope.cpp engine/resource_manager/resource.cpp"
set "NET_SOURCES=engine/net/wire.cpp engine/net/discovery.cpp engine/net/host_server.cpp engine/net/guest_client.cpp engine/net/proxy.cpp engine/net/transport.cpp engine/net/transport_tls.cpp"
rem Moteur navigateur reel (fabrique WebView2 + repli gere) : poste de commande et tests.
set "BROWSER_SOURCES=engine/browser/webview2_backend.cpp engine/browser/egress_guard.cpp engine/media/jpeg_decoder.cpp engine/share/media_wire.cpp engine/share/share_host.cpp engine/share/share_guest.cpp"
rem Design system DevLink : themes.cpp porte les palettes DevDark/DevLight.
set "UI_SOURCES=app/desktop_ui/design_system/themes.cpp app/desktop_ui/design_system/typography.cpp app/desktop_ui/design_system/window_icon_data.cpp"
set "IMGUI_SOURCES=imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_sdl3.cpp backends/imgui_impl_sdlrenderer3.cpp"
set "TEST_SOURCES=tests/unit/test_main.cpp tests/integration/platform_tests.cpp tests/integration/browser_pipeline_tests.cpp tests/integration/share_pipeline_tests.cpp tests/integration/desktop_e2e_tests.cpp"
set "HEADERS=engine/share/view_mapping.hpp engine/share/share_policy.hpp engine/share/media_wire.hpp engine/share/share_host.hpp engine/share/share_guest.hpp engine/browser/cdp_json.hpp engine/browser/egress_guard.hpp engine/media/jpeg_decoder.hpp engine/browser/browser.hpp engine/browser/remote_frame.hpp engine/media/frame_queue.hpp engine/browser/url_origin.hpp engine/browser/webview2_backend.hpp engine/core/clock.hpp engine/core/digest.hpp engine/core/format.hpp engine/core/mac.hpp engine/core/random.hpp engine/core/result.hpp engine/core/secrets.hpp engine/core/text.hpp engine/core/types.hpp engine/diagnostics/audit.hpp engine/discovery/monitor.hpp engine/discovery/process_tree.hpp engine/discovery/terminal_classifier.hpp engine/messaging/messaging.hpp engine/net/chat.hpp engine/net/discovery.hpp engine/net/guest_client.hpp engine/net/host_server.hpp engine/net/proxy.hpp engine/net/transport.hpp engine/net/wire.hpp engine/platform/connection_pool.hpp engine/platform/native.hpp engine/platform/platform.hpp engine/platform/socket.hpp engine/port_allocator/port_allocator.hpp engine/protocol/envelope.hpp engine/resource_manager/resource.hpp engine/service_registry/registry.hpp engine/session/session.hpp engine/platform/windows/windows_platform.hpp engine/platform/windows/winsock.hpp"
rem En-tetes du poste de commande : ils incluent <imgui.h>, donc leur sonde
rem d'independance recoit les memes chemins -isystem que sa compilation reelle.
set "UI_HEADERS=app/desktop_ui/design_system/tokens.hpp app/desktop_ui/design_system/themes.hpp app/desktop_ui/design_system/typography.hpp app/desktop_ui/design_system/icons.hpp app/desktop_ui/design_system/components.hpp app/desktop_ui/design_system/layout.hpp app/desktop_ui/design_system/window_icon_data.hpp"
call :stamp
set "OP_START=%STAMP%"
call :dispatch
set "FINALRC=%errorlevel%"
if "%LOCK%"=="1" rmdir "%OUT%\.build-lock"
set "LOCK=0"
if "%STEP_OPEN%"=="1" if "%FINALRC%"=="77" call :step_skip "non disponible ici : SKIP, code 77"
if "%STEP_OPEN%"=="1" if not "%FINALRC%"=="0" if not "%FINALRC%"=="77" call :step_fail
if "%FINALRC%"=="0" (
  >>"%LOG%" echo SUCCESS operation=%ACTION%
) else (
  >>"%LOG%" echo FAILURE operation=%ACTION% exit=%FINALRC%
  if "%GITHUB_ACTIONS%"=="true" if not "%DEVLINK_EXPECT_FAILURE%"=="1" echo ::error title=DevLink Windows::Operation %ACTION% failed, exit %FINALRC%. See retained logs.
)
call :summary
goto finish

:help
call :banner
echo   %C_CREAM%Usage%C0%  build.bat ^<opération^> [options]
echo(
echo   %C_ACC%Opérations%C0%
echo     %C_CREAM%debug%C0%                 Compile la configuration Debug (opération par défaut)
echo     %C_CREAM%release%C0%               Compile la configuration Release optimisée
echo     %C_CREAM%test%C0% [debug^|release^|asan]
echo                           Compile depuis zéro puis exécute toutes les suites de tests
echo     %C_CREAM%asan%C0%                  Tests avec AddressSanitizer (SKIP, code 77, si le runtime manque)
echo     %C_CREAM%fuzz%C0%                  Fuzzing libFuzzer de l'enveloppe (SKIP, code 77, si indisponible)
echo     %C_CREAM%package%C0%               Release + tests + archive ZIP publiée dans releases\
echo     %C_CREAM%doctor%C0%                Diagnostique la chaîne MSYS2 UCRT64 et les dépendances
echo     %C_CREAM%deps%C0% [--fetch]        Vérifie les archives épinglées ; --fetch les télécharge
echo     %C_CREAM%clean%C0%                 Supprime les sorties compilées (releases\ et journaux conservés)
echo     %C_CREAM%selftest%C0%              Auto-test des garde-fous du script (échecs volontaires)
echo     %C_CREAM%ide-check%C0%             Vérifie le contrat des tâches VS Code
echo     %C_CREAM%help%C0%                  Affiche cette aide
echo(
echo   %C_ACC%Options%C0%
echo     %C_CREAM%--verbose%C0%             Affiche chaque commande et sa sortie complète (diagnostic)
echo     %C_CREAM%--plain%C0%               Sortie sans couleur ni réécriture de ligne (aussi : NO_COLOR=1)
echo     %C_CREAM%--native-ui%C0%           Tests : lance aussi une vraie fenêtre Windows
echo     %C_CREAM%--core-only%C0%           Moteur et moniteur seulement, sans interface SDL3/ImGui
echo     %C_CREAM%--fetch%C0%               deps : autorise le téléchargement des archives épinglées
echo(
echo   %C_DIM%Seul deps --fetch télécharge. Un SKIP (code 77) n'est jamais compté comme réussi.%C0%
echo   %C_DIM%Chaque commande est journalisée sous out\windows\logs ; un échec affiche sa sortie.%C0%
echo(
set "FINALRC=0"
goto finish

:finish
call :ui_restore
endlocal & exit /b %FINALRC%

rem ===========================================================================
rem Interface console : bandeau, etapes [n/N], progression, resume.
rem Les sous-routines d'affichage finissent TOUTES par "exit /b 0" : un
rem "set /p" laisse ERRORLEVEL a 1 et ne doit jamais ressembler a un echec.
rem Aucun bloc entre parentheses n'affiche un libelle : un ")" dans le texte
rem fermerait le bloc. Les branches utilisent goto.
rem ===========================================================================
:ui_init
set "CP0="
for /f "tokens=2 delims=:." %%c in ('chcp') do for /f "tokens=1" %%d in ("%%c") do set "CP0=%%d"
chcp 65001 >nul 2>nul
set "STEP=0"
set "TOTAL=0"
set "STEP_OPEN=0"
set "STEP_LABEL="
set "FAILED_STEP="
set "ESC="
set "COLOR=1"
if "%PLAIN%"=="1" set "COLOR=0"
if defined NO_COLOR set "COLOR=0"
if "%DEVLINK_PLAIN%"=="1" set "COLOR=0"
if "%COLOR%"=="1" for /f %%a in ('echo prompt $E^| cmd /d') do set "ESC=%%a"
if not defined ESC set "COLOR=0"
rem Progression reecrite sur place seulement dans un terminal interactif : pas
rem en CI (journal lineaire), pas en --verbose (les sorties s'intercaleraient).
set "LIVE=%COLOR%"
if "%VERBOSE%"=="1" set "LIVE=0"
if "%GITHUB_ACTIONS%"=="true" set "LIVE=0"
set "C0="
set "C_B1="
set "C_B2="
set "C_B3="
set "C_B4="
set "C_B5="
set "C_B6="
set "C_ACC="
set "C_CREAM="
set "C_MUTED="
set "C_DIM="
set "C_OK="
set "C_WARN="
set "C_ERR="
set "C_BOLD="
set "EL="
set "COL="
set "COL_ROW="
if not "%COLOR%"=="1" goto ui_init_glyphs
rem Degrade marron de la marque (assets/branding/BRAND_GUIDELINES.md, themes.cpp).
set "C0=%ESC%[0m"
set "C_B1=%ESC%[38;2;222;188;152m"
set "C_B2=%ESC%[38;2;206;166;128m"
set "C_B3=%ESC%[38;2;196;154;120m"
set "C_B4=%ESC%[38;2;180;138;104m"
set "C_B5=%ESC%[38;2;164;122;89m"
set "C_B6=%ESC%[38;2;129;92;67m"
set "C_ACC=%ESC%[38;2;196;154;120m"
set "C_CREAM=%ESC%[38;2;245;233;220m"
set "C_MUTED=%ESC%[38;2;187;169;154m"
set "C_DIM=%ESC%[38;2;140;122;108m"
set "C_OK=%ESC%[38;2;104;163;87m"
set "C_WARN=%ESC%[38;2;217;155;67m"
set "C_ERR=%ESC%[38;2;199;84;80m"
set "C_BOLD=%ESC%[1m"
set "EL=%ESC%[2K%ESC%[1G"
set "COL=%ESC%[62G"
set "COL_ROW=%ESC%[26G"
:ui_init_glyphs
rem Glyphes presents dans Consolas ET Lucida Console : la console classique
rem (conhost) ne substitue aucune police, un glyphe absent deviendrait un carre.
set "G_OK=√"
set "G_FAIL=×"
set "G_SKIP=○"
set "G_RUN=•"
set "G_DOT=·"
set "RULE=────────────────────────────────────────────────────────────────────────"
exit /b 0

:ui_restore
if defined CP0 chcp %CP0% >nul 2>nul
exit /b 0

:banner
echo(
set "BANNER_ROW=0"
if not exist "%ROOT%\tools\build\devlink-banner.txt" goto banner_fallback
for /f "usebackq delims=" %%L in ("%ROOT%\tools\build\devlink-banner.txt") do call :banner_row "%%L"
goto banner_caption
:banner_fallback
echo   %C_B3%%C_BOLD%D E V L I N K%C0%
:banner_caption
echo(
echo   %C_CREAM%%C_BOLD%DevLink %PRODUCT_VERSION%%C0%  %C_DIM%%G_DOT%%C0%  %C_MUTED%build Windows x64%C0%  %C_DIM%%G_DOT%%C0%  %C_MUTED%MSYS2 UCRT64 %G_DOT% clang++%C0%
if /I "%ACTION%"=="help" goto banner_rule
call :relpath LOG_REL "%LOG%"
set "OP_TEXT=%ACTION%"
if /I "%ACTION%"=="test" set "OP_TEXT=test %MODE%"
if "%CORE_ONLY%"=="1" set "OP_TEXT=%OP_TEXT% --core-only"
if "%NATIVE_UI%"=="1" set "OP_TEXT=%OP_TEXT% --native-ui"
echo   %C_MUTED%Opération%C0%  %C_CREAM%%OP_TEXT%%C0%   %C_DIM%%G_DOT%%C0%   %C_MUTED%Journal%C0%  %C_DIM%%LOG_REL%%C0%
:banner_rule
echo   %C_DIM%%RULE%%C0%
echo(
exit /b 0

:banner_row
set /a BANNER_ROW+=1 >nul
set "ROWC=%C_B1%"
if "%BANNER_ROW%"=="2" set "ROWC=%C_B2%"
if "%BANNER_ROW%"=="3" set "ROWC=%C_B3%"
if "%BANNER_ROW%"=="4" set "ROWC=%C_B4%"
if "%BANNER_ROW%"=="5" set "ROWC=%C_B5%"
if "%BANNER_ROW%"=="6" set "ROWC=%C_B6%"
echo   %ROWC%%~1%C0%
exit /b 0

:relpath
rem %1 = variable de sortie, %2 = chemin. Retire le prefixe du depot, pour l'affichage.
set "RP_VALUE=%~2"
call set "RP_VALUE=%%RP_VALUE:%ROOT%\=%%"
set "%~1=%RP_VALUE%"
exit /b 0

:step
rem %1 = libelle. Ouvre l'etape suivante ; la ligne est completee par step_done.
set /a STEP+=1 >nul
set "STEP_LABEL=%~1"
set "STEP_OPEN=1"
set "UNIT_INDEX=0"
set "UNIT_TOTAL=0"
call :stamp
set "STEP_START=%STAMP%"
if defined LOG >>"%LOG%" echo(
if defined LOG >>"%LOG%" echo ==== [%STEP%/%TOTAL%] %~1
if not "%LIVE%"=="1" goto step_plain
<nul set /p "=%EL%  %C_DIM%%G_RUN%%C0%  %C_ACC%[%STEP%/%TOTAL%]%C0% %C_CREAM%%~1%C0%"
exit /b 0
:step_plain
echo   %C_ACC%[%STEP%/%TOTAL%]%C0% %~1
exit /b 0

:step_progress
rem %1/%2 = rang/total, %3 = element courant. Interactif seulement.
if not "%LIVE%"=="1" exit /b 0
<nul set /p "=%EL%  %C_DIM%%G_RUN%%C0%  %C_ACC%[%STEP%/%TOTAL%]%C0% %C_CREAM%%STEP_LABEL%%C0%%COL%%C_DIM%%~1/%~2 %G_DOT% %~3%C0%"
exit /b 0

:step_progress_text
rem %1 = texte libre de progression. Interactif seulement.
if not "%LIVE%"=="1" exit /b 0
<nul set /p "=%EL%  %C_DIM%%G_RUN%%C0%  %C_ACC%[%STEP%/%TOTAL%]%C0% %C_CREAM%%STEP_LABEL%%C0%%COL%%C_DIM%%~1%C0%"
exit /b 0

:step_elapsed
call :stamp
set /a "STEP_CS=STAMP-STEP_START" >nul
if %STEP_CS% LSS 0 set /a "STEP_CS+=8640000" >nul
call :fmt_duration %STEP_CS% STEP_DUR
exit /b 0

:step_done
rem %1 = detail (facultatif).
call :step_elapsed
set "STEP_OPEN=0"
set "DETAIL=%~1"
if defined DETAIL goto step_done_detail
set "DETAIL=%STEP_DUR%"
goto step_done_print
:step_done_detail
set "DETAIL=%~1 %G_DOT% %STEP_DUR%"
:step_done_print
if defined LOG >>"%LOG%" echo ==== OK [%STEP%/%TOTAL%] %STEP_LABEL% - %DETAIL%
if not "%LIVE%"=="1" goto step_done_plain
echo %EL%  %C_OK%%G_OK%%C0%  %C_ACC%[%STEP%/%TOTAL%]%C0% %STEP_LABEL%%COL%%C_DIM%%DETAIL%%C0%
exit /b 0
:step_done_plain
echo        %C_OK%%G_OK%%C0% %DETAIL%
exit /b 0

:step_skip
rem Etape non executable dans cet environnement : jamais presentee comme reussie.
call :step_elapsed
set "STEP_OPEN=0"
if defined LOG >>"%LOG%" echo ==== SKIP [%STEP%/%TOTAL%] %STEP_LABEL% - %~1
if not "%LIVE%"=="1" goto step_skip_plain
echo %EL%  %C_WARN%%G_SKIP%%C0%  %C_ACC%[%STEP%/%TOTAL%]%C0% %STEP_LABEL%%COL%%C_WARN%%~1%C0%
exit /b 0
:step_skip_plain
echo        %C_WARN%%G_SKIP%%C0% %~1
exit /b 0

:step_fail
if not "%STEP_OPEN%"=="1" exit /b 0
call :step_elapsed
set "STEP_OPEN=0"
set "FAILED_STEP=[%STEP%/%TOTAL%] %STEP_LABEL%"
if defined LOG >>"%LOG%" echo ==== FAIL [%STEP%/%TOTAL%] %STEP_LABEL% - %STEP_DUR%
if not "%LIVE%"=="1" goto step_fail_plain
echo %EL%  %C_ERR%%G_FAIL%%C0%  %C_ACC%[%STEP%/%TOTAL%]%C0% %STEP_LABEL%%COL%%C_ERR%échec %G_DOT% %STEP_DUR%%C0%
exit /b 0
:step_fail_plain
echo        %C_ERR%%G_FAIL% échec%C0% %G_DOT% %STEP_DUR%
exit /b 0

:row
rem Ligne de resultat sous une etape : %1 = ok^|skip^|note, %2 = nom, %3 = detail.
set "ROW_GLYPH=%C_OK%%G_OK%%C0%"
if /I "%~1"=="skip" set "ROW_GLYPH=%C_WARN%%G_SKIP%%C0%"
if /I "%~1"=="note" set "ROW_GLYPH=%C_DIM%%G_DOT%%C0%"
if "%COLOR%"=="1" goto row_color
echo          %ROW_GLYPH% %~2 - %~3
exit /b 0
:row_color
echo          %ROW_GLYPH% %C_CREAM%%~2%C0%%COL_ROW%%C_DIM%%~3%C0%
exit /b 0

:note_later
rem Remarque affichee dans le resume final (et journalisee tout de suite).
if defined LOG >>"%LOG%" echo NOTE: %~1
if defined LOG >>"%LOG%.notes" echo %~1
exit /b 0

:fmt_duration
rem %1 = centiemes de seconde, %2 = variable de sortie ("4,2 s" ou "3 min 07 s").
set /a "FD_CS=%~1, FD_S=FD_CS/100, FD_D=(FD_CS%%100)/10, FD_M=FD_S/60, FD_R=FD_S%%60" >nul
if %FD_S% LSS 60 (set "%~2=%FD_S%,%FD_D% s"& exit /b 0)
if %FD_R% LSS 10 set "FD_R=0%FD_R%"
set "%~2=%FD_M% min %FD_R% s"
exit /b 0

:count_words
rem %1 = variable de sortie, %2... = liste.
set "CW_OUT=%~1"
set "CW_N=0"
:count_words_loop
shift
if "%~1"=="" goto count_words_end
set /a CW_N+=1 >nul
goto count_words_loop
:count_words_end
set "%CW_OUT%=%CW_N%"
exit /b 0

:print_excerpt
rem %1 = fichier, %2 = lignes maximum, %3 = capture complete (pour le renvoi).
rem Lignes BRUTES, non indentees : "fichier:ligne:col: error:" reste reconnu par
rem le problem matcher $gcc de VS Code.
set "EX_TOTAL=0"
for /f %%N in ('type "%~1" ^| find /c /v ""') do set "EX_TOTAL=%%N"
if "%EX_TOTAL%"=="0" exit /b 0
echo     %C_DIM%┌─ sortie%C0%
for /f "tokens=1,* delims=:" %%A in ('findstr /n "^" "%~1"') do if %%A LEQ %~2 echo(%%B
set /a "EX_MORE=EX_TOTAL-%~2" >nul
call :relpath EX_REL "%~3"
if %EX_MORE% GTR 0 goto print_excerpt_more
echo     %C_DIM%└─ capture complète : %EX_REL%%C0%
exit /b 0
:print_excerpt_more
echo     %C_DIM%└─ %EX_MORE% ligne(s) de plus : %EX_REL%%C0%
exit /b 0

:lock_busy
call :problem "un autre build détient out\windows\.build-lock"
echo     %C_MUTED%Vérifiez qu'aucun build.bat ni clang++ ne tourne, puis supprimez le verrou :%C0%
echo     %C_CREAM%rmdir "%OUT%\.build-lock"%C0%
echo     %C_DIM%Un build interrompu par Ctrl+C laisse ce verrou en place.%C0%
exit /b 0

:summary
call :stamp
set /a "OP_CS=STAMP-OP_START" >nul
if %OP_CS% LSS 0 set /a "OP_CS+=8640000" >nul
call :fmt_duration %OP_CS% OP_DUR
call :relpath LOG_REL "%LOG%"
echo(
echo   %C_DIM%%RULE%%C0%
if "%FINALRC%"=="0" goto summary_ok
if "%FINALRC%"=="77" goto summary_skip
echo   %C_ERR%%C_BOLD%%G_FAIL% Échec%C0%  %C_CREAM%%OP_TEXT%%C0%  %C_DIM%%G_DOT% code %FINALRC% %G_DOT% %OP_DUR%%C0%
if defined FAILED_STEP echo     %C_MUTED%Étape%C0%      %FAILED_STEP%
goto summary_paths
:summary_skip
echo   %C_WARN%%C_BOLD%%G_SKIP% Terminé avec des étapes ignorées%C0%  %C_CREAM%%OP_TEXT%%C0%  %C_DIM%%G_DOT% code 77 %G_DOT% %OP_DUR%%C0%
echo     %C_MUTED%Un SKIP n'est jamais compté comme une réussite : la qualification reste incomplète.%C0%
goto summary_paths
:summary_ok
echo   %C_OK%%C_BOLD%%G_OK% Réussite%C0%  %C_CREAM%%OP_TEXT%%C0%  %C_DIM%%G_DOT% %STEP%/%TOTAL% étapes %G_DOT% %OP_DUR%%C0%
:summary_paths
if not defined SUMMARY_ARTIFACT goto summary_log
call :relpath ART_REL "%SUMMARY_ARTIFACT%"
echo     %C_MUTED%%SUMMARY_ARTIFACT_LABEL%%C0%%C_CREAM%%ART_REL%%C0%
:summary_log
echo     %C_MUTED%Journal    %C0%%C_DIM%%LOG_REL%%C0%
if not exist "%LOG%.notes" goto summary_hint
for /f "usebackq delims=" %%N in ("%LOG%.notes") do echo     %C_DIM%%G_DOT% %%N%C0%
:summary_hint
if "%FINALRC%"=="0" goto summary_end
if "%FINALRC%"=="77" goto summary_end
if not "%VERBOSE%"=="1" echo     %C_DIM%Relancez avec --verbose pour afficher chaque commande et sa sortie.%C0%
:summary_end
echo(
exit /b 0

rem ===========================================================================
rem Operations
rem ===========================================================================
:dispatch
set "SUMMARY_ARTIFACT="
set "SUMMARY_ARTIFACT_LABEL=Binaires   "
if /I "%ACTION%"=="deps" goto dependencies_action
if /I "%ACTION%"=="clean" goto clean_action
if /I "%ACTION%"=="doctor" goto doctor_action
if /I "%ACTION%"=="selftest" goto selftest
if /I "%ACTION%"=="ide-check" goto ide_check
if /I "%ACTION%"=="fuzz" goto fuzz
if /I "%ACTION%"=="package" goto package
if /I "%ACTION%"=="debug" set "MODE=debug"
if /I "%ACTION%"=="release" set "MODE=release"
set "WITH_TESTS=0"
set "DIR=%OUT%\%MODE%"
if /I "%ACTION%"=="test" (set "WITH_TESTS=1"& set "DIR=%OUT%\tests\%MODE%")
if /I "%ACTION%"=="asan" (set "MODE=asan"& set "WITH_TESTS=1"& set "DIR=%OUT%\tests\asan")
set "TOTAL=8"
if "%WITH_TESTS%"=="1" set "TOTAL=11"
call :build_project || exit /b
set "SUMMARY_ARTIFACT=%DIR%\bin"
if "%WITH_TESTS%"=="1" call :tests || exit /b
exit /b 0

:doctor_action
set "TOTAL=3"
call :step "Contrôle de la chaîne de compilation UCRT64"
call :toolchain || exit /b
call :step_done "%TOOLCHAIN_SUMMARY%"
if "%ASAN_AVAILABLE%"=="true" (call :row ok "ASan" "runtime UCRT64 présent") else call :row skip "ASan" "runtime UCRT64 absent : test asan renverra SKIP, code 77"
if "%FUZZ_AVAILABLE%"=="true" (call :row ok "libFuzzer" "sonde compilée, liée et exécutée") else call :row skip "libFuzzer" "sonde non opérationnelle : fuzz renverra SKIP, code 77"
call :step "Vérification des dépendances épinglées"
call :dependencies || exit /b
if "%CORE_ONLY%"=="1" (call :step_skip "non requis avec --core-only") else call :step_done "SDL3 3.4.20-1 %G_DOT% ImGui c1aaac6 %G_DOT% SHA-256 OK"
call :step "Préparation de Dear ImGui"
if "%CORE_ONLY%"=="1" (call :step_skip "non requis avec --core-only"& exit /b 0)
call :prepare_imgui "%OUT%\doctor" || exit /b
call :step_done "sources épinglées extraites et contrôlées"
exit /b 0

:plain_directory
rem Reject reparse points before any recursive cleanup; all callers use fixed checkout descendants.
if not exist "%~1" exit /b 0
set "ATTR="
for %%I in ("%~1") do set "ATTR=%%~aI"
echo %ATTR% | findstr /I "l" >nul
if "%errorlevel%"=="0" (call :problem "reparse-point output is not allowed: %~1"& exit /b 1)
exit /b 0

:fresh
call :plain_directory "%~1" || exit /b
if exist "%~1" rmdir /s /q "%~1"
if exist "%~1" (call :problem "cannot remove old outputs: %~1"& exit /b 1)
mkdir "%~1"
if not exist "%~1" (call :problem "cannot create output: %~1"& exit /b 1)
exit /b 0

:run
rem Each invocation gets a NEW capture file: never read stale output or reuse a
rem file handle inherited by a background child of a native environment initializer.
set /a DEVLINK_COMMAND_INDEX+=1 >nul
set "CMDLOG=%LOG%.command-%DEVLINK_COMMAND_INDEX%.txt"
if exist "%CMDLOG%" (call :problem "refusing stale command capture: %CMDLOG%"& exit /b 1)
>"%CMDLOG%" type nul
if not exist "%CMDLOG%" (call :problem "cannot create command capture: %CMDLOG%"& exit /b 1)
>"%CMDLOG%.command" echo %*
type "%CMDLOG%.command" >>"%CMDLOG%"
rem Capture each process without a pipeline masking its status. The console only
rem shows it with --verbose, or after a failure (see run_failed).
>>"%LOG%" echo + %*
if "%VERBOSE%"=="1" echo + %*
set "RUN_TOOL=%~nx1"
rem Group the command so redirection belongs to the complete native invocation.
rem CALL sites must not append conditional operators to this variadic subroutine.
(%*) >"%CMDLOG%" 2>&1
set "RC=%errorlevel%"
if "%VERBOSE%"=="1" type "%CMDLOG%"
type "%CMDLOG%" >>"%LOG%"
if not "%errorlevel%"=="0" (call :problem "cannot preserve command log"& exit /b 1)
if "%RC%"=="0" exit /b 0
>>"%LOG%" echo Command exit %RC%: %*
if "%VERBOSE%"=="1" echo Command exit %RC%: %*
if "%GITHUB_ACTIONS%"=="true" if not "%DEVLINK_EXPECT_FAILURE%"=="1" if not "%RC%"=="%RUN_EXPECTED%" (
  echo ::notice title=DevLink command failure::exit=%RC% command=%*
  call :notice_file "%CMDLOG%"
  findstr /I /C:"error:" /C:"fatal" /C:"undefined" /C:"FAIL" "%CMDLOG%" >"%LOG%.diagnostics.txt"
  call :notice_file "%LOG%.diagnostics.txt"
)
if "%DEVLINK_EXPECT_FAILURE%"=="1" exit /b %RC%
if "%RC%"=="%RUN_EXPECTED%" exit /b %RC%
if "%RUN_QUIET%"=="1" exit /b %RC%
call :run_failed
exit /b %RC%

:run_failed
rem Ferme l'etape en echec, puis montre la sortie de la commande fautive.
call :step_fail
set "RF_ITEM="
if defined STEP_ITEM set "RF_ITEM= %G_DOT% %STEP_ITEM%"
echo     %C_ERR%%G_FAIL% %RUN_TOOL% a échoué%C0% %C_DIM%(code %RC%)%RF_ITEM%%C0%
call :print_excerpt "%CMDLOG%" 60 "%CMDLOG%"
exit /b 0

:problem
rem Echec explicite : l'etape ouverte est fermee en echec, puis le motif affiche.
if defined LOG >>"%LOG%" echo ERROR: %~1
if "%DEVLINK_QUIET_PROBLEM%"=="1" goto problem_ci
call :step_fail
echo     %C_ERR%%G_FAIL% %~1%C0%
:problem_ci
if "%GITHUB_ACTIONS%"=="true" if not "%DEVLINK_EXPECT_FAILURE%"=="1" echo ::error title=DevLink Windows validation::%~1
if not "%DEVLINK_EXPECT_FAILURE%"=="1" if defined CMDLOG call :notice_file "%CMDLOG%"
exit /b 0

:notice_file
if not "%GITHUB_ACTIONS%"=="true" exit /b 0
rem One bounded multiline annotation avoids GitHub's per-step annotation limit.
setlocal EnableDelayedExpansion
set "NOTE="
for /f "usebackq delims=" %%L in ("%~1") do (
  set "LINE=%%L"
  set "LINE=!LINE:%%=%%25!"
  set "LINE=!LINE:~0,1000!"
  set "NOTE=!NOTE!!LINE!%%0A"
  if not "!NOTE:~6500!"=="" goto notice_emit
)
:notice_emit
if not defined NOTE (endlocal& exit /b 0)
set "NOTE=!NOTE:~0,7000!"
echo ::notice title=DevLink Windows evidence::!NOTE!
endlocal
exit /b 0

:paths
rem Never discover a compiler from PATH; the selected installation is the only authority.
if defined DEVLINK_CXX (call :problem "DEVLINK_CXX is not supported on Windows. Set DEVLINK_MSYS2_ROOT instead."& exit /b 2)
if defined PLPT_CXX (call :problem "PLPT_CXX is not supported on Windows. Set DEVLINK_MSYS2_ROOT instead."& exit /b 2)
if defined DEVLINK_EXTRA_CXXFLAGS (call :problem "injected compiler flags are not permitted in the UCRT64 profile"& exit /b 2)
if defined PLPT_EXTRA_CXXFLAGS (call :problem "PLPT_EXTRA_CXXFLAGS is deprecated; injected flags remain forbidden"& exit /b 2)
if defined DEVLINK_EXTRA_LDFLAGS (call :problem "injected linker flags are not permitted in the UCRT64 profile"& exit /b 2)
if defined PLPT_EXTRA_LDFLAGS (call :problem "PLPT_EXTRA_LDFLAGS is deprecated; injected flags remain forbidden"& exit /b 2)
if defined MSYSTEM if /I not "%MSYSTEM%"=="UCRT64" (call :problem "MSYSTEM must be UCRT64 or unset in an ordinary Windows terminal"& exit /b 2)
for %%E in (CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH LIBRARY_PATH COMPILER_PATH GCC_EXEC_PREFIX) do (
  if defined %%E (call :problem "ambient %%E would alter compiler search paths. Clear it first."& exit /b 2)
)
if not defined DEVLINK_MSYS2_ROOT if defined PLPT_MSYS2_ROOT set "DEVLINK_MSYS2_ROOT=%PLPT_MSYS2_ROOT%"
if not defined DEVLINK_MSYS2_ROOT set "DEVLINK_MSYS2_ROOT=C:\msys64"
for %%I in ("%DEVLINK_MSYS2_ROOT%") do set "DEVLINK_MSYS2_ROOT=%%~fI"
set "UCRT=%DEVLINK_MSYS2_ROOT%\ucrt64"
set "UCRT_SLASH=%UCRT:\=/%"
set "CXX=%UCRT%\bin\clang++.exe"
set "LD=%UCRT%\bin\ld.exe"
set "WINDRES=%UCRT%\bin\windres.exe"
set "OBJDUMP=%UCRT%\bin\objdump.exe"
set "TAR=%UCRT%\bin\bsdtar.exe"
set "PACMAN=%DEVLINK_MSYS2_ROOT%\usr\bin\pacman.exe"
set "MSYSTEM=UCRT64"
rem Native utilities and UCRT DLLs only; no Developer Command Prompt or SDK setup.
set "PATH=%UCRT%\bin;%SystemRoot%\System32;%SystemRoot%"
set "INCLUDE="
set "LIB="
set "LIBPATH="
for %%F in ("%CXX%" "%LD%" "%WINDRES%" "%OBJDUMP%" "%TAR%" "%PACMAN%" "%SystemRoot%\System32\curl.exe" "%SystemRoot%\System32\certutil.exe") do (
  if not exist "%%~F" (call :problem "required UCRT64/native tool missing: %%~F. See docs/BUILD.md."& exit /b 2)
)
set "DRIVER=--no-default-config --target=x86_64-w64-windows-gnu"
set "LINK_DRIVER=%DRIVER% --ld-path="%LD%" -pthread"
exit /b 0

:check_search_entry
rem Relative compiler-internal entries are not filesystem selections; every entry
rem that names the selected root must still stay inside the selected root.
set "SEARCH_ENTRY=%~1"
if not defined SEARCH_ENTRY exit /b 0
>"%OUT%\doctor\search-entry.txt" echo(%SEARCH_ENTRY%
findstr /L /I /C:"%UCRT_SLASH%/" "%OUT%\doctor\search-entry.txt" >nul
if errorlevel 1 exit /b 0
call :path_in_ucrt "%SEARCH_ENTRY%"

:path_in_ucrt_path
rem Ignore the official compiler's nonexistent legacy GCC probe directories;
rem every path it actually uses must still reside in the selected UCRT64 root.
echo(%~1| findstr /I /L /C:"ignoring nonexistent directory" >nul && exit /b 0
call :path_in_ucrt "%~1"

:path_in_ucrt
set "CHECK_PATH=%~1"
set "CHECK_PATH=%CHECK_PATH:/=\%"
for %%I in ("%CHECK_PATH%") do set "CHECK_PATH=%%~fI"
set "CHECK_PATH=%CHECK_PATH:\=/%"
echo(%CHECK_PATH%| findstr /B /I /L /C:"%UCRT_SLASH%/" >nul
if errorlevel 1 (call :problem "path escapes selected UCRT64 toolchain: %CHECK_PATH%"& exit /b 2)
exit /b 0

:reject_foreign_paths
findstr /L /I /C:"Windows Kits" /C:"Microsoft Visual Studio" /C:"Program Files/LLVM" /C:"Program Files\LLVM" /C:"/mingw64/" /C:"\mingw64" /C:"/clang64/" /C:"\clang64" /C:"/usr/include" /C:"/usr/lib" /C:"windows-msvc" "%~1" >nul
if not errorlevel 1 (call :problem "foreign toolchain path/target in compiler diagnostics: %~1"& exit /b 2)
exit /b 0

:toolchain
call :paths || exit /b
if not exist "%OUT%\doctor" mkdir "%OUT%\doctor"
set "TOOLCHAIN_INFO=%LOG%.toolchain.txt"
>"%TOOLCHAIN_INFO%" echo PROFILE=MSYS2 UCRT64 root="%DEVLINK_MSYS2_ROOT%"
>>"%TOOLCHAIN_INFO%" echo compiler=%CXX%
>>"%TOOLCHAIN_INFO%" echo PATH=%PATH%
call :run "%PACMAN%" -Qqo /ucrt64/bin/clang++.exe
if errorlevel 1 exit /b
call :exact_output "%CMDLOG%" "mingw-w64-ucrt-x86_64-clang" || (call :problem "clang++.exe is not owned by the UCRT64 clang package"& exit /b 2)
for %%I in ("%CXX%") do >>"%TOOLCHAIN_INFO%" echo compiler-exact=%%~fI
call :run "%CXX%" --version
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
findstr /C:"clang version" "%CMDLOG%" >nul || (call :problem "selected executable is not clang++"& exit /b 2)
set "CLANG_VERSION="
for /f "tokens=3" %%V in ('findstr /C:"clang version" "%CMDLOG%"') do if not defined CLANG_VERSION set "CLANG_VERSION=%%V"
call :run "%CXX%" -dumpmachine
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :exact_output "%CMDLOG%" "x86_64-w64-windows-gnu" || (call :problem "expected default target x86_64-w64-windows-gnu, no target substitution"& exit /b 2)
call :run "%CXX%" -print-search-dirs
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :reject_foreign_paths "%CMDLOG%" || exit /b
set "SEARCH_DIRS="
for /f "usebackq tokens=1,* delims==" %%A in ("%CMDLOG%") do if "%%A"=="libraries: " set "SEARCH_DIRS=%%B"
if not defined SEARCH_DIRS (call :problem "cannot determine actual compiler library search paths"& exit /b 2)
for %%L in ("%SEARCH_DIRS:;=" "%") do (call :check_search_entry "%%~L" || exit /b)
call :run "%CXX%" -print-prog-name=ld
if errorlevel 1 exit /b
set "RESOLVED_LD="
for /f "usebackq delims=" %%I in ("%CMDLOG%") do set "RESOLVED_LD=%%I"
if not defined RESOLVED_LD (call :problem "compiler did not resolve a linker"& exit /b 2)
call :path_in_ucrt "%RESOLVED_LD%" || exit /b
>>"%TOOLCHAIN_INFO%" echo default-linker=%RESOLVED_LD%; explicit-linker=%LD%
call :run "%LD%" --version
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :run "%CXX%" -print-resource-dir
if errorlevel 1 exit /b
set "RESOURCE_DIR="
for /f "usebackq delims=" %%I in ("%CMDLOG%") do set "RESOURCE_DIR=%%I"
call :path_in_ucrt "%RESOURCE_DIR%" || exit /b
for %%L in (libstdc++.dll.a libucrt.a libws2_32.a libiphlpapi.a libbcrypt.a libkernel32.a libuser32.a libgdi32.a libshell32.a) do (
  call :resolved_library "%%L" || exit /b
)
for %%F in (windows.h _mingw.h stdlib.h winsock2.h iphlpapi.h bcrypt.h) do (
  if not exist "%UCRT%\include\%%F" (call :problem "required MinGW-w64 header missing: %UCRT%\include\%%F"& exit /b 2)
)
call :run "%CXX%" %DRIVER% -std=c++20 -pthread -E -v -x c++ tests/build/toolchain_probe.cpp -o NUL
if errorlevel 1 exit /b
rem The -v log lists unused legacy GCC probe directories; only real search entries are path-checked.
findstr /L /I /C:"windows-msvc" /C:"x86_64-pc-windows-msvc" "%CMDLOG%" >nul && (call :problem "MSVC target appeared in compiler diagnostics"& exit /b 2)
findstr /R /C:"^ [A-Za-z]:" "%CMDLOG%" >"%LOG%.include-paths.txt"
if errorlevel 1 (call :problem "cannot identify actual native header search paths"& exit /b 2)
for /f "usebackq tokens=*" %%I in ("%LOG%.include-paths.txt") do (call :path_in_ucrt_path "%%I" || exit /b)
call :reject_foreign_paths "%LOG%.include-paths.txt" || exit /b
type "%LOG%.include-paths.txt" >>"%TOOLCHAIN_INFO%"
call :run "%PACMAN%" -Q mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-headers mingw-w64-ucrt-x86_64-crt mingw-w64-ucrt-x86_64-libwinpthread mingw-w64-ucrt-x86_64-binutils
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :run "%CXX%" %LINK_DRIVER% -std=c++20 -Wall -Wextra -Wpedantic -Werror tests/build/toolchain_probe.cpp -o "%OUT%\doctor\probe.exe" -lbcrypt
if errorlevel 1 exit /b
if not exist "%OUT%\doctor\probe.exe" (call :problem "native probe output missing"& exit /b 1)
call :pe_header "%OUT%\doctor\probe.exe" || exit /b
call :run "%OBJDUMP%" -p "%OUT%\doctor\probe.exe"
if errorlevel 1 exit /b
findstr /C:"DLL Name:" "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
call :reject_runtime "%CMDLOG%" || exit /b
call :run "%OUT%\doctor\probe.exe" --expect-compiler "%CXX%"
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%TOOLCHAIN_INFO%"
>>"%TOOLCHAIN_INFO%" echo probe-exit-code=PASS native C++20/x64/thread/BCrypt, no MSVC headers
set "ASAN_AVAILABLE=false"
set "FUZZ_AVAILABLE=false"
set "ASAN_DLL="
set "ASAN_LIB="
set "FUZZ_LIB="
>>"%TOOLCHAIN_INFO%" echo compiler-rt-runtime-bin:
if exist "%UCRT%\bin\*clang_rt*" (dir /b "%UCRT%\bin\*clang_rt*" >>"%TOOLCHAIN_INFO%") else >>"%TOOLCHAIN_INFO%" echo none
>>"%TOOLCHAIN_INFO%" echo compiler-rt-resource-libs:
if exist "%RESOURCE_DIR%\lib\windows\*" (dir /b "%RESOURCE_DIR%\lib\windows\*" >>"%TOOLCHAIN_INFO%") else >>"%TOOLCHAIN_INFO%" echo none
for %%F in ("%UCRT%\bin\*asan_dynamic-x86_64*.dll") do set "ASAN_DLL=%%~fF"
for %%F in ("%RESOURCE_DIR%\lib\windows\*asan_dynamic-x86_64*.a") do set "ASAN_LIB=%%~fF"
for %%F in ("%RESOURCE_DIR%\lib\windows\*fuzzer-x86_64*.a") do set "FUZZ_LIB=%%~fF"
if defined ASAN_DLL if defined ASAN_LIB set "ASAN_AVAILABLE=true"
>>"%TOOLCHAIN_INFO%" echo asan-dll=%ASAN_DLL%; asan-import-lib=%ASAN_LIB%; fuzzer-lib=%FUZZ_LIB%
set "FUZZ_LINK_SAN=-fsanitize=fuzzer"
if "%ASAN_AVAILABLE%"=="true" set "FUZZ_LINK_SAN=-fsanitize=address,fuzzer"
>>"%TOOLCHAIN_INFO%" echo fuzzer-link-flags=%FUZZ_LINK_SAN%
rem Archive presence is not capability. Build and execute the real two-step
rem recipe only in doctor/fuzz operations; ordinary builds must not run an
rem expected-to-fail capability probe (some UCRT64 Clang targets reject the
rem fuzzer-no-link option even when a resource archive is installed).
set "FUZZ_PROBE=0"
if /I "%ACTION%"=="doctor" set "FUZZ_PROBE=1"
if /I "%ACTION%"=="fuzz" set "FUZZ_PROBE=1"
if "%FUZZ_PROBE%"=="0" set "FUZZ_LIB="
set "EXPECT_FAILURE_SAVED=%DEVLINK_EXPECT_FAILURE%"
set "DEVLINK_EXPECT_FAILURE=1"
set "FUZZ_REASON="
if defined FUZZ_LIB call :run "%CXX%" %LINK_DRIVER% -std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link -c tests/build/fuzzer_capability_probe.cpp -o "%OUT%\doctor\fuzzer-probe.o"
if not exist "%OUT%\doctor\fuzzer-probe.o" (set "FUZZ_LIB="& set "FUZZ_REASON=%CMDLOG%")
if defined FUZZ_LIB call :run "%CXX%" %LINK_DRIVER% -std=c++20 -O1 -g %FUZZ_LINK_SAN% "%OUT%\doctor\fuzzer-probe.o" -o "%OUT%\doctor\fuzzer-probe.exe"
if not exist "%OUT%\doctor\fuzzer-probe.exe" (set "FUZZ_LIB="& set "FUZZ_REASON=%CMDLOG%")
if defined FUZZ_LIB call :run "%OUT%\doctor\fuzzer-probe.exe" -runs=1 -seed=1 -max_len=64
if defined FUZZ_LIB if errorlevel 1 (set "FUZZ_LIB="& set "FUZZ_REASON=%CMDLOG%")
if defined FUZZ_LIB (>>"%TOOLCHAIN_INFO%" echo fuzzer-probe=PASS compiled linked and executed 1 run& type "%CMDLOG%" >>"%TOOLCHAIN_INFO%") else >>"%TOOLCHAIN_INFO%" echo fuzzer-probe=UNAVAILABLE; native fuzz stays SKIP, never PASS
set "DEVLINK_EXPECT_FAILURE=%EXPECT_FAILURE_SAVED%"
set "EXPECT_FAILURE_SAVED="
rem An unavailable capability must carry the compiler's own reason, not an assertion.
if /I "%ACTION%"=="doctor" if not defined FUZZ_LIB if defined FUZZ_REASON call :notice_file "%FUZZ_REASON%"
if defined FUZZ_LIB set "FUZZ_AVAILABLE=true"
>>"%TOOLCHAIN_INFO%" echo ASan available=%ASAN_AVAILABLE%; libFuzzer available=%FUZZ_AVAILABLE%
if "%ASAN_AVAILABLE%"=="false" >>"%LOG%" echo SKIP capability: UCRT64 ASan runtime not installed. Not a memory-test PASS.
if "%FUZZ_AVAILABLE%"=="false" if "%FUZZ_PROBE%"=="1" >>"%LOG%" echo SKIP capability: native UCRT64 libFuzzer probe did not build, link and run. Not a fuzz-test PASS.
if "%FUZZ_PROBE%"=="0" >>"%LOG%" echo NOTE capability: libFuzzer probe deferred for this non-fuzz operation.
if /I "%ACTION%"=="doctor" if defined GITHUB_OUTPUT (
  >>"%GITHUB_OUTPUT%" echo "asan=%ASAN_AVAILABLE%"
  >>"%GITHUB_OUTPUT%" echo "fuzz=%FUZZ_AVAILABLE%"
)
call :notice_file "%TOOLCHAIN_INFO%"
set "TOOLCHAIN_SUMMARY=clang %CLANG_VERSION% %G_DOT% x86_64-w64-windows-gnu"
exit /b 0

:resolved_library
call :run "%CXX%" -print-file-name=%~1
if errorlevel 1 exit /b
set "RESOLVED_LIBRARY="
for /f "usebackq delims=" %%I in ("%CMDLOG%") do set "RESOLVED_LIBRARY=%%I"
if not exist "%RESOLVED_LIBRARY%" (call :problem "UCRT64 import/runtime library not resolved: %~1"& exit /b 2)
call :path_in_ucrt "%RESOLVED_LIBRARY%" || exit /b
>>"%TOOLCHAIN_INFO%" echo %~1=%RESOLVED_LIBRARY%
exit /b 0

:pe_header
call :run "%OBJDUMP%" -f "%~1"
if errorlevel 1 exit /b
findstr /C:"architecture: i386:x86-64" "%CMDLOG%" >nul || (call :problem "not an x64 PE/COFF file: %~1"& exit /b 1)
findstr /C:"file format pe-i386" /C:"file format pei-i386" "%CMDLOG%" >nul
if not errorlevel 1 (call :problem "32-bit member in dependency: %~1"& exit /b 1)
exit /b 0

:reject_runtime
findstr /I /C:"DLL Name: msvcrt.dll" /C:"DLL Name: MSVCP" /C:"DLL Name: VCRUNTIME" /C:"DLL Name: msys-" /C:"DLL Name: libc++" "%~1" >nul
if not errorlevel 1 (call :problem "forbidden MSVCRT/MSVC/MSYS/libc++ runtime import in %~1"& exit /b 2)
exit /b 0

:exact_output
rem FOR /F handles both MSYS LF and native CRLF; FINDSTR /X is not portable between them.
set "OUTPUT_LINES=0"
set "OBSERVED_LINE="
for /f "usebackq delims=" %%I in ("%~1") do (
  set /a OUTPUT_LINES+=1 >nul
  set "OBSERVED_LINE=%%I"
)
if not defined OBSERVED_LINE exit /b 1
if not "%OUTPUT_LINES%"=="1" exit /b 1
if not "%OBSERVED_LINE%"=="%~2" exit /b 1
exit /b 0

:contains_exact_line
set "MATCHED_LINE=0"
for /f "usebackq delims=" %%I in ("%~1") do if "%%I"=="%~2" set "MATCHED_LINE=1"
if not "%MATCHED_LINE%"=="1" exit /b 1
exit /b 0

:verify
if not exist "%~1" (call :problem "missing archive %~1. Run build.bat deps --fetch or provision that exact pinned file."& exit /b 2)
call :run "%SystemRoot%\System32\certutil.exe" -hashfile "%~1" SHA256
if errorlevel 1 exit /b
findstr /I /X /C:"%~2" "%CMDLOG%" >nul
if not "%errorlevel%"=="0" (call :problem "SHA-256 mismatch for %~1. Archive NOT extracted."& exit /b 1)
>>"%LOG%" echo SHA256 OK %~2 %~1
if "%VERBOSE%"=="1" echo SHA256 OK %~2 %~1
rem Hash checks always run/log; only provisioning emits repeated CI metadata.
if "%GITHUB_ACTIONS%"=="true" if /I "%ACTION%"=="deps" if /I not "%~x1"==".part" echo ::notice title=DevLink Windows dependency::SHA256 %~2 %~nx1
exit /b 0

:dependencies
if "%CORE_ONLY%"=="1" exit /b 0
call :verify "%SDL_ARCHIVE%" "%SDL_SHA%" || exit /b
call :verify "%IMGUI_ARCHIVE%" "%IMGUI_SHA%" || exit /b
call :run "%PACMAN%" -Q mingw-w64-ucrt-x86_64-sdl3
if errorlevel 1 exit /b
call :exact_output "%CMDLOG%" "mingw-w64-ucrt-x86_64-sdl3 3.4.20-1" || (call :problem "installed SDL3 must be the pinned UCRT64 package 3.4.20-1"& exit /b 2)
set "SDL_VERIFY=%OUT%\doctor\sdl-package"
call :fresh "%SDL_VERIFY%" || exit /b
call :run "%TAR%" -xf "%SDL_ARCHIVE%" -C "%SDL_VERIFY%"
if errorlevel 1 exit /b
call :contains_exact_line "%SDL_VERIFY%\.PKGINFO" "pkgname = mingw-w64-ucrt-x86_64-sdl3" || (call :problem "wrong SDL3 package identity"& exit /b 2)
call :contains_exact_line "%SDL_VERIFY%\.PKGINFO" "pkgver = 3.4.20-1" || (call :problem "wrong SDL3 package revision"& exit /b 2)
for %%F in (lib\libSDL3.dll.a lib\libSDL3.a bin\SDL3.dll) do (
  call :run fc.exe /b "%SDL_VERIFY%\ucrt64\%%F" "%UCRT%\%%F"
  if errorlevel 1 exit /b
)
if not exist "%UCRT%\include\SDL3\SDL.h" (call :problem "installed SDL3 headers missing"& exit /b 2)
for %%F in ("%SDL_VERIFY%\ucrt64\include\SDL3\*.h") do (
  call :run fc.exe /b "%%~fF" "%UCRT%\include\SDL3\%%~nxF"
  if errorlevel 1 exit /b
)
call :pe_header "%UCRT%\lib\libSDL3.dll.a" || exit /b
call :pe_header "%UCRT%\bin\SDL3.dll" || exit /b
call :run "%OBJDUMP%" -p "%UCRT%\bin\SDL3.dll"
if errorlevel 1 exit /b
call :reject_runtime "%CMDLOG%" || exit /b
if "%GITHUB_ACTIONS%"=="true" if /I "%ACTION%"=="deps" echo ::notice title=DevLink UCRT64 dependencies::PASS installed SDL3 3.4.20-1 headers and both GNU libraries and DLL match verified package bytes
exit /b 0

:dependencies_action
set "TOTAL=3"
call :step "Récupération des archives épinglées"
call :paths || exit /b
if not exist "%DEPS%" mkdir "%DEPS%"
set "DEPS_SOURCE=archives locales"
if not exist "%SDL_ARCHIVE%" if exist "%DEVLINK_MSYS2_ROOT%\var\cache\pacman\pkg\%SDL_PACKAGE%" (
  call :run copy /y "%DEVLINK_MSYS2_ROOT%\var\cache\pacman\pkg\%SDL_PACKAGE%" "%SDL_ARCHIVE%"
  if errorlevel 1 exit /b
)
if "%FETCH%"=="1" set "DEPS_SOURCE=téléchargement HTTPS autorisé par --fetch"
if "%FETCH%"=="1" (
  call :fetch_one "https://repo.msys2.org/mingw/ucrt64/%SDL_PACKAGE%" "%SDL_ARCHIVE%" "%SDL_SHA%" || exit /b
  call :fetch_one "https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859" "%IMGUI_ARCHIVE%" "%IMGUI_SHA%" || exit /b
)
call :step_done "%DEPS_SOURCE%"
call :step "Vérification des dépendances épinglées"
call :dependencies || exit /b
if "%CORE_ONLY%"=="1" (call :step_skip "non requis avec --core-only") else call :step_done "SDL3 3.4.20-1 %G_DOT% ImGui c1aaac6 %G_DOT% SHA-256 OK"
call :step "Préparation de Dear ImGui"
call :prepare_imgui "%OUT%\doctor" || exit /b
call :step_done "sources épinglées extraites et contrôlées"
exit /b 0

:prepare_imgui
set "IMGUI_ROOT=%~1\vendor\imgui"
call :fresh "%IMGUI_ROOT%" || exit /b
call :run "%TAR%" -xzf "%IMGUI_ARCHIVE%" -C "%IMGUI_ROOT%" --strip-components=1
if errorlevel 1 exit /b
set "IMGUI_HEADERS=imgui.h imconfig.h imgui_internal.h imstb_rectpack.h imstb_textedit.h imstb_truetype.h backends/imgui_impl_sdl3.h backends/imgui_impl_sdlrenderer3.h"
for %%F in (%IMGUI_SOURCES% %IMGUI_HEADERS%) do (
  if not exist "%IMGUI_ROOT%\%%F" (call :problem "pinned ImGui input missing: %%F"& exit /b 2)
)
rem Ancien nom accepte : PLPT_IMGUI_DIR est aussi celui de .vscode/c_cpp_properties.json.
if not defined DEVLINK_IMGUI_DIR if defined PLPT_IMGUI_DIR set "DEVLINK_IMGUI_DIR=%PLPT_IMGUI_DIR%"
if not defined DEVLINK_IMGUI_DIR exit /b 0
for %%I in ("%DEVLINK_IMGUI_DIR%") do set "DEVLINK_IMGUI_DIR=%%~fI"
rem Existing owner sources are read-only inputs, not overwritten or trusted by version text alone.
for %%F in (%IMGUI_SOURCES% %IMGUI_HEADERS%) do (
  call :run fc.exe /b "%IMGUI_ROOT%\%%F" "%DEVLINK_IMGUI_DIR%\%%F"
  if errorlevel 1 exit /b
)
set "IMGUI_ROOT=%DEVLINK_IMGUI_DIR%"
>>"%LOG%" echo Using existing verified ImGui sources: %IMGUI_ROOT%
call :note_later "Sources Dear ImGui existantes utilisées après comparaison octet par octet : %IMGUI_ROOT%"
exit /b 0

:fetch_one
if exist "%~2" (call :verify "%~2" "%~3"& exit /b)
call :run "%SystemRoot%\System32\curl.exe" --fail --location --show-error --proto "=https" --tlsv1.2 --output "%~2.part" "%~1"
if errorlevel 1 exit /b
call :verify "%~2.part" "%~3" || exit /b
move /y "%~2.part" "%~2" >nul
exit /b %errorlevel%

:stamp
for /f "tokens=1-4 delims=:., " %%a in ("%TIME: =0%") do set /a "STAMP=((1%%a-100)*360000)+((1%%b-100)*6000)+((1%%c-100)*100)+(1%%d-100)" >nul
exit /b 0

:configure_flags
set "PROFILE_FLAGS=-O0 -g"
set "SAN="
if "%MODE%"=="release" set "PROFILE_FLAGS=-O2 -DNDEBUG"
if "%MODE%"=="asan" (
  set "PROFILE_FLAGS=-O1 -g"
  set "SAN=-fsanitize=address -fno-omit-frame-pointer"
)
rem Fuzzing is not conditional on ASan: libFuzzer links and runs on its own.
rem ASan is added back by the fuzz operation when this toolchain really has it.
if "%MODE%"=="fuzz" (
  set "PROFILE_FLAGS=-O1 -g"
  set "SAN=-fno-omit-frame-pointer"
)
set "VFLAG="
if "%VERBOSE%"=="1" set "VFLAG=-v"
set "COMMON_FLAGS=%DRIVER% -std=c++20 -pthread -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00"
set "WARNINGS=-Wall -Wextra -Wpedantic -Werror -Wshadow"
set "NATIVE_LIBS=-lws2_32 -liphlpapi -lbcrypt -lkernel32 -luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -lwindowscodecs -luuid"
set "NEGATIVE_CXX="
set "NEGATIVE_LINK="
if "%DEVLINK_EXPECT_FAILURE%"=="1" if "%DEVLINK_SELFTEST_FAIL%"=="compile" set "NEGATIVE_CXX=-fdevlink-deliberate-invalid-option"
if "%DEVLINK_EXPECT_FAILURE%"=="1" if "%DEVLINK_SELFTEST_FAIL%"=="link" set "NEGATIVE_LINK=-Wl,--devlink-deliberate-invalid-option"
exit /b 0

:cpp
set "SOURCE=%~1"
set "OBJECT=%DIR%\objects\%SOURCE:/=_%.o"
for %%F in ("%SOURCE%") do set "STEP_ITEM=%%~nxF"
set /a UNIT_INDEX+=1 >nul
call :step_progress %UNIT_INDEX% %UNIT_TOTAL% "%STEP_ITEM%"
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %SAN% %WARNINGS% %NEGATIVE_CXX% -I "%ROOT%" %UI_INCLUDES% -MMD -MP -MF "%OBJECT%.d" -c "%SOURCE%" -o "%OBJECT%"
if errorlevel 1 exit /b
if not exist "%OBJECT%" (call :problem "compiler returned success without object %OBJECT%"& exit /b 1)
exit /b 0

:core_one
call :cpp "%~1" || exit /b
set "CORE_OBJECTS=%CORE_OBJECTS% "%OBJECT%""
exit /b 0
:model_one
call :cpp "%~1" || exit /b
set "MODEL_OBJECTS=%MODEL_OBJECTS% "%OBJECT%""
exit /b 0
:net_one
call :cpp "%~1" || exit /b
set "NET_OBJECTS=%NET_OBJECTS% "%OBJECT%""
exit /b 0
:browser_one
call :cpp "%~1" || exit /b
set "BROWSER_OBJECTS=%BROWSER_OBJECTS% "%OBJECT%""
exit /b 0
:ui_one
call :cpp "%~1" || exit /b
set "UI_OBJECTS=%UI_OBJECTS% "%OBJECT%""
exit /b 0
:gui_one
set "GUI_SOURCE=%~1"
set "GUI_OBJECT=%DIR%\objects\imgui_%GUI_SOURCE:/=_%.o"
for %%F in ("%GUI_SOURCE%") do set "STEP_ITEM=%%~nxF"
set /a UNIT_INDEX+=1 >nul
call :step_progress %UNIT_INDEX% %UNIT_TOTAL% "%STEP_ITEM%"
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %SAN% -isystem "%IMGUI_ROOT%" -MMD -MP -MF "%GUI_OBJECT%.d" -c "%IMGUI_ROOT%\%GUI_SOURCE%" -o "%GUI_OBJECT%"
if errorlevel 1 exit /b
if not exist "%GUI_OBJECT%" (call :problem "missing ImGui object %GUI_OBJECT%"& exit /b 1)
set "GUI_OBJECTS=%GUI_OBJECTS% "%GUI_OBJECT%""
exit /b 0
:test_one
call :cpp "%~1" || exit /b
for %%F in ("%~1") do set "TEST_OBJ_%%~nF=%OBJECT%"
exit /b 0

:link
rem All links use the same clang++ driver and GNU ld from this installation.
rem %1 = executable, %2 = CONSOLE|WINDOWS. PE checks run in a separate step.
if not exist "%OUT%\doctor\probe.exe" call :toolchain || exit /b
set "STEP_ITEM=%~1.exe"
set /a UNIT_INDEX+=1 >nul
call :step_progress %UNIT_INDEX% %UNIT_TOTAL% "%STEP_ITEM%"
set "SUBSYSTEM=-mconsole"
rem Windows-subsystem binaries define UNICODE, so SDL3's header-only entry
rem implementation provides wWinMain(), not ANSI WinMain(). The MinGW CRT
rem startup only calls wWinMain when -municode selects crtexewin_w.o; plain
rem -mwindows selects crtexewin.o and dies with "undefined reference to WinMain".
rem SDL's own SDL_RunApp then rebuilds a real UTF-8 argc/argv from the command
rem line, so argument handling stays identical for the smoke tests.
if /I "%~2"=="WINDOWS" set "SUBSYSTEM=-mwindows -municode"
call :run "%CXX%" %VFLAG% %LINK_DRIVER% %PROFILE_FLAGS% %SAN% %LINK_OBJECTS% "%RESOURCE_O%" %NATIVE_LIBS% %LINK_EXTRA% %SUBSYSTEM% -Wl,--no-undefined,--fatal-warnings,--dynamicbase,--nxcompat %NEGATIVE_LINK% -o "%BIN%\%~1.exe"
if errorlevel 1 exit /b
if not exist "%BIN%\%~1.exe" (call :problem "linker returned success without executable"& exit /b 1)
set "LINKED=%LINKED% %~1"
exit /b 0

:pe_manifest_one
set "STEP_ITEM=%~1.exe"
set /a UNIT_INDEX+=1 >nul
call :step_progress %UNIT_INDEX% %UNIT_TOTAL% "%STEP_ITEM%"
call :run "%OUT%\doctor\probe.exe" --manifest "%BIN%\%~1.exe" --expect-version %VERSION_QUAD%
if errorlevel 1 exit /b
exit /b 0

:sanitizer_runtime
if not "%MODE%"=="asan" exit /b 0
if "%ASAN_AVAILABLE%"=="false" (call :note_later "Runtime ASan UCRT64 absent : profil asan non exécuté, SKIP code 77, jamais compté comme réussi."& exit /b 77)
exit /b 0

:product_identity
rem Version publique unique (mandat 16) : lue depuis engine/core/product.hpp,
rem puis quadruplet Windows derive. Aucune autre copie de la version.
set "PRODUCT_VERSION="
set "VERSION_QUAD="
set "PVI_LINE="
for /f "usebackq delims=" %%L in (`findstr /C:"kProductVersion = " "%ROOT%\engine\core\product.hpp"`) do set "PVI_LINE=%%L"
if not defined PVI_LINE (call :problem "kProductVersion introuvable dans engine/core/product.hpp"& exit /b 1)
call set "PVI_LINE=%%PVI_LINE:*kProductVersion =%%"
call set "PVI_LINE=%%PVI_LINE:~2%%"
for /f "delims=;" %%A in ("%PVI_LINE%") do set "PRODUCT_VERSION=%%A"
set "PRODUCT_VERSION=%PRODUCT_VERSION:"=%"
for /f "tokens=1,2,3 delims=.-" %%A in ("%PRODUCT_VERSION%") do set "VERSION_QUAD=%%A.%%B.%%C.0"
if not defined PRODUCT_VERSION (call :problem "version produit vide"& exit /b 1)
if not defined VERSION_QUAD (call :problem "quadruplet de version impossible a deriver"& exit /b 1)
exit /b 0

:render_manifest
rem Rend le gabarit de manifeste avec le quadruplet calcule : le fichier rendu
rem part dans out/, le gabarit versionne reste la seule source editee a la main.
setlocal EnableDelayedExpansion
if exist "%DIR%\app.manifest" del "%DIR%\app.manifest"
for /f "usebackq delims=" %%L in ("%ROOT%\packaging\windows\app.manifest") do (
  set "MLINE=%%L"
  set "MLINE=!MLINE:@DEVLINK_VERSION_QUAD@=%VERSION_QUAD%!"
  >>"%DIR%\app.manifest" echo !MLINE!
)
endlocal
if not exist "%DIR%\app.manifest" (call :problem "manifeste rendu manquant"& exit /b 1)
exit /b 0

:render_resources
rem Rend le gabarit de ressources PE (icone + VERSIONINFO + manifeste) pour le
rem binaire applicatif devlink-desktop : une ressource unique et tracable par
rem binaire (mandat 15). monitor et demo-service gardent l'objet manifeste seul.
setlocal EnableDelayedExpansion
set "VC=%VERSION_QUAD:.=,%"
if exist "%DIR%\desktop_application.rc" del "%DIR%\desktop_application.rc"
for /f "usebackq delims=" %%L in ("%ROOT%\packaging\windows\application.rc.in") do (
  set "RLINE=%%L"
  set "RLINE=!RLINE:@DEVLINK_MANIFEST@=%DIR:\=/%/app.manifest!"
  set "RLINE=!RLINE:@DEVLINK_ICO@=%ROOT:\=/%/assets/branding/devlink.ico!"
  set "RLINE=!RLINE:@DEVLINK_VERSION_COMMA@=%VC%!"
  set "RLINE=!RLINE:@DEVLINK_PRODUCT_VERSION@=%PRODUCT_VERSION%!"
  set "RLINE=!RLINE:@DEVLINK_FILE_DESCRIPTION@=DevLink desktop control post!"
  set "RLINE=!RLINE:@DEVLINK_INTERNAL_NAME@=devlink-desktop!"
  set "RLINE=!RLINE:@DEVLINK_ORIGINAL_FILENAME@=devlink-desktop.exe!"
  >>"%DIR%\desktop_application.rc" echo !RLINE!
)
endlocal
if not exist "%DIR%\desktop_application.rc" (call :problem "ressources PE rendues manquantes"& exit /b 1)
exit /b 0

:build_project
rem Phases reelles, dans l'ordre : chaine, dependances, preparation, compilation
rem (moteur, applications, tests), edition des liens, controles PE, en-tetes,
rem fermeture des DLL. Toute la compilation precede toute edition des liens.
call :stamp
set "START_STAMP=%STAMP%"
set "ENGINE_FULL=0"
if "%CORE_ONLY%"=="0" set "ENGINE_FULL=1"
if "%WITH_TESTS%"=="1" set "ENGINE_FULL=1"
rem Remove old outputs before validation, so a failed attempt cannot reuse a prior binary.
call :fresh "%DIR%" || exit /b
set "BIN=%DIR%\bin"
mkdir "%BIN%" "%DIR%\objects"
if not exist "%DIR%\objects" exit /b 1
rem Objet de ressources par defaut : manifeste seul (monitor, demo-service).
set "RESOURCE_O=%DIR%\application.o"

call :step "Contrôle de la chaîne de compilation UCRT64"
call :toolchain || exit /b
call :step_done "%TOOLCHAIN_SUMMARY%"

call :step "Vérification des dépendances épinglées"
call :dependencies || exit /b
if "%CORE_ONLY%"=="1" (call :step_skip "non requis avec --core-only") else call :step_done "SDL3 3.4.20-1 %G_DOT% ImGui c1aaac6 %G_DOT% SHA-256 OK"

call :step "Préparation des sources et ressources Windows"
call :configure_flags || exit /b
call :sanitizer_runtime || exit /b
if "%CORE_ONLY%"=="0" call :prepare_imgui "%DIR%" || exit /b
call :product_identity || exit /b
call :render_manifest || exit /b
if "%GITHUB_ACTIONS%"=="true" call :notice_file "%DIR%\app.manifest"
rem L'icone versionnee est controlee AVANT compilation des ressources : un .ico
rem renomme ou incomplet fait echouer le build, il ne peut pas passer en silence.
call :run "%OUT%\doctor\probe.exe" --ico "%ROOT%\assets\branding\devlink.ico" --expect-entries 7
if errorlevel 1 exit /b
>"%DIR%\application.rc" echo 1 24 "%DIR:\=/%/app.manifest"
call :run "%WINDRES%" --target=pe-x86-64 --input-format=rc --output-format=coff --preprocessor="%CXX%" --preprocessor-arg=--no-default-config --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED -i "%DIR%\application.rc" -o "%DIR%\application.o"
if errorlevel 1 exit /b
if not exist "%DIR%\application.o" (call :problem "resource compiler did not create manifest resource"& exit /b 1)
if "%CORE_ONLY%"=="1" goto prepare_done
call :render_resources || exit /b
call :run "%WINDRES%" --target=pe-x86-64 --input-format=rc --output-format=coff --preprocessor="%CXX%" --preprocessor-arg=--no-default-config --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED -i "%DIR%\desktop_application.rc" -o "%DIR%\desktop_application.o"
if errorlevel 1 exit /b
if not exist "%DIR%\desktop_application.o" (call :problem "resource compiler did not create desktop resources"& exit /b 1)
call :run copy /y "%UCRT%\bin\SDL3.dll" "%BIN%\SDL3.dll"
if errorlevel 1 exit /b
:prepare_done
>>"%LOG%" echo Compilation started: %MODE% - deterministic fresh objects, no cache
call :step_done "%MODE% %G_DOT% version %VERSION_QUAD% %G_DOT% icône 7 tailles"

set "CORE_OBJECTS="
set "MODEL_OBJECTS="
set "NET_OBJECTS="
set "GUI_OBJECTS="
set "BROWSER_OBJECTS="
set "UI_OBJECTS="
set "UI_INCLUDES="
set "LINK_EXTRA="
set "LINKED="

call :step "Compilation du moteur DevLink"
call :count_words N_CORE %CORE_SOURCES%
call :count_words N_MODEL %MODEL_SOURCES%
call :count_words N_NET %NET_SOURCES%
call :count_words N_BROWSER %BROWSER_SOURCES%
set /a "UNIT_TOTAL=N_CORE" >nul
if "%ENGINE_FULL%"=="1" set /a "UNIT_TOTAL=N_CORE+N_MODEL+N_NET+N_BROWSER" >nul
for %%S in (%CORE_SOURCES%) do (call :core_one "%%S" || exit /b)
rem Models, networking and the browser engine are compiled once: the desktop
rem control post and the tests both need them, the CLI monitor links without them.
if "%ENGINE_FULL%"=="1" (for %%S in (%MODEL_SOURCES%) do (call :model_one "%%S" || exit /b))
if "%ENGINE_FULL%"=="1" (for %%S in (%NET_SOURCES%) do (call :net_one "%%S" || exit /b))
if "%ENGINE_FULL%"=="1" (for %%S in (%BROWSER_SOURCES%) do (call :browser_one "%%S" || exit /b))
call :step_done "%UNIT_TOTAL% fichiers"

call :step "Compilation des applications et de l'interface"
call :count_words N_IMGUI %IMGUI_SOURCES%
call :count_words N_UI %UI_SOURCES%
set /a "UNIT_TOTAL=2" >nul
if "%CORE_ONLY%"=="0" set /a "UNIT_TOTAL=2+N_IMGUI+N_UI+1" >nul
call :cpp app/host_shell/main.cpp || exit /b
set "HOST_SHELL_O=%OBJECT%"
call :cpp app/demo_service/main.cpp || exit /b
set "DEMO_O=%OBJECT%"
if "%CORE_ONLY%"=="1" goto apps_done
for %%S in (%IMGUI_SOURCES%) do (call :gui_one "%%S" || exit /b)
set "UI_INCLUDES=-isystem "%IMGUI_ROOT%" -isystem "%IMGUI_ROOT%\backends""
for %%S in (%UI_SOURCES%) do (call :ui_one "%%S" || exit /b)
call :cpp app/desktop_ui/main.cpp || exit /b
set "DESKTOP_O=%OBJECT%"
set "UI_INCLUDES="
:apps_done
call :step_done "%UNIT_TOTAL% fichiers"

if not "%WITH_TESTS%"=="1" goto link_phase
call :step "Compilation des suites de tests"
call :count_words UNIT_TOTAL %TEST_SOURCES%
for %%S in (%TEST_SOURCES%) do (call :test_one "%%S" || exit /b)
call :step_done "%UNIT_TOTAL% suites"

:link_phase
call :step "Édition des liens"
set /a "UNIT_TOTAL=2" >nul
if "%CORE_ONLY%"=="0" set /a "UNIT_TOTAL+=1" >nul
if "%WITH_TESTS%"=="1" set /a "UNIT_TOTAL+=5" >nul
set "LINK_OBJECTS=%CORE_OBJECTS% "%HOST_SHELL_O%""
call :link devlink-monitor CONSOLE || exit /b
set "LINK_OBJECTS=%CORE_OBJECTS% "%DEMO_O%""
call :link devlink-demo-service CONSOLE || exit /b
if "%CORE_ONLY%"=="1" goto link_tests
set "LINK_OBJECTS=%CORE_OBJECTS% %MODEL_OBJECTS% %NET_OBJECTS% %BROWSER_OBJECTS% %GUI_OBJECTS% %UI_OBJECTS% "%DESKTOP_O%""
set "RESOURCE_O=%DIR%\desktop_application.o"
set "LINK_EXTRA="%UCRT%\lib\libSDL3.dll.a""
call :link devlink-desktop WINDOWS || exit /b
set "LINK_EXTRA="
set "RESOURCE_O=%DIR%\application.o"
:link_tests
if not "%WITH_TESTS%"=="1" goto link_done
rem %MODEL_OBJECTS% and %NET_OBJECTS% were built during compilation; no rebuild.
set "TEST_LINK_BASE=%CORE_OBJECTS% %MODEL_OBJECTS% %NET_OBJECTS% %BROWSER_OBJECTS%"
set "LINK_OBJECTS=%TEST_LINK_BASE% "%TEST_OBJ_test_main%""
call :link devlink-unit-tests CONSOLE || exit /b
set "LINK_OBJECTS=%TEST_LINK_BASE% "%TEST_OBJ_platform_tests%""
call :link devlink-platform-tests CONSOLE || exit /b
set "LINK_OBJECTS=%TEST_LINK_BASE% "%TEST_OBJ_browser_pipeline_tests%""
call :link devlink-browser-tests CONSOLE || exit /b
set "LINK_OBJECTS=%TEST_LINK_BASE% "%TEST_OBJ_share_pipeline_tests%""
call :link devlink-share-tests CONSOLE || exit /b
set "LINK_OBJECTS=%TEST_LINK_BASE% "%TEST_OBJ_desktop_e2e_tests%""
call :link devlink-desktop-e2e-tests CONSOLE || exit /b
:link_done
call :step_done "%UNIT_TOTAL% exécutables"

call :step "Contrôle des binaires PE"
call :count_words UNIT_TOTAL %LINKED%
set "PE_DETAIL=manifeste %G_DOT% version %VERSION_QUAD%"
for %%E in (%LINKED%) do (call :pe_manifest_one "%%E" || exit /b)
if "%CORE_ONLY%"=="1" goto pe_done
rem Le binaire applicatif doit porter reellement l'icone et la version : la
rem sonde lit les ressources PE (RT_GROUP_ICON + 7 RT_ICON), pas le fichier .ico.
call :run "%OUT%\doctor\probe.exe" --pe-icons "%BIN%\devlink-desktop.exe" --expect-icons 7
if errorlevel 1 exit /b
set "PE_DETAIL=%PE_DETAIL% %G_DOT% 7 icônes"
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink UCRT64::PASS SDL3 GNU import library and embedded asInvoker/PerMonitorV2 manifest
:pe_done
call :step_done "%PE_DETAIL%"

if not "%WITH_TESTS%"=="1" goto runtime_phase
call :step "Contrôle d'indépendance des en-têtes"
call :count_words N_HEADERS %HEADERS%
call :count_words N_UI_HEADERS %UI_HEADERS%
set /a "UNIT_TOTAL=N_HEADERS" >nul
if "%CORE_ONLY%"=="0" set /a "UNIT_TOTAL=N_HEADERS+N_UI_HEADERS" >nul
set "HEADER_COUNT=0"
for %%H in (%HEADERS%) do (call :header_one "%%H" || exit /b)
>>"%LOG%" echo PASS %HEADER_COUNT% independent native headers
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows::PASS %HEADER_COUNT% independent native headers
rem Sondes d'independance des en-tetes d'interface : ImGui doit etre extrait.
set "UI_HEADER_COUNT=0"
set "UI_INCLUDES=-isystem "%IMGUI_ROOT%" -isystem "%IMGUI_ROOT%\backends""
if "%CORE_ONLY%"=="0" for %%H in (%UI_HEADERS%) do (call :ui_header_one "%%H" || exit /b)
set "UI_INCLUDES="
if "%CORE_ONLY%"=="0" >>"%LOG%" echo PASS %UI_HEADER_COUNT% independent UI headers
if "%CORE_ONLY%"=="0" if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows::PASS %UI_HEADER_COUNT% independent UI headers
call :step_done "%HEADER_COUNT% moteur + %UI_HEADER_COUNT% interface"

:runtime_phase
call :step "Assemblage des DLL d'exécution et des licences"
call :runtime_dependencies || exit /b
set "N_DLL=0"
for %%F in ("%BIN%\*.dll") do set /a N_DLL+=1 >nul
set "N_LIC=0"
for /d %%D in ("%DIR%\runtime-licenses\*") do set /a N_LIC+=1 >nul
call :step_done "%N_DLL% DLL %G_DOT% %N_LIC% paquets de licences"

call :stamp
set /a "ELAPSED=STAMP-START_STAMP" >nul
if %ELAPSED% LSS 0 set /a "ELAPSED+=8640000" >nul
>>"%LOG%" echo BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED%
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows::BUILD SUCCESS mode=%MODE% elapsed_centiseconds=%ELAPSED% core_only=%CORE_ONLY%
>"%DIR%\BUILD_SUCCEEDED" echo mode=%MODE% compiler=%CXX%
exit /b 0

:runtime_dependencies
rem Resolve the complete PE import closure, not just SDL's direct DLL list.
set "RUNTIME_REPORT=%DIR%\runtime-dependencies.txt"
>"%RUNTIME_REPORT%" echo Native UCRT64 PE dependency closure; source root=%UCRT%
call :fresh "%DIR%\runtime-seen" || exit /b
call :fresh "%DIR%\runtime-licenses" || exit /b
rem DLLs loaded at run time (LoadLibraryExW, application directory only) are not
rem in any import table: seed them so they follow the same pinned UCRT64
rem provenance, SHA-256 and licence closure as linked imports (ADR-018/ADR-016).
for %%D in (WebView2Loader.dll libssl-3-x64.dll libcrypto-3-x64.dll) do (call :resolve_runtime_dll "%%D" || exit /b)
set "RUNTIME_ROUND=0"
:runtime_round
set /a RUNTIME_ROUND+=1 >nul
if %RUNTIME_ROUND% GTR 32 (call :problem "runtime dependency closure exceeded 32 rounds"& exit /b 1)
set "RUNTIME_NEW=0"
for %%F in ("%BIN%\*.exe" "%BIN%\*.dll") do (
  call :audit_runtime_file "%%~fF" || exit /b
)
if "%RUNTIME_NEW%"=="1" goto runtime_round
call :notice_file "%RUNTIME_REPORT%"
exit /b 0

:audit_runtime_file
set "PE_FILE=%~1"
set "PE_NAME=%~nx1"
if exist "%DIR%\runtime-seen\%PE_NAME%" exit /b 0
set "STEP_ITEM=%PE_NAME%"
call :step_progress_text "%PE_NAME% %G_DOT% tour %RUNTIME_ROUND%"
call :pe_header "%PE_FILE%" || exit /b
call :run "%OBJDUMP%" -p "%PE_FILE%"
if errorlevel 1 exit /b
call :reject_runtime "%CMDLOG%" || exit /b
findstr /C:"DLL Name:" "%CMDLOG%" >"%DIR%\runtime-seen\%PE_NAME%.imports"
if errorlevel 1 (call :problem "cannot inspect imports for %PE_FILE%"& exit /b 1)
>>"%RUNTIME_REPORT%" echo PE=%PE_FILE%
type "%DIR%\runtime-seen\%PE_NAME%.imports" >>"%RUNTIME_REPORT%"
for /f "usebackq tokens=3" %%D in ("%DIR%\runtime-seen\%PE_NAME%.imports") do (
  call :resolve_runtime_dll "%%D" || exit /b
)
>"%DIR%\runtime-seen\%PE_NAME%" echo checked
exit /b 0

:resolve_runtime_dll
set "DLL_NAME=%~1"
if /I "%DLL_NAME:~0,11%"=="api-ms-win-" exit /b 0
if /I "%DLL_NAME:~0,11%"=="ext-ms-win-" exit /b 0
rem OS libraries are dependencies, never copied from a developer SDK or redistributed here.
for %%S in (KERNEL32.dll KERNELBASE.dll ntdll.dll USER32.dll GDI32.dll ADVAPI32.dll SHELL32.dll ole32.dll OLEAUT32.dll WindowsCodecs.dll bcrypt.dll bcryptprimitives.dll WS2_32.dll IPHLPAPI.dll ucrtbase.dll IMM32.dll SETUPAPI.dll VERSION.dll WINMM.dll SHLWAPI.dll DWMAPI.dll CFGMGR32.dll COMDLG32.dll COMBASE.dll USERENV.dll SECUR32.dll CRYPT32.dll SHCORE.dll POWRPROF.dll WINSPOOL.DRV) do if /I "%DLL_NAME%"=="%%S" exit /b 0
if exist "%DIR%\runtime-seen\%DLL_NAME%.provenance" exit /b 0
if not exist "%UCRT%\bin\%DLL_NAME%" (call :problem "unresolved non-system DLL: %DLL_NAME%. Only selected UCRT64 bin is permitted."& exit /b 2)
call :run "%PACMAN%" -Qqo /ucrt64/bin/%DLL_NAME%
if errorlevel 1 exit /b
set "DLL_OWNER="
for /f "usebackq delims=" %%P in ("%CMDLOG%") do set "DLL_OWNER=%%P"
echo(%DLL_OWNER%| findstr /B /L /C:"mingw-w64-ucrt-x86_64-" >nul
if errorlevel 1 (call :problem "non-UCRT64 runtime owner for %DLL_NAME%: %DLL_OWNER%"& exit /b 2)
call :run "%PACMAN%" -Q "%DLL_OWNER%"
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%RUNTIME_REPORT%"
call :run "%SystemRoot%\System32\certutil.exe" -hashfile "%UCRT%\bin\%DLL_NAME%" SHA256
if errorlevel 1 exit /b
type "%CMDLOG%" >>"%RUNTIME_REPORT%"
call :run copy /y "%UCRT%\bin\%DLL_NAME%" "%BIN%\%DLL_NAME%"
if errorlevel 1 exit /b
call :run fc.exe /b "%UCRT%\bin\%DLL_NAME%" "%BIN%\%DLL_NAME%"
if errorlevel 1 exit /b
call :runtime_license "%DLL_OWNER%" || exit /b
>"%DIR%\runtime-seen\%DLL_NAME%.provenance" echo %DLL_OWNER%
set "RUNTIME_NEW=1"
exit /b 0

:runtime_license
set "LICENSE_OWNER=%~1"
if exist "%DIR%\runtime-licenses\%LICENSE_OWNER%\PROVENANCE.txt" exit /b 0
mkdir "%DIR%\runtime-licenses\%LICENSE_OWNER%"
call :run "%PACMAN%" -Qi "%LICENSE_OWNER%"
if errorlevel 1 exit /b
copy /y "%CMDLOG%" "%DIR%\runtime-licenses\%LICENSE_OWNER%\PROVENANCE.txt" >nul || exit /b
call :run "%PACMAN%" -Ql "%LICENSE_OWNER%"
if errorlevel 1 exit /b
findstr /C:"/share/licenses/" "%CMDLOG%" >"%DIR%\runtime-seen\%LICENSE_OWNER%.licenses"
if errorlevel 1 (call :problem "runtime package has no recorded license files: %LICENSE_OWNER%"& exit /b 2)
set "LICENSE_COUNT=0"
for /f "usebackq tokens=2" %%F in ("%DIR%\runtime-seen\%LICENSE_OWNER%.licenses") do (
  call :copy_runtime_license "%%F" || exit /b
)
if "%LICENSE_COUNT%"=="0" (call :problem "no runtime notice copied for %LICENSE_OWNER%"& exit /b 2)
exit /b 0

:copy_runtime_license
set "LICENSE_SOURCE=%~1"
set "LICENSE_SOURCE=%DEVLINK_MSYS2_ROOT%%LICENSE_SOURCE:/=\%"
if exist "%LICENSE_SOURCE%\" exit /b 0
if not exist "%LICENSE_SOURCE%" (call :problem "runtime notice missing: %LICENSE_SOURCE%"& exit /b 2)
for %%F in ("%LICENSE_SOURCE%") do set "LICENSE_FILE=%%~nxF"
call :run copy /y "%LICENSE_SOURCE%" "%DIR%\runtime-licenses\%LICENSE_OWNER%\%LICENSE_FILE%"
if errorlevel 1 exit /b
set /a LICENSE_COUNT+=1 >nul
exit /b 0

:ui_header_one
>"%DIR%\ui-header-probe.cpp" echo #include "%~1"
for %%F in ("%~1") do set "STEP_ITEM=%%~nxF"
set /a UNIT_INDEX+=1 >nul
call :step_progress %UNIT_INDEX% %UNIT_TOTAL% "%STEP_ITEM%"
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %WARNINGS% -I "%ROOT%" %UI_INCLUDES% -fsyntax-only "%DIR%\ui-header-probe.cpp"
if errorlevel 1 exit /b
set /a UI_HEADER_COUNT+=1 >nul
exit /b 0

:header_one
>"%DIR%\header-probe.cpp" echo #include "%~1"
for %%F in ("%~1") do set "STEP_ITEM=%%~nxF"
set /a UNIT_INDEX+=1 >nul
call :step_progress %UNIT_INDEX% %UNIT_TOTAL% "%STEP_ITEM%"
call :run "%CXX%" %VFLAG% %COMMON_FLAGS% %PROFILE_FLAGS% %WARNINGS% -I "%ROOT%" -fsyntax-only "%DIR%\header-probe.cpp"
if errorlevel 1 exit /b
set /a HEADER_COUNT+=1 >nul
exit /b 0

:expect
rem The command is stored in TEST_COMMAND; no shell pipeline around the child.
set "CASE_NAME=%~1"
set "EXPECTED=%~2"
set "PATTERN=%~3"
set "STEP_ITEM=%CASE_NAME%"
set /a UNIT_INDEX+=1 >nul
call :step_progress %UNIT_INDEX% %UNIT_TOTAL% "%CASE_NAME%"
>>"%LOG%" echo Test started: %CASE_NAME%, expected exit=%EXPECTED%
set "RUN_EXPECTED=%EXPECTED%"
set "RUN_QUIET=1"
call :run %TEST_COMMAND%
set "TEST_RC=%errorlevel%"
set "RUN_QUIET="
set "RUN_EXPECTED="
copy /y "%CMDLOG%" "%DIR%\%CASE_NAME%.log" >nul
set "SKIP_REASON="
if "%TEST_RC%"=="77" if "%CASE_NAME%"=="ipv6" set "SKIP_REASON=IPv6 indisponible sur cette machine"
if "%TEST_RC%"=="77" if "%CASE_NAME%"=="browser" set "SKIP_REASON=runtime WebView2 indisponible"
if "%TEST_RC%"=="77" if "%CASE_NAME%"=="share" set "SKIP_REASON=runtime WebView2 indisponible"
if "%TEST_RC%"=="77" if "%CASE_NAME%"=="desktop-e2e" set "SKIP_REASON=runtime WebView2 indisponible"
if defined SKIP_REASON goto expect_skip
if not "%TEST_RC%"=="%EXPECTED%" goto expect_bad_exit
findstr /I /R /C:"%PATTERN%" "%CMDLOG%" >nul || goto expect_bad_output
findstr /I /C:"AddressSanitizer" /C:"LeakSanitizer" /C:"UndefinedBehaviorSanitizer" /C:"runtime error:" "%CMDLOG%" >nul
if "%errorlevel%"=="0" goto expect_sanitizer
set /a PASS+=1 >nul
>>"%LOG%" echo PASS %CASE_NAME% exit=%TEST_RC%
>>"%DIR%\test-summary.txt" echo PASS %CASE_NAME% exit=%TEST_RC% mode=%MODE%
findstr /C:"desktop-runtime:" /C:"cases," "%CMDLOG%" >>"%DIR%\test-summary.txt"
if /I "%CASE_NAME:~0,4%"=="cli-" (set /a CLI_PASS+=1 >nul& exit /b 0)
if /I "%CASE_NAME:~0,3%"=="ui-" (set /a UI_PASS+=1 >nul& exit /b 0)
rem Bilan du harnais "suite: N cases, M checks, 0 failures" : passe par un
rem fichier, cmd ne reanalyse ainsi aucune virgule ni aucun motif. L'espace doit
rem etre le DERNIER delimiteur : "delims=: ," fait echouer l'analyse de cmd.
set "ROW_DETAIL=code %TEST_RC% attendu"
findstr /R /C:" cases, [0-9]* checks, " "%CMDLOG%" >"%CMDLOG%.totals" 2>nul
for /f "usebackq tokens=2,4 delims=:, " %%A in ("%CMDLOG%.totals") do set "ROW_DETAIL=%%A cas %G_DOT% %%B vérifications %G_DOT% 0 échec"
>>"%TEST_ROWS%" echo ok^|%CASE_NAME%^|%ROW_DETAIL%
exit /b 0
:expect_skip
set /a SKIP+=1 >nul
>>"%LOG%" echo SKIP %CASE_NAME%: %SKIP_REASON%. NOT counted as passed.
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows::SKIP %CASE_NAME% - qualification incomplete
>>"%TEST_ROWS%" echo skip^|%CASE_NAME%^|%SKIP_REASON% : SKIP code 77, jamais compté réussi
exit /b 0
:expect_bad_exit
call :step_fail
echo     %C_ERR%%G_FAIL% Test %CASE_NAME% : code %TEST_RC%, attendu %EXPECTED%%C0%
>>"%LOG%" echo FAIL %CASE_NAME% expected=%EXPECTED% actual=%TEST_RC% command=%TEST_COMMAND%
call :test_excerpt
if "%TEST_RC%"=="0" exit /b 1
exit /b %TEST_RC%
:expect_bad_output
call :step_fail
echo     %C_ERR%%G_FAIL% Test %CASE_NAME% : sortie attendue absente%C0% %C_DIM%%PATTERN%%C0%
>>"%LOG%" echo FAIL %CASE_NAME% missing expected output %PATTERN%
call :test_excerpt
exit /b 1
:expect_sanitizer
call :step_fail
echo     %C_ERR%%G_FAIL% Test %CASE_NAME% : diagnostic de sanitizer, malgré un code de sortie conforme%C0%
>>"%LOG%" echo FAIL %CASE_NAME% sanitizer diagnostic, even though exit matched
call :test_excerpt
exit /b 1

:test_excerpt
rem Lignes utiles d'une suite en echec : assertions, exceptions, preuves, bilan.
rem Sensible a la casse : "[PASS] ..._fail_closed" ne doit pas remonter.
findstr /C:"FAIL" /C:"EXCEPTION" /C:"error:" /C:"EVIDENCE" /C:"SKIP" /C:"cases," /C:"Usage:" /C:"Sanitizer" "%CMDLOG%" >"%CMDLOG%.excerpt" 2>nul
call :print_excerpt "%CMDLOG%.excerpt" 30 "%CMDLOG%"
exit /b 0

:tests
call :step "Exécution des tests"
>"%DIR%\test-summary.txt" echo Native Windows tests mode=%MODE%
set "TEST_ROWS=%DIR%\.console-test-rows.txt"
if exist "%TEST_ROWS%" del "%TEST_ROWS%"
set "PASS=0"
set "SKIP=0"
set "CLI_PASS=0"
set "UI_PASS=0"
rem 5 suites moteur + 5 controles CLI ; + e2e et 6 controles UI (+1 fenetre native).
set "UNIT_TOTAL=10"
if "%CORE_ONLY%"=="0" set "UNIT_TOTAL=17"
if "%CORE_ONLY%"=="0" if "%NATIVE_UI%"=="1" set "UNIT_TOTAL=18"
set "TEST_COMMAND="%BIN%\devlink-unit-tests.exe""
call :expect unit 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\devlink-platform-tests.exe""
call :expect platform 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\devlink-platform-tests.exe" --ipv6"
call :expect ipv6 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\devlink-browser-tests.exe""
call :expect browser 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\devlink-share-tests.exe""
call :expect share 0 "0 failures" || exit /b
set "TEST_COMMAND="%BIN%\devlink-desktop-e2e-tests.exe""
if "%CORE_ONLY%"=="0" (call :expect desktop-e2e 0 "0 failures" || exit /b)
set "TEST_COMMAND="%BIN%\devlink-monitor.exe" --help"
call :expect cli-help 0 "no probes, no server, no sharing" || exit /b
set "TEST_COMMAND="%BIN%\devlink-monitor.exe" --version"
call :expect cli-version 0 "no sharing" || exit /b
set "TEST_COMMAND="%BIN%\devlink-monitor.exe" --not-a-devlink-option"
call :expect cli-unknown 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\devlink-monitor.exe" --version extra"
call :expect cli-extra 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\devlink-monitor.exe" --help --version"
call :expect cli-ambiguous 2 "Usage:" || exit /b
if "%CORE_ONLY%"=="0" (call :desktop_tests || exit /b)
>>"%LOG%" echo TEST SUMMARY passed=%PASS% skipped=%SKIP% mode=%MODE%
>>"%DIR%\test-summary.txt" echo TEST SUMMARY passed=%PASS% skipped=%SKIP% mode=%MODE%
call :notice_file "%DIR%\test-summary.txt"
set "TESTS_DETAIL=%PASS% réussis"
if not "%SKIP%"=="0" set "TESTS_DETAIL=%PASS% réussis %G_DOT% %SKIP% ignoré(s)"
call :step_done "%TESTS_DETAIL%"
if exist "%TEST_ROWS%" for /f "usebackq tokens=1-3 delims=|" %%A in ("%TEST_ROWS%") do call :row "%%A" "%%B" "%%C"
call :row ok "cli" "%CLI_PASS% contrôles de la ligne de commande du moniteur"
if "%CORE_ONLY%"=="0" call :row ok "ui" "%UI_PASS% contrôles du poste desktop %G_DOT% %UI_NOTE%"
if exist "%TEST_ROWS%" del "%TEST_ROWS%"
if not "%SKIP%"=="0" exit /b 77
exit /b 0

:desktop_tests
set "SDL_VIDEODRIVER=dummy"
set "SDL_RENDER_DRIVER=software"
set "TEST_COMMAND="%BIN%\devlink-desktop.exe" --smoke-test --screenshot "%DIR%\desktop.bmp""
call :expect ui-smoke 0 "desktop-smoke:" || exit /b
set "TEST_COMMAND="%BIN%\devlink-desktop.exe" --not-a-devlink-option"
call :expect ui-unknown 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\devlink-desktop.exe" --smoke-test --screenshot"
call :expect ui-missing-argument 2 "Usage:" || exit /b
set "TEST_COMMAND="%BIN%\devlink-desktop.exe" --screenshot never-created.bmp"
call :expect ui-capture-requires-smoke 2 "--smoke-test" || exit /b
set "SDL_VIDEODRIVER=devlink-invalid-video-driver"
set "TEST_COMMAND="%BIN%\devlink-desktop.exe" --smoke-test"
call :expect ui-driver-error 1 "SDL_Init:" || exit /b
set "SDL_VIDEODRIVER=dummy"
set "TEST_COMMAND="%BIN%\devlink-desktop.exe" --smoke-test --screenshot "%BIN%\devlink-desktop.exe\not-a-directory.bmp""
call :expect ui-file-error 1 "SDL_IOFromFile:" || exit /b
set "UI_NOTE=pilote dummy, fenêtre native avec --native-ui"
if "%NATIVE_UI%"=="1" (
  set "SDL_VIDEODRIVER=windows"
  set "UI_NOTE=fenêtre Windows native incluse"
  call :native_window_test || exit /b
) else >>"%LOG%" echo NOT RUN: native Windows window launch. Use --native-ui; dummy is not native display qualification.
set "SDL_VIDEODRIVER="
set "SDL_RENDER_DRIVER="
exit /b 0
:native_window_test
set "TEST_COMMAND="%BIN%\devlink-desktop.exe" --smoke-test --screenshot "%DIR%\desktop-native.bmp""
call :expect ui-native-windows 0 "video=windows renderer=software" || exit /b
exit /b 0

:fuzz
set "TOTAL=5"
set "MODE=fuzz"
set "CORE_ONLY=1"
set "DIR=%OUT%\tests\fuzz"
call :fresh "%DIR%" || exit /b
set "BIN=%DIR%\bin"
mkdir "%BIN%" "%DIR%\objects" "%DIR%\corpus"
call :step "Contrôle de la chaîne et sonde libFuzzer"
call :toolchain || exit /b
call :configure_flags || exit /b
call :sanitizer_runtime || exit /b
if "%FUZZ_AVAILABLE%"=="false" (call :note_later "libFuzzer UCRT64 indisponible : la sonde n'a pas compilé, lié et exécuté. SKIP code 77, jamais compté comme réussi."& exit /b 77)
call :step_done "%TOOLCHAIN_SUMMARY%"
if "%ASAN_AVAILABLE%"=="true" (set "SAN=%SAN% -fsanitize=address") else call :note_later "Ce clang++ UCRT64 n'a pas de runtime ASan : fuzzing sans instrumentation mémoire, jamais présenté comme test mémoire."
set "SAN=%SAN% -fsanitize=fuzzer-no-link"
set "UI_INCLUDES="
call :step "Compilation de la cible de fuzzing"
set "UNIT_TOTAL=2"
call :cpp engine/protocol/envelope.cpp || exit /b
set "FUZZ_OBJECTS="%OBJECT%""
call :cpp tests/fuzz/envelope_fuzz.cpp || exit /b
call :step_done "2 fichiers"
call :step "Édition des liens du fuzzer"
set "STEP_ITEM=devlink-fuzz-envelope.exe"
call :run "%CXX%" %VFLAG% %LINK_DRIVER% %PROFILE_FLAGS% %FUZZ_LINK_SAN% %FUZZ_OBJECTS% "%OBJECT%" -o "%BIN%\devlink-fuzz-envelope.exe"
if errorlevel 1 exit /b
if not exist "%BIN%\devlink-fuzz-envelope.exe" (call :problem "fuzzer output missing"& exit /b 1)
call :step_done "%FUZZ_LINK_SAN%"
call :step "Assemblage des DLL d'exécution et des licences"
call :runtime_dependencies || exit /b
call :step_done
call :step "Fuzzing de l'enveloppe : 20 000 exécutions"
copy /y tests\fuzz\corpus\hello "%DIR%\corpus\hello" >nul || exit /b
copy /y tests\fuzz\corpus\chat "%DIR%\corpus\chat" >nul || exit /b
call :run "%BIN%\devlink-fuzz-envelope.exe" -runs=20000 -seed=20261113 -max_len=4096 -timeout=2 "%DIR%\corpus"
if errorlevel 1 exit /b
set "FUZZ_NOTICE=PASS native libFuzzer without ASan (not installed in this UCRT64 clang++), 20000 executions"
if "%ASAN_AVAILABLE%"=="true" set "FUZZ_NOTICE=PASS native libFuzzer plus ASan, 20000 executions"
>>"%DIR%\fuzz-summary.txt" echo %FUZZ_NOTICE%
>>"%DIR%\fuzz-summary.txt" echo link-flags=%FUZZ_LINK_SAN% compile-flags=%SAN%
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows::%FUZZ_NOTICE%
call :step_done "aucun plantage %G_DOT% graine 20261113"
set "SUMMARY_ARTIFACT=%DIR%\fuzz-summary.txt"
set "SUMMARY_ARTIFACT_LABEL=Bilan      "
exit /b 0

:package
set "TOTAL=14"
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_START
call :fresh "%OUT%\packages" || exit /b
if "%CORE_ONLY%"=="1" (call :problem "package requires the full native desktop"& exit /b 2)
set "MODE=release"
set "DIR=%OUT%\release"
set "WITH_TESTS=1"
set "NATIVE_UI=1"
call :build_project || exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_BUILD_SUCCESS
call :tests || exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_TESTS_SUCCESS
call :step "Assemblage du paquet"
set "STAGE=%OUT%\packages\stage"
call :fresh "%STAGE%" || exit /b
mkdir "%STAGE%\DevLink\bin" "%STAGE%\DevLink\docs\reports" "%STAGE%\DevLink\licenses"
call :run copy /y packaging\README.md "%STAGE%\DevLink\README.md"
if errorlevel 1 exit /b
call :run copy /y docs\reports\BUILD_DEPENDENCIES.md "%STAGE%\DevLink\docs\reports\"
if errorlevel 1 exit /b
call :run copy /y docs\reports\UCRT64_WINDOWS_VALIDATION.md "%STAGE%\DevLink\docs\reports\"
if errorlevel 1 exit /b
call :run copy /y "%BIN%\devlink-monitor.exe" "%STAGE%\DevLink\bin\"
if errorlevel 1 exit /b
call :run copy /y "%BIN%\devlink-demo-service.exe" "%STAGE%\DevLink\bin\"
if errorlevel 1 exit /b
call :run copy /y "%BIN%\devlink-desktop.exe" "%STAGE%\DevLink\bin\"
if errorlevel 1 exit /b
call :run copy /y "%BIN%\*.dll" "%STAGE%\DevLink\bin\"
if errorlevel 1 exit /b
for %%F in (docs\BUILD.md docs\SECURITY.md docs\QUICKSTART.md docs\USER_GUIDE.md third_party\DEPENDENCIES.md) do (
  call :run copy /y "%%F" "%STAGE%\DevLink\docs\"
  if errorlevel 1 exit /b
)
for %%F in (SDL3.txt ImGui.txt Bundled-notices.txt) do (
  call :run copy /y "third_party\licenses\%%F" "%STAGE%\DevLink\licenses\"
  if errorlevel 1 exit /b
)
call :run copy /y docs\reports\UCRT64_DEPENDENCIES.md "%STAGE%\DevLink\docs\reports\"
if errorlevel 1 exit /b
call :run copy /y "%RUNTIME_REPORT%" "%STAGE%\DevLink\docs\runtime-dependencies.txt"
if errorlevel 1 exit /b
call :run xcopy.exe /E /I /Y "%DIR%\runtime-licenses" "%STAGE%\DevLink\licenses\runtime"
if errorlevel 1 exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_STAGE_READY
set "ARCHIVE=%OUT%\packages\DevLink-%PRODUCT_VERSION%-windows-ucrt64-x64.zip"
call :run "%TAR%" -a -cf "%ARCHIVE%" -C "%STAGE%" DevLink
if errorlevel 1 exit /b
call :run "%SystemRoot%\System32\certutil.exe" -hashfile "%ARCHIVE%" SHA256
if errorlevel 1 exit /b
for %%A in ("%ARCHIVE%") do set /a "ARCHIVE_KB=%%~zA/1024" >nul
call :step_done "DevLink-%PRODUCT_VERSION%-windows-ucrt64-x64.zip %G_DOT% %ARCHIVE_KB% Kio"
rem Etape 8 (mandat) : publication ATOMIQUE vers releases/ : ecriture .partial,
rem integrite et SHA-256 verifiees AVANT le renommage, jamais d'ecrasement muet.
rem Memes garanties que publish_release de build.sh (Linux).
call :step "Publication atomique dans releases"
set "PUBLISHED=%ROOT%\releases\DevLink-%PRODUCT_VERSION%-windows-ucrt64-x64.zip"
call :publish_release "%ARCHIVE%" "%PUBLISHED%" || exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_PUBLISHED
call :notice_file "%CMDLOG%"
call :step_done "SHA-256 %PR_HASH_SHORT%"
call :step "Vérification de l'archive publiée"
call :fresh "%OUT%\packages\check" || exit /b
rem L'extraction de preuve porte sur l'archive PUBLIEE : celle qu'un tiers recoit.
call :run "%TAR%" -xf "%PUBLISHED%" -C "%OUT%\packages\check"
if errorlevel 1 exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_EXTRACTED
for %%F in ("%STAGE%\DevLink\bin\*.exe" "%STAGE%\DevLink\bin\*.dll") do (
  call :run fc.exe /b "%%~fF" "%OUT%\packages\check\DevLink\bin\%%~nxF"
  if errorlevel 1 exit /b
)
call :run fc.exe /b packaging\README.md "%OUT%\packages\check\DevLink\README.md"
if errorlevel 1 exit /b
for %%F in (BUILD_DEPENDENCIES.md UCRT64_WINDOWS_VALIDATION.md) do (
  call :run fc.exe /b "docs\reports\%%F" "%OUT%\packages\check\DevLink\docs\reports\%%F"
  if errorlevel 1 exit /b
)
for %%F in (SDL3.txt ImGui.txt Bundled-notices.txt) do (
  call :run fc.exe /b "third_party\licenses\%%F" "%OUT%\packages\check\DevLink\licenses\%%F"
  if errorlevel 1 exit /b
)
rem Prove the extracted archive does not resolve missing DLLs from the MSYS2 PATH.
set "PATH=%SystemRoot%\System32;%SystemRoot%"
call :run "%OUT%\packages\check\DevLink\bin\devlink-monitor.exe" --version
if errorlevel 1 exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_MONITOR_EXECUTED_WITH_SYSTEM_ONLY_PATH
set "SDL_VIDEODRIVER=windows"
set "SDL_RENDER_DRIVER=software"
call :run "%OUT%\packages\check\DevLink\bin\devlink-desktop.exe" --smoke-test
if errorlevel 1 exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows package::PACKAGE_DESKTOP_EXECUTED_WITH_SYSTEM_ONLY_PATH
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows::PASS UCRT64 Release ZIP extracted and executed using native Windows video with system-only PATH
call :step_done "octets identiques %G_DOT% exécution hors MSYS2"
set "SUMMARY_ARTIFACT=%PUBLISHED%"
set "SUMMARY_ARTIFACT_LABEL=Archive    "
exit /b 0

rem Publication d'une archive vers releases/ (mandat 8, identique a build.sh) :
rem   ecriture <cible>.partial, integrite lue de bout en bout, SHA-256 publiee
rem   dans <cible>.sha256, renommage atomique, aucun ecrasement silencieux,
rem   cible refusee hors de %ROOT%\releases. IMPLEMENTED_BUT_NOT_VERIFIED depuis
rem   Linux : la mecanique est prouvee cote bash (auto-test release-publish-proof) ;
rem   cette copie batch suit exactement la meme procedure avec tar/certutil/move.
:publish_release
set "PR_SRC=%~1"
set "PR_DST=%~2"
if not exist "%ROOT%\releases" mkdir "%ROOT%\releases"
if not exist "%PR_SRC%" (call :problem "publish_release: archive source absente : %PR_SRC%"& exit /b 2)
echo "%PR_DST%" | findstr /I /B /C:"%ROOT%\releases\" >NUL
if errorlevel 1 (call :problem "publish_release: cible hors de releases/ : %PR_DST%"& exit /b 2)
for %%T in ("%PR_DST%") do set "PR_BASE=%%~nxT"
set "PR_PARTIAL=%PR_DST%.partial"
set "PR_SIDECAR=%PR_DST%.sha256"
set "PR_SIDE_PARTIAL=%PR_DST%.sha256.partial"
del /f /q "%PR_PARTIAL%" "%PR_SIDE_PARTIAL%" 2>NUL
call :run copy /y "%PR_SRC%" "%PR_PARTIAL%" >NUL
if errorlevel 1 exit /b
call :run "%TAR%" -tf "%PR_PARTIAL%" >NUL
if errorlevel 1 (del /f /q "%PR_PARTIAL%" 2>NUL & call :problem "publish_release: archive illisible : %PR_BASE%"& exit /b 2)
set "PR_HASH="
for /f "tokens=* delims=" %%H in ('certutil.exe -hashfile "%PR_PARTIAL%" SHA256 ^| findstr /V /C:"CertUtil"') do set "PR_HASH=%%H"
if "%PR_HASH%"=="" (del /f /q "%PR_PARTIAL%" 2>NUL & call :problem "publish_release: SHA-256 introuvable via certutil"& exit /b 2)
> "%PR_SIDE_PARTIAL%" echo %PR_HASH%  %PR_BASE%
set "PR_EXIST="
if exist "%PR_DST%" (
  for /f "tokens=* delims=" %%H in ('certutil.exe -hashfile "%PR_DST%" SHA256 ^| findstr /V /C:"CertUtil"') do set "PR_EXIST=%%H"
)
if defined PR_EXIST if /I not "%PR_EXIST%"=="%PR_HASH%" (
  del /f /q "%PR_PARTIAL%" "%PR_SIDE_PARTIAL%" 2>NUL
  call :problem "publish_release: %PR_BASE% existe deja avec une empreinte differente - aucun ecrasement silencieux"
  exit /b 2
)
if defined PR_EXIST del /f /q "%PR_PARTIAL%" 2>NUL & goto :pr_sidecar_only
move /y "%PR_PARTIAL%" "%PR_DST%" >NUL
if errorlevel 1 (del /f /q "%PR_PARTIAL%" 2>NUL & call :problem "publish_release: renommage atomique impossible"& exit /b 2)
:pr_sidecar_only
move /y "%PR_SIDE_PARTIAL%" "%PR_SIDECAR%" >NUL
if errorlevel 1 (call :problem "publish_release: publication de l'empreinte impossible"& exit /b 2)
set "PR_FINAL="
for /f "tokens=* delims=" %%H in ('certutil.exe -hashfile "%PR_DST%" SHA256 ^| findstr /V /C:"CertUtil"') do set "PR_FINAL=%%H"
if /I not "%PR_FINAL%"=="%PR_HASH%" (call :problem "publish_release: empreinte finale != empreinte calculee"& exit /b 2)
if exist "%PR_PARTIAL%" (call :problem "publish_release: fichier .partial restant"& exit /b 2)
set "PR_HASH_SHORT=%PR_HASH:~0,16%"
>>"%LOG%" echo RELEASE PUBLIEE %PR_DST% sha256=%PR_HASH%
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink Windows::PACKAGE SHA256 %PR_HASH% %PR_BASE%
exit /b 0

:clean_action
set "TOTAL=1"
call :step "Suppression des sorties compilées"
for %%D in (debug release tests packages doctor) do (
  call :plain_directory "%OUT%\%%D" || exit /b
  if exist "%OUT%\%%D" rmdir /s /q "%OUT%\%%D"
  if exist "%OUT%\%%D" (call :problem "clean failed for %%D"& exit /b 1)
)
>>"%LOG%" echo CLEAN SUCCESS: compiled outputs removed. releases/ untouched; logs and dependencies retained.
call :step_done "releases\, journaux et dépendances conservés"
exit /b 0

:selftest
set "TOTAL=7"
call :paths || exit /b
rem Negative child calls delegate to this script; no alternative compiler is invoked.
rem Les enfants ecrivent dans des journaux : sortie sans couleur ni reecriture.
rmdir "%OUT%\.build-lock"
set "LOCK=0"
set "DEVLINK_EXPECT_FAILURE=1"
set "DEVLINK_PLAIN=1"
call :step "Refus d'une installation MSYS2 absente"
set "SAVE_ROOT=%DEVLINK_MSYS2_ROOT%"
set "DEVLINK_MSYS2_ROOT=C:\devlink-missing-msys2"
call "%ROOT%\build.bat" doctor --core-only >"%OUT%\logs\selftest-missing-compiler.log" 2>&1
set "SELF_RC=%errorlevel%"
set "DEVLINK_MSYS2_ROOT=%SAVE_ROOT%"
if "%SELF_RC%"=="0" (call :problem "missing UCRT64 compiler falsely succeeded"& exit /b 1)
call :step_done "refusée, code %SELF_RC%"
call :step "Refus d'un environnement MSYS2 CLANG64"
set "MSYSTEM=CLANG64"
call "%ROOT%\build.bat" doctor --core-only >"%OUT%\logs\selftest-wrong-environment.log" 2>&1
set "SELF_RC=%errorlevel%"
set "MSYSTEM=UCRT64"
if "%SELF_RC%"=="0" (call :problem "CLANG64 was incorrectly accepted"& exit /b 1)
call :step_done "refusé, code %SELF_RC%"
call :step "Refus d'un compilateur imposé par variable"
set "PLPT_CXX=C:\Program Files\LLVM\bin\clang++.exe"
call "%ROOT%\build.bat" doctor --core-only >"%OUT%\logs\selftest-compiler-override.log" 2>&1
set "SELF_RC=%errorlevel%"
set "PLPT_CXX="
if "%SELF_RC%"=="0" (call :problem "alternative compiler override was accepted"& exit /b 1)
call :step_done "refusé, code %SELF_RC%"
call :step "Échec de compilation propagé, sans reliquat"
set "DEVLINK_SELFTEST_FAIL=compile"
call "%ROOT%\build.bat" debug --core-only >"%OUT%\logs\selftest-compile-failure.log" 2>&1
set "SELF_RC=%errorlevel%"
set "DEVLINK_SELFTEST_FAIL="
if "%SELF_RC%"=="0" (call :problem "invalid compiler flag falsely succeeded"& exit /b 1)
if exist "%OUT%\debug\BUILD_SUCCEEDED" (call :problem "stale build success after compilation failure"& exit /b 1)
if exist "%OUT%\debug\bin\devlink-monitor.exe" (call :problem "stale executable after compilation failure"& exit /b 1)
call :step_done "code %SELF_RC% %G_DOT% aucune sortie réussie"
call :step "Échec de liaison propagé, sans reliquat"
set "DEVLINK_SELFTEST_FAIL=link"
call "%ROOT%\build.bat" debug --core-only >"%OUT%\logs\selftest-link-failure.log" 2>&1
set "SELF_RC=%errorlevel%"
set "DEVLINK_SELFTEST_FAIL="
if "%SELF_RC%"=="0" (call :problem "invalid linker flag falsely succeeded"& exit /b 1)
if exist "%OUT%\debug\BUILD_SUCCEEDED" (call :problem "stale build success after link failure"& exit /b 1)
if exist "%OUT%\debug\bin\devlink-monitor.exe" (call :problem "stale executable after link failure"& exit /b 1)
call :step_done "code %SELF_RC% %G_DOT% aucune sortie réussie"
call :step "Refus d'une archive corrompue"
set "DIR=%OUT%\tests\negative"
call :fresh "%DIR%" || exit /b
>"%DIR%\bad.pkg" echo Deliberate corrupt archive
set "DEVLINK_QUIET_PROBLEM=1"
call :verify "%DIR%\bad.pkg" "%SDL_SHA%"
set "SELF_RC=%errorlevel%"
set "DEVLINK_QUIET_PROBLEM="
if "%SELF_RC%"=="0" (call :problem "corrupt archive was accepted"& exit /b 1)
call :step_done "empreinte SHA-256 refusée, rien d'extrait"
call :step "Propagation exacte du code de sortie d'un test"
if not exist "%OUT%\doctor\probe.exe" (call :problem "sonde absente : lancez d'abord build.bat doctor"& exit /b 1)
set "SELF_RC=0"
"%OUT%\doctor\probe.exe" --fail >"%DIR%\intentional-exit.log" 2>&1
set "SELF_RC=%errorlevel%"
if not "%SELF_RC%"=="23" (call :problem "test process exit code 23 was not preserved"& exit /b 1)
findstr /C:"intentional test failure: exit 23" "%DIR%\intentional-exit.log" >nul
if errorlevel 1 (call :problem "intentional failure probe output was not preserved"& exit /b 1)
>>"%LOG%" echo PASS intentional-exit-propagation: exit=23 preserved by the probe
call :step_done "code 23 conservé"
>>"%LOG%" echo SELFTEST PASS: missing/wrong toolchain, compile, link, hash and test exit 23; no stale successful output
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink UCRT64::SELFTEST PASS missing compiler, CLANG64, compiler override, compile/link/hash failures and test exit 23
set "DEVLINK_PLAIN="
exit /b 0

:ide_check
set "TOTAL=4"
set "DIR=%OUT%\tests\ide"
call :fresh "%DIR%" || exit /b
set "BIN=%DIR%\bin"
mkdir "%BIN%" "%DIR%\objects"
call :step "Contrôle de la chaîne de compilation UCRT64"
call :toolchain || exit /b
call :configure_flags || exit /b
call :step_done "%TOOLCHAIN_SUMMARY%"
call :step "Compilation de la sonde de contrat VS Code"
set "UNIT_TOTAL=1"
call :cpp tests/build/vscode_contract.cpp || exit /b
call :step_done "1 fichier"
call :step "Édition des liens et DLL d'exécution"
set "STEP_ITEM=vscode-contract.exe"
call :run "%CXX%" %LINK_DRIVER% "%OBJECT%" -o "%BIN%\vscode-contract.exe"
if errorlevel 1 exit /b
call :runtime_dependencies || exit /b
call :step_done
call :step "Vérification du contrat .vscode"
set "STEP_ITEM=vscode-contract.exe"
call :run "%BIN%\vscode-contract.exe"
if errorlevel 1 exit /b
if "%GITHUB_ACTIONS%"=="true" echo ::notice title=DevLink UCRT64::PASS native VS Code task/config contract; interactive editor UI not tested
call :step_done "UCRT64 %G_DOT% C++20 %G_DOT% ImGui"
exit /b 0
