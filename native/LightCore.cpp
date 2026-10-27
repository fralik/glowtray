#include "LightCore.h"
#include <winhttp.h>
#include <iphlpapi.h>
#include <windns.h>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace light {
namespace {
using namespace winrt::Windows::Data::Json;

std::wstring Canonical(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    while (!text.empty() && text.back() == L'.') text.pop_back();
    return text;
}

void Check(BOOL success, const wchar_t* operation) {
    if (!success) throw Error(std::wstring(operation) + L": " + ErrorText(GetLastError()));
}

struct InternetHandle {
    HINTERNET value;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    explicit InternetHandle(HINTERNET handle) : value(handle) {
        if (!value) throw Error(L"HTTP initialization: " + ErrorText(GetLastError()));
    }
};

std::wstring Request(const Device& device, const wchar_t* method, const wchar_t* path,
                     const std::string& body = {}) {
    Validate(device);
    InternetHandle session(WinHttpOpen(L"RingLightNative/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    Check(WinHttpSetTimeouts(session.value, 1500, 2500, 2500, 2500), L"Setting HTTP timeouts");
    InternetHandle connection(WinHttpConnect(session.value, device.address.c_str(), device.port, 0));
    InternetHandle request(WinHttpOpenRequest(connection.value, method, path, L"HTTP/1.1",
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    Check(WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)),
        L"Disabling HTTP redirects");
    const wchar_t* headers = L"Content-Type: application/json\r\nConnection: close\r\n";
    const auto length = static_cast<DWORD>(body.size());
    // Supplying the total length avoids chunked requests, which Ring Light firmware rejects.
    Check(WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), length, length, 0),
        L"Sending to light");
    Check(WinHttpReceiveResponse(request.value, nullptr), L"Receiving from light");
    DWORD status = 0, size = sizeof(status);
    Check(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX), L"Reading HTTP status");
    if (status < 200 || status >= 300) throw Error(L"The light returned HTTP " + std::to_wstring(status));
    std::string result;
    char buffer[4096];
    const auto started = GetTickCount64();
    for (;;) {
        DWORD received = 0;
        Check(WinHttpReadData(request.value, buffer, sizeof(buffer), &received), L"Reading light response");
        if (!received) break;
        if (result.size() + received > 65536 || GetTickCount64() - started > 8000)
            throw Error(L"The light response exceeded the size or time limit.");
        result.append(buffer, received);
    }
    return Wide(result);
}

JsonObject Object(const std::wstring& json) {
    JsonObject result{nullptr};
    if (!JsonObject::TryParse(json, result)) throw Error(L"Invalid JSON received from the light or settings file.");
    return result;
}

int Integer(const JsonObject& object, const wchar_t* name, int minimum, int maximum) {
    if (!object.HasKey(name) || object.GetNamedValue(name).ValueType() != JsonValueType::Number)
        throw Error(L"Missing or non-numeric field: " + std::wstring(name));
    const double value = object.GetNamedNumber(name);
    if (!std::isfinite(value) || value != std::floor(value) || value < minimum || value > maximum)
        throw Error(L"Out-of-range field: " + std::wstring(name));
    return static_cast<int>(value);
}

std::wstring String(const JsonObject& object, const wchar_t* name, bool required = false) {
    if (!object.HasKey(name)) {
        if (required) throw Error(L"Missing field: " + std::wstring(name));
        return {};
    }
    if (!required && object.GetNamedValue(name).ValueType() == JsonValueType::Null) return {};
    if (object.GetNamedValue(name).ValueType() != JsonValueType::String)
        throw Error(L"Invalid text field: " + std::wstring(name));
    return std::wstring(object.GetNamedString(name));
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error(L"Cannot open settings: " + path.wstring());
    std::string result;
    char block[4096];
    while (input.read(block, sizeof(block)) || input.gcount()) {
        result.append(block, static_cast<std::size_t>(input.gcount()));
        if (result.size() > 65536) throw Error(L"The settings file is too large.");
    }
    if (!input.eof()) throw Error(L"Could not read the settings file.");
    if (result.starts_with("\xef\xbb\xbf")) result.erase(0, 3);
    return result;
}

struct Socket {
    SOCKET value = INVALID_SOCKET;
    explicit Socket(SOCKET handle) : value(handle) {}
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
    Socket(Socket&& other) noexcept : value(std::exchange(other.value, INVALID_SOCKET)) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

struct BrowseContext {
    std::atomic_uint references{2}; // Browse lifetime and caller lifetime.
    std::mutex mutex;
    std::map<std::wstring, std::pair<std::wstring, unsigned short>> services;
    std::map<std::wstring, std::set<std::wstring>> addresses;
    DNS_SERVICE_CANCEL cancel{};
    DNS_SERVICE_BROWSE_REQUEST request{};
    DWORD error = ERROR_SUCCESS;
    void Release() { if (references.fetch_sub(1) == 1) delete this; }
};

void WINAPI BrowseCallback(DWORD status, void* context, DNS_RECORD* records) noexcept {
    auto* data = static_cast<BrowseContext*>(context);
    data->references.fetch_add(1);
    try {
        std::lock_guard lock(data->mutex);
        if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) data->error = status;
        if (status == ERROR_SUCCESS) {
            for (auto* record = records; record; record = record->pNext) {
                if (!record->pName || record->dwTtl == 0) continue;
                if (record->wType == DNS_TYPE_SRV && record->Data.SRV.pNameTarget &&
                    Canonical(record->pName).ends_with(L"._elg._tcp.local")) {
                    data->services[Canonical(record->pName)] = {
                        Canonical(record->Data.SRV.pNameTarget), record->Data.SRV.wPort};
                } else if (record->wType == DNS_TYPE_A) {
                    wchar_t address[INET_ADDRSTRLEN]{};
                    if (InetNtopW(AF_INET, &record->Data.A.IpAddress, address, INET_ADDRSTRLEN))
                        data->addresses[Canonical(record->pName)].insert(address);
                }
            }
        }
    } catch (const std::exception&) {
        std::lock_guard lock(data->mutex);
        data->error = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
    if (status == ERROR_CANCELLED) data->Release();
    data->Release();
}

std::vector<std::wstring> LocalCandidates() {
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG status = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size);
    }
    if (status == ERROR_NO_DATA) return {};
    if (status != NO_ERROR) throw Error(L"Reading network adapters: " + ErrorText(status));
    std::set<std::wstring> candidates, ownAddresses;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            (adapter->IfType != IF_TYPE_IEEE80211 && adapter->IfType != IF_TYPE_ETHERNET_CSMACD)) continue;
        for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            if (entry->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* address = reinterpret_cast<sockaddr_in*>(entry->Address.lpSockaddr);
            const auto ip = ntohl(address->sin_addr.s_addr);
            if ((ip >> 24) == 127 || (ip & 0xffff0000u) == 0xa9fe0000u || ip == 0) continue;
            wchar_t own[INET_ADDRSTRLEN]{};
            InetNtopW(AF_INET, &address->sin_addr, own, INET_ADDRSTRLEN);
            ownAddresses.insert(own);
            for (auto& candidate : SubnetCandidates(ip, entry->OnLinkPrefixLength))
                candidates.insert(std::move(candidate));
        }
    }
    for (const auto& own : ownAddresses) candidates.erase(own);
    return {candidates.begin(), candidates.end()};
}
}

Error::Error(const std::wstring& message) : std::runtime_error(Utf8(message)) {}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode text.");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 response.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size);
    return result;
}

std::wstring ErrorText(DWORD code) {
    wchar_t buffer[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring text = buffer;
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    return (text.empty() ? L"Windows error" : text) + L" (" + std::to_wstring(code) + L")";
}

std::wstring Describe(const std::exception& error) { return Wide(error.what()); }
std::wstring Device::Key() const { return Canonical(address) + L":" + std::to_wstring(port); }

void Validate(const Device& device) {
    if (device.address.empty() || !device.port || device.name.empty() || device.address.size() > 253 ||
        device.address.find_first_of(L"/\\@?# \t\r\n") != std::wstring::npos)
        throw Error(L"Invalid light address, name, or port.");
}

Device ParseAddress(const std::wstring& input) {
    const auto start = input.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) throw Error(L"Enter an IP address or hostname.");
    auto text = input.substr(start, input.find_last_not_of(L" \t\r\n") - start + 1);
    if (text.find(L"://") == std::wstring::npos) text = L"http://" + text;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(text.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTP ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength ||
        parts.dwExtraInfoLength || (parts.dwUrlPathLength &&
            std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/"))
        throw Error(L"Enter an IP address or hostname, optionally followed by :port (HTTP only).");
    Device result;
    result.address.assign(parts.lpszHostName, parts.dwHostNameLength);
    if (result.address.starts_with(L"[") && result.address.ends_with(L"]"))
        result.address = result.address.substr(1, result.address.size() - 2);
    const auto authorityStart = text.find(L"://") + 3;
    const auto authority = text.substr(authorityStart, text.find(L'/', authorityStart) - authorityStart);
    const bool explicitPort = authority.starts_with(L"[")
        ? authority.find(L"]:") != std::wstring::npos : authority.find(L':') != std::wstring::npos;
    result.port = explicitPort ? parts.nPort : 9123;
    Validate(result);
    return result;
}

bool Change::Empty() const { return !on && !brightness && !temperature; }
void Change::Merge(const Change& newer) {
    if (newer.on) on = newer.on;
    if (newer.brightness) brightness = newer.brightness;
    if (newer.temperature) temperature = newer.temperature;
}
void Change::Validate() const {
    if (Empty() || (brightness && (*brightness < 0 || *brightness > 100)) ||
        (temperature && (*temperature < 143 || *temperature > 344)))
        throw Error(L"Invalid light setting.");
}

int ToMired(int kelvin) {
    if (kelvin < 2900 || kelvin > 7000) throw Error(L"Temperature must be between 2900 and 7000 K.");
    return std::clamp(static_cast<int>(std::lround(1000000.0 / kelvin)), 143, 344);
}
int ToKelvin(int mired) {
    if (mired < 143 || mired > 344) throw Error(L"Invalid light temperature.");
    return std::clamp(static_cast<int>(std::lround(1000000.0 / mired / 50)) * 50, 2900, 7000);
}

State ParseState(const std::wstring& json) {
    try {
        const auto root = Object(json);
        Integer(root, L"numberOfLights", 1, 1);
        const auto lights = root.GetNamedArray(L"lights");
        if (lights.Size() != 1) throw Error(L"Only single white Elgato lights are supported.");
        const auto item = lights.GetObjectAt(0);
        return {Integer(item, L"on", 0, 1) == 1, Integer(item, L"brightness", 0, 100),
            Integer(item, L"temperature", 143, 344)};
    } catch (const winrt::hresult_error& error) {
        throw Error(L"Unsupported or malformed light response: " + std::wstring(error.message()));
    }
}

std::string ChangeJson(const Change& change) {
    change.Validate();
    std::string fields;
    auto add = [&](const char* name, int value) {
        if (!fields.empty()) fields += ',';
        fields += "\"" + std::string(name) + "\":" + std::to_string(value);
    };
    if (change.on) add("on", *change.on ? 1 : 0);
    if (change.brightness) add("brightness", *change.brightness);
    if (change.temperature) add("temperature", *change.temperature);
    return "{\"numberOfLights\":1,\"lights\":[{" + fields + "}]}";
}

State Read(const Device& device) { return ParseState(Request(device, L"GET", L"/elgato/lights")); }
State Write(const Device& device, const Change& change) {
    Request(device, L"PUT", L"/elgato/lights", ChangeJson(change));
    return Read(device);
}
ProbeResult Probe(const Device& device) {
    auto result = ProbeResult{device, Read(device)};
    try {
        const auto info = Object(Request(device, L"GET", L"/elgato/accessory-info"));
        const auto product = String(info, L"productName", true);
        if (product.empty()) throw Error(L"The device did not return a product name.");
        const auto name = String(info, L"displayName");
        result.device.name = name.empty() ? product : name;
        return result;
    } catch (const winrt::hresult_error& error) {
        throw Error(L"Invalid accessory information: " + std::wstring(error.message()));
    }
}

std::filesystem::path DataDirectory() {
    PWSTR path = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path));
    const auto result = std::filesystem::path(path) / L"RingLightNative";
    CoTaskMemFree(path);
    return result;
}

Settings LoadSettings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    try {
        const auto root = Object(Wide(ReadFile(path)));
        Settings result;
        result.selectedKey = String(root, L"SelectedKey");
        const auto devices = root.GetNamedArray(L"Devices");
        if (devices.Size() > 64) throw Error(L"Too many saved lights.");
        std::set<std::wstring> keys;
        for (const auto& value : devices) {
            const auto item = value.GetObject();
            Device device;
            device.address = String(item, L"Address", true);
            device.port = static_cast<unsigned short>(Integer(item, L"Port", 1, 65535));
            device.name = String(item, L"Name", true);
            if (item.HasKey(L"ServiceId") && item.GetNamedValue(L"ServiceId").ValueType() != JsonValueType::Null)
                device.serviceId = String(item, L"ServiceId");
            Validate(device);
            if (keys.insert(device.Key()).second) result.devices.push_back(std::move(device));
        }
        return result;
    } catch (const winrt::hresult_error& error) {
        throw Error(L"Invalid settings: " + std::wstring(error.message()));
    }
}

void SaveSettings(const std::filesystem::path& path, const Settings& settings) {
    JsonObject root;
    JsonArray devices;
    for (const auto& device : settings.devices) {
        Validate(device);
        JsonObject item;
        item.SetNamedValue(L"Address", JsonValue::CreateStringValue(device.address));
        item.SetNamedValue(L"Port", JsonValue::CreateNumberValue(device.port));
        item.SetNamedValue(L"Name", JsonValue::CreateStringValue(device.name));
        item.SetNamedValue(L"ServiceId", JsonValue::CreateStringValue(device.serviceId));
        devices.Append(item);
    }
    root.SetNamedValue(L"Devices", devices);
    root.SetNamedValue(L"SelectedKey", JsonValue::CreateStringValue(settings.selectedKey));
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto bytes = Utf8(std::wstring(root.Stringify()));
        if (!output || !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !output.flush())
            throw Error(L"Could not write settings.");
    }
    Check(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
        L"Saving settings");
}

void Log(const std::wstring& message) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    try {
        const auto directory = DataDirectory();
        std::filesystem::create_directories(directory);
        const auto path = directory / L"errors.log";
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1000000)
            Check(MoveFileExW(path.c_str(), (directory / L"errors.previous.log").c_str(),
                MOVEFILE_REPLACE_EXISTING), L"Rotating log");
        std::ofstream output(path, std::ios::binary | std::ios::app);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t stamp[32]{};
        swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u ", time.wYear, time.wMonth,
            time.wDay, time.wHour, time.wMinute, time.wSecond);
        const auto text = Utf8(std::wstring(stamp) + message + L"\r\n");
        if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size())))
            OutputDebugStringW((L"Could not write RingLight log: " + message).c_str());
    } catch (const std::exception& error) {
        OutputDebugStringA(("Could not write RingLight log: " + std::string(error.what())).c_str());
    } catch (const winrt::hresult_error& error) {
        OutputDebugStringW(error.message().c_str());
    }
}

std::vector<std::wstring> SubnetCandidates(std::uint32_t address, unsigned prefixLength) {
    if (!prefixLength || prefixLength > 32) throw Error(L"Invalid IPv4 subnet.");
    if (prefixLength >= 31) return {};
    const auto mask = 0xffffffffu << (32 - prefixLength);
    const auto first = std::max((address & mask) + 1, (address & 0xffffff00u) + 1);
    const auto last = std::min((address | ~mask) - 1, (address & 0xffffff00u) + 254);
    std::vector<std::wstring> result;
    for (auto ip = first; ip <= last; ++ip) {
        if (ip == address) continue;
        IN_ADDR native{};
        native.s_addr = htonl(ip);
        wchar_t text[INET_ADDRSTRLEN]{};
        InetNtopW(AF_INET, &native, text, INET_ADDRSTRLEN);
        result.emplace_back(text);
    }
    return result;
}

DiscoveryResult Discover(std::stop_token stop) {
    DiscoveryResult result;
    auto* browse = new BrowseContext;
    browse->request.Version = DNS_QUERY_REQUEST_VERSION1;
    browse->request.QueryName = L"_elg._tcp.local";
    browse->request.pBrowseCallback = BrowseCallback;
    browse->request.pQueryContext = browse;
    const auto status = DnsServiceBrowse(&browse->request, &browse->cancel);
    if (status == DNS_REQUEST_PENDING) {
        for (int i = 0; i < 30 && !stop.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::lock_guard lock(browse->mutex);
            for (const auto& [service, target] : browse->services) {
                for (const auto& address : browse->addresses[target.first])
                    result.devices.push_back({address, target.second, L"Elgato light", service});
            }
            if (browse->error) result.notice = L"mDNS: " + ErrorText(browse->error);
        }
        const auto cancelled = DnsServiceBrowseCancel(&browse->cancel);
        if (cancelled != ERROR_SUCCESS) result.notice = L"Canceling mDNS: " + ErrorText(cancelled);
    } else {
        result.notice = L"mDNS: " + ErrorText(status);
        browse->Release();
    }
    browse->Release();
    if (!result.devices.empty() || stop.stop_requested()) return result;
    const auto candidates = LocalCandidates();
    for (std::size_t start = 0; start < candidates.size() && !stop.stop_requested(); start += 32) {
        std::vector<Socket> sockets;
        std::vector<std::wstring> addresses;
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        for (auto i = start; i < std::min(start + 32, candidates.size()); ++i) {
            Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (socket.value == INVALID_SOCKET) throw Error(L"Creating discovery socket: " + ErrorText(WSAGetLastError()));
            u_long nonBlocking = 1;
            if (ioctlsocket(socket.value, FIONBIO, &nonBlocking) != 0)
                throw Error(L"Configuring discovery socket: " + ErrorText(WSAGetLastError()));
            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_port = htons(9123);
            InetPtonW(AF_INET, candidates[i].c_str(), &endpoint.sin_addr);
            const auto connected = connect(socket.value, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint));
            if (connected == 0) {
                result.devices.push_back({candidates[i]});
                continue;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) continue;
            FD_SET(socket.value, &writable);
            FD_SET(socket.value, &errors);
            sockets.push_back(std::move(socket));
            addresses.push_back(candidates[i]);
        }
        if (sockets.empty()) continue;
        timeval timeout{0, 650000};
        const auto ready = select(0, nullptr, &writable, &errors, &timeout);
        if (ready == SOCKET_ERROR) throw Error(L"Waiting for discovery: " + ErrorText(WSAGetLastError()));
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            if (!FD_ISSET(sockets[i].value, &writable)) continue;
            int error = 0, length = sizeof(error);
            if (getsockopt(sockets[i].value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 &&
                error == 0) result.devices.push_back({addresses[i]});
        }
    }
    if (!result.devices.empty()) result.notice = L"Found using local-network discovery.";
    return result;
}
}
