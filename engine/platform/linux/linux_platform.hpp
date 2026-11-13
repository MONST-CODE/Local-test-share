#pragma once
#include "../platform.hpp"
#include <string_view>

namespace plpt {
// Parsers purs : fixtures déterministes, pas d'exception stoi/stoul sur une ligne.
struct ProcSocketRow { SocketRecord socket; std::uint64_t inode = 0; };
Result<ProcSocketRow> parse_proc_socket(std::string_view line, TransportProtocol, IpFamily);
Result<ProcessRecord> parse_proc_stat(std::string_view line);
class LinuxSocketTableProvider final : public ISocketTableProvider {
public:
    explicit LinuxSocketTableProvider(std::string proc_root = "/proc") : root_(std::move(proc_root)) {}
    Result<Observation<SocketRecord>> snapshot() override;
private:
    std::string root_;
};
class LinuxProcessEnumerator final : public IProcessEnumerator {
public:
    explicit LinuxProcessEnumerator(std::string proc_root = "/proc") : root_(std::move(proc_root)) {}
    Result<Observation<ProcessRecord>> enumerate() override;
private:
    std::string root_;
};
class LinuxNetworkInterfaceProvider final : public INetworkInterfaceProvider {
public:
    Result<std::vector<NetworkInterfaceInfo>> enumerate() override;
};
} // namespace plpt
