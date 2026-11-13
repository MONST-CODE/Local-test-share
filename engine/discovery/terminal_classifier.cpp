#include "terminal_classifier.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>
namespace plpt {
namespace {
struct Rule { std::string_view name; OriginKind kind; int priority; };
constexpr Rule rules[] = {
    {"windowsterminal.exe", OriginKind::Terminal, 100}, {"wt.exe", OriginKind::Terminal, 100},
    {"gnome-terminal-server", OriginKind::Terminal, 100}, {"konsole", OriginKind::Terminal, 100},
    {"code.exe", OriginKind::Ide, 90}, {"code", OriginKind::Ide, 90},
    {"devenv.exe", OriginKind::Ide, 90}, {"idea64.exe", OriginKind::Ide, 90},
    {"powershell.exe", OriginKind::Shell, 80}, {"pwsh.exe", OriginKind::Shell, 80},
    {"pwsh", OriginKind::Shell, 80}, {"cmd.exe", OriginKind::Shell, 80},
    {"bash", OriginKind::Shell, 80}, {"zsh", OriginKind::Shell, 80}, {"fish", OriginKind::Shell, 80},
    {"docker-proxy", OriginKind::ContainerProxy, 70}, {"com.docker.backend.exe", OriginKind::ContainerProxy, 70},
    {"npm.cmd", OriginKind::Runner, 30}, {"npm", OriginKind::Runner, 30},
    {"pnpm", OriginKind::Runner, 30}, {"yarn", OriginKind::Runner, 30},
    {"systemd", OriginKind::Service, 10}, {"services.exe", OriginKind::Service, 10}
};
}
OriginInference classify_origin(const ProcessTree& tree, ProcessId pid) {
    OriginInference result;
    const auto chain = tree.ancestors(pid);
    const auto* child = tree.find(pid);
    bool verified_times = child && child->record.start_time_ticks != 0;
    int best = -1;
    for (const auto& ancestor : chain) {
        verified_times = verified_times && ancestor.start_time_ticks != 0;
        auto name = ancestor.image_name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& rule : rules) {
            if (rule.name != name) continue; // pas de faux positif « not-bash-server ».
            result.alternatives.push_back(ancestor.image_name + " (probable)");
            result.evidence.push_back("ancêtre PPID observé : " + ancestor.image_name);
            if (rule.priority > best) {
                best = rule.priority;
                result.kind = rule.kind;
                result.label = ancestor.image_name + " (probable)";
            }
        }
    }
    if (best < 0) {
        result.confidence = chain.empty() ? Confidence::Unknown : Confidence::Low;
        result.evidence.push_back(chain.empty() ? "aucun ancêtre fiable observable" : "ancêtres non reconnus");
    } else {
        result.confidence = verified_times && best >= 80 ? Confidence::High : Confidence::Medium;
        if (!verified_times) result.evidence.push_back("heure de création partiellement inconnue");
    }
    // Confiance dans une filiation observée, JAMAIS preuve d'identité/authentification.
    return result;
}
} // namespace plpt
