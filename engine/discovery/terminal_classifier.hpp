#pragma once
#include <string>
#include <vector>
#include "process_tree.hpp"
namespace plpt {
enum class OriginKind : std::uint8_t { Unknown, Terminal, Shell, Ide, Runner, ContainerProxy, Service };
struct OriginInference {
    OriginKind kind = OriginKind::Unknown;
    std::string label = "origine inconnue";
    Confidence confidence = Confidence::Unknown;
    std::vector<std::string> evidence;
    std::vector<std::string> alternatives;
};
OriginInference classify_origin(const ProcessTree&, ProcessId);
} // namespace plpt
