#pragma once
#ifdef _WIN32
#include "windows/windows_platform.hpp"
namespace plpt {
using NativeSocketTableProvider = WindowsSocketTableProvider;
using NativeProcessEnumerator = WindowsProcessEnumerator;
using NativeNetworkInterfaceProvider = WindowsNetworkInterfaceProvider;
}
#elif defined(__linux__)
#include "linux/linux_platform.hpp"
namespace plpt {
using NativeSocketTableProvider = LinuxSocketTableProvider;
using NativeProcessEnumerator = LinuxProcessEnumerator;
using NativeNetworkInterfaceProvider = LinuxNetworkInterfaceProvider;
}
#else
#error "PLPT supports Windows x64 first, Linux x64 second."
#endif
