#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace light {

class Error : public std::runtime_error {
public:
    explicit Error(const std::wstring& message);
};

std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);
std::wstring ErrorText(DWORD code);
std::wstring Describe(const std::exception& error);

struct Device {
    std::wstring address;
    unsigned short port = 9123;
    std::wstring name = L"Elgato light";
    std::wstring serviceId;
    std::wstring Key() const;
    bool operator==(const Device&) const = default;
};

Device ParseAddress(const std::wstring& address);
void Validate(const Device& device);

struct State {
    bool on = false;
    int brightness = 0;
    int temperature = 222;
    bool operator==(const State&) const = default;
};

struct Change {
    std::optional<bool> on;
    std::optional<int> brightness;
    std::optional<int> temperature;
    bool Empty() const;
    void Merge(const Change& newer);
    void Validate() const;
};

int ToMired(int kelvin);
int ToKelvin(int mired);
State ParseState(const std::wstring& json);
std::string ChangeJson(const Change& change);
State Read(const Device& device);
State Write(const Device& device, const Change& change);
struct ProbeResult { Device device; State state; };
ProbeResult Probe(const Device& device);

struct Settings {
    std::vector<Device> devices;
    std::wstring selectedKey;
};

std::filesystem::path DataDirectory();
Settings LoadSettings(const std::filesystem::path& path);
void SaveSettings(const std::filesystem::path& path, const Settings& settings);
void Log(const std::wstring& message);

struct DiscoveryResult {
    std::vector<Device> devices;
    std::wstring notice;
};

std::vector<std::wstring> SubnetCandidates(std::uint32_t address, unsigned prefixLength);
DiscoveryResult Discover(std::stop_token stop = {});

}
