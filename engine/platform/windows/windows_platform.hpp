#pragma once
#include "../platform.hpp"
namespace plpt {
class WindowsSocketTableProvider final : public ISocketTableProvider {
public:
    Result<Observation<SocketRecord>> snapshot() override;
};
class WindowsProcessEnumerator final : public IProcessEnumerator {
public:
    Result<Observation<ProcessRecord>> enumerate() override;
};
class WindowsNetworkInterfaceProvider final : public INetworkInterfaceProvider {
public:
    Result<std::vector<NetworkInterfaceInfo>> enumerate() override;
};
} // namespace plpt
