include(FetchContent)
# Consentement explicite PLPT_BUILD_UI=ON : téléchargements de build uniquement.
# Aucun tracking de branche ni find_package qui accepterait une version inconnue.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
if(PLPT_HEADLESS_UI)
  if(WIN32)
    message(FATAL_ERROR "Headless UI profile is intended only for the Linux sandbox")
  endif()
  set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
  set(SDL_X11 OFF CACHE BOOL "" FORCE)
  set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
  set(SDL_KMSDRM OFF CACHE BOOL "" FORCE)
  set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
  set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
  set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(sdl3
  URL https://codeload.github.com/libsdl-org/SDL/tar.gz/f00dc919ca35b9756316e86c4777392de442f9b2
  URL_HASH SHA256=e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(imgui
  URL https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859
  URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sdl3 imgui)
add_library(plpt_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(plpt_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(plpt_imgui PUBLIC SDL3::SDL3-static)
add_executable(plpt-desktop app/desktop_ui/main.cpp)
target_link_libraries(plpt-desktop PRIVATE plpt_core plpt_imgui)
if(WIN32)
  set_target_properties(plpt-desktop PROPERTIES WIN32_EXECUTABLE ON)
  target_sources(plpt-desktop PRIVATE ${PROJECT_SOURCE_DIR}/packaging/windows/app.manifest)
endif()
install(TARGETS plpt-desktop RUNTIME DESTINATION bin)
install(FILES third_party/licenses/SDL3.txt third_party/licenses/ImGui.txt
  third_party/licenses/Bundled-notices.txt DESTINATION share/licenses/plpt)
if(BUILD_TESTING)
  add_test(NAME desktop-smoke COMMAND plpt-desktop --smoke-test)
  set_tests_properties(desktop-smoke PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software")
  add_test(NAME desktop-errors COMMAND ${CMAKE_COMMAND} -DPLPT_DESKTOP=$<TARGET_FILE:plpt-desktop>
    -P ${PROJECT_SOURCE_DIR}/tests/desktop_errors.cmake)
  set_tests_properties(desktop-errors PROPERTIES TIMEOUT 60)
endif()
