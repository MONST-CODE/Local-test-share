#include <iostream>
#include <string_view>
#include "engine/discovery/monitor.hpp"
#include "engine/platform/native.hpp"
namespace {
std::string printable(std::string value) {
    if (value.size() > 256) value.resize(256);
    for (char& c : value) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
    return value;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "PLPT 0.1.0-preview / local monitor / no sharing\n";
        return 0;
    }
    if (argc > 1) {
        std::cout << "Usage: plpt-monitor [--help | --version]\n"
                     "Local TCP listeners / UDP endpoints; no probes, no server, no sharing.\n";
        return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }
    plpt::NativeProcessEnumerator processes;
    plpt::NativeSocketTableProvider sockets;
    auto snapshot = plpt::observe(processes, sockets);
    if (!snapshot) {
        std::cerr << snapshot.error().context << ": " << snapshot.error().message << " (" << snapshot.error().native_code << ")\n";
        return 2;
    }
    plpt::ServiceRegistry registry;
    plpt::SystemClock clock;
    auto& data = snapshot.value();
    if (auto r = registry.ingest(data.sockets, data.processes, clock.now_monotonic_ms()); !r) {
        std::cerr << r.error().message << '\n'; return 2;
    }
    std::cout << "PLPT — moniteur local 0.1.0-preview\nAucun partage actif. Aucun listener PLPT ouvert.\n"
                 "PROTO\tADRESSE\tPORT\tPID\tPROCESSUS\tORIGINE PROBABLE\tCONFIANCE\tÉTAT\n";
    for (const auto& [id, service] : registry.services()) {
        (void)id;
        for (const auto& endpoint : service.endpoints) {
            std::cout << (endpoint.protocol == plpt::TransportProtocol::Tcp ? "TCP" : "UDP") << '\t'
                << endpoint.local.address << '%' << endpoint.local.scope_id << '\t' << endpoint.local.port.value << '\t'
                << (endpoint.owner_pid ? std::to_string(endpoint.owner_pid->value) : "?") << '\t'
                << printable(service.process ? service.process->image_name : "inconnu") << '\t'
                << printable(service.origin.label) << '\t' << plpt::to_string(service.origin.confidence) << '\t'
                << plpt::to_string(service.share_state) << '\n';
        }
    }
    for (const auto& e : data.sockets.warnings) std::cerr << e.context << ": " << e.message << " (" << e.native_code << ")\n";
    for (const auto& e : data.processes.warnings) std::cerr << e.context << ": " << e.message << " (" << e.native_code << ")\n";
    std::cout << registry.live_count() << " groupe(s) de services observés. Un PID inconnu reste inconnu.\n";
    return data.sockets.complete && data.processes.complete ? 0 : 3;
}
