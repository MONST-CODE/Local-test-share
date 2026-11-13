// Native contract test for editor delegation; never invokes a compiler/build itself.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
std::string compact(const char* path) {
    std::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    std::string result;
    bool quoted = false, escaped = false;
    for (const char c : text) {
        if (!quoted && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) continue;
        result += c;
        if (escaped) { escaped = false; continue; }
        if (quoted && c == '\\') { escaped = true; continue; }
        if (c == '"') quoted = !quoted;
    }
    return quoted || !input ? std::string{} : result;
}
std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}
}
int main() {
    const auto tasks = compact(".vscode/tasks.json");
    const auto config = compact(".vscode/c_cpp_properties.json");
    if (tasks.empty() || config.empty()) return 1;
    if (count(tasks, "\"type\":\"process\"") != 4 ||
        count(tasks, "\"command\":\"${env:ComSpec}\"") != 4 ||
        count(tasks, "\"cwd\":\"${workspaceFolder}\"") != 4) return 1;
    for (const auto action : {"debug", "release", "test", "clean"}) {
        const std::string args = std::string("\"args\":[\"/d\",\"/c\",\"build.bat\",\"") + action + "\"]";
        if (count(tasks, args) != 1) return 1;
        std::printf("VS Code task contract: cmd.exe /d /c build.bat %s\n", action);
    }
    if (count(config, "\"compilerPath\":\"C:/msys64/ucrt64/bin/clang++.exe\"") != 1 ||
        count(config, "\"compilerPath\":\"${env:PLPT_MSYS2_ROOT}/ucrt64/bin/clang++.exe\"") != 1 ||
        count(config, "\"intelliSenseMode\":\"windows-clang-x64\"") != 2 ||
        count(config, "\"cppStandard\":\"c++20\"") != 2 ||
        count(config, "\"${env:PLPT_IMGUI_DIR}\"") != 1) return 1;
    std::puts("VS Code configuration contract: PASS UCRT64 compiler, GNU discovery, C++20, existing ImGui root");
    std::puts("Interactive VS Code UI/extension execution is NOT covered by this contract test.");
    return 0;
}
