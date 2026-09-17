#pragma once
#include <string>
namespace Network {
bool Start();
void Stop();
void Restart();
bool IsRunning();
std::string ConsoleIpAddress();
std::string ListenerStatusSummary();
} // namespace Network
