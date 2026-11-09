#pragma once

#include <cstdint>
#include <stop_token>
#include <string>

namespace Core::Uwp {
void UpdateShaderCompilation(std::uint32_t stage, std::uint32_t processed = 0,
                             std::uint32_t total = 0, std::uint32_t compiled = 0);
bool StopRequested();
bool PauseRequested();
std::stop_token GuestStopToken();
void RegisterGuestWorker(std::stop_source source);
void StopGuestWorkers();
void SetGuestReady(bool ready);
void ReportError(std::string message);
void FailGuestSession(std::string message);
void FailGuestMemory(std::uint32_t error, std::uint64_t address, std::uint64_t access) noexcept;
void ClearStorage();
std::string PrepareBootStorage(std::string executable);
} // namespace Core::Uwp
