#pragma once
#include <cstdint>
#include <string>
namespace Network {
bool Start();
void Stop();
void Restart();
void Reconfigure();
void EnsureListeners();
bool IsRunning();
uint64_t UptimeMs();
std::string ConsoleIpAddress();
std::string ListenerStatusSummary();
} // namespace Network
