#include "LightCore.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iostream>
#include <utility>

namespace {
const std::wstring Sample = LR"({"numberOfLights":1,"lights":[{"on":0,"brightness":3,"temperature":277}]})";

void Require(bool condition, const char* message = "Assertion failed.") {
    if (!condition) throw std::runtime_error(message);
}
template<class Action> void Reject(Action action) {
    try { action(); }
    catch (const light::Error&) { return; }
    throw std::runtime_error("Expected a validation error.");
}
std::string StateJson(const light::State& state) {
    return "{\"numberOfLights\":1,\"lights\":[{\"on\":" + std::to_string(state.on ? 1 : 0) +
        ",\"brightness\":" + std::to_string(state.brightness) + ",\"temperature\":" +
        std::to_string(state.temperature) + "}]}";
}

int Serve() {
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) throw light::Error(L"Test listener creation failed.");
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.sin_port = htons(19124);
    if (bind(listener, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener);
        throw light::Error(L"Cannot listen on test port 19124.");
    }
    std::cout << "FAKE_LIGHT_READY 127.0.0.1:19124" << std::endl;
    light::State state{false, 37, 222};
    bool offline = false, running = true;
    while (running) {
        const auto client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) { closesocket(listener); throw light::Error(L"Test accept failed."); }
        DWORD timeout = 2500;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        char buffer[4096];
        std::size_t bodyStart = std::string::npos, contentLength = 0;
        while (request.size() < 65536) {
            const auto received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) break;
            request.append(buffer, static_cast<std::size_t>(received));
            const auto headersEnd = request.find("\r\n\r\n");
            if (headersEnd == std::string::npos) continue;
            bodyStart = headersEnd + 4;
            auto headers = request.substr(0, headersEnd);
            std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            const auto position = headers.find("content-length:");
            if (position != std::string::npos)
                contentLength = std::strtoul(headers.c_str() + position + 15, nullptr, 10);
            if (request.size() >= bodyStart + contentLength) break;
        }
        int status = 200;
        std::string body;
        try {
            if (request.starts_with("GET /shutdown ")) running = false;
            else if (request.starts_with("GET /offline ")) offline = true;
            else if (request.starts_with("GET /online ")) offline = false;
            else if (request.starts_with("GET /state ")) body = StateJson(state);
            else if (offline) status = 503;
            else if (request.starts_with("GET /elgato/accessory-info "))
                body = R"({"productName":"Elgato Ring Light","displayName":"C++ Test Ring Light"})";
            else if (request.starts_with("GET /elgato/lights ")) body = StateJson(state);
            else if (request.starts_with("PUT /elgato/lights ")) {
                if (!contentLength || bodyStart == std::string::npos)
                    throw light::Error(L"Test firmware rejects missing Content-Length.");
                using namespace winrt::Windows::Data::Json;
                const auto patch = JsonObject::Parse(light::Wide(request.substr(bodyStart, contentLength)))
                    .GetNamedArray(L"lights").GetObjectAt(0);
                auto next = state;
                if (patch.HasKey(L"on")) next.on = patch.GetNamedNumber(L"on") == 1;
                if (patch.HasKey(L"brightness")) next.brightness = static_cast<int>(patch.GetNamedNumber(L"brightness"));
                if (patch.HasKey(L"temperature")) next.temperature = static_cast<int>(patch.GetNamedNumber(L"temperature"));
                state = light::ParseState(light::Wide(StateJson(next)));
                std::cout << "FAKE_PUT " << light::Utf8(std::wstring(patch.Stringify())) << std::endl;
                // Empty successful PUT response exercises firmware read-back handling.
            } else status = 404;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            status = 400;
        } catch (const winrt::hresult_error& error) {
            std::wcerr << error.message().c_str() << '\n';
            status = 400;
        }
        const auto response = "HTTP/1.1 " + std::to_string(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::size_t written = 0;
        while (written < response.size()) {
            const auto count = send(client, response.data() + written, static_cast<int>(response.size() - written), 0);
            if (count <= 0) break;
            written += static_cast<std::size_t>(count);
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    }
    closesocket(listener);
    return 0;
}

int Tests() {
    std::vector<std::pair<const char*, std::function<void()>>> tests;
    auto test = [&](const char* name, auto run) { tests.emplace_back(name, run); };
    test("Actual Ring Light state", [] { Require(light::ParseState(Sample) == light::State{false, 3, 277}); });
    test("Rejects malformed JSON", [] { Reject([] { light::ParseState(L"<html>not JSON</html>"); }); });
    test("Rejects multiple lights", [] { Reject([] { light::ParseState(LR"({"numberOfLights":2,"lights":[]})"); }); });
    test("Rejects missing temperature", [] { Reject([] {
        light::ParseState(LR"({"numberOfLights":1,"lights":[{"on":0,"brightness":3}]})");
    }); });
    test("Rejects invalid power", [] { Reject([] {
        light::ParseState(LR"({"numberOfLights":1,"lights":[{"on":2,"brightness":3,"temperature":277}]})");
    }); });
    test("Rejects non-integer brightness", [] { Reject([] {
        light::ParseState(LR"({"numberOfLights":1,"lights":[{"on":0,"brightness":3.5,"temperature":277}]})");
    }); });
    test("Rejects string brightness", [] { Reject([] {
        light::ParseState(LR"({"numberOfLights":1,"lights":[{"on":0,"brightness":"3","temperature":277}]})");
    }); });
    test("Rejects null light", [] { Reject([] { light::ParseState(LR"({"numberOfLights":1,"lights":[null]})"); }); });
    test("Rejects invalid temperature", [] { Reject([] {
        light::ParseState(LR"({"numberOfLights":1,"lights":[{"on":0,"brightness":3,"temperature":0}]})");
    }); });
    test("Zero brightness is valid", [] {
        Require(light::ParseState(LR"({"numberOfLights":1,"lights":[{"on":0,"brightness":0,"temperature":143}]})").brightness == 0);
    });
    test("Default port", [] { Require(light::ParseAddress(L"192.0.2.1").port == 9123); });
    test("Custom port", [] { Require(light::ParseAddress(L"127.0.0.1:19124").port == 19124); });
    test("Explicit port 80", [] { Require(light::ParseAddress(L"192.0.2.1:80").port == 80); });
    test("Hostname", [] { Require(light::ParseAddress(L"ring-light.local").address == L"ring-light.local"); });
    test("IPv6", [] { Require(light::ParseAddress(L"[::1]:9124").address == L"::1"); });
    test("Rejects empty address", [] { Reject([] { light::ParseAddress(L" "); }); });
    test("Rejects HTTPS", [] { Reject([] { light::ParseAddress(L"https://192.0.2.1"); }); });
    test("Rejects URL path", [] { Reject([] { light::ParseAddress(L"192.0.2.1/elgato/lights"); }); });
    test("Rejects credentials", [] { Reject([] { light::ParseAddress(L"http://user:pass@192.0.2.1"); }); });
    test("Rejects URL query", [] { Reject([] { light::ParseAddress(L"192.0.2.1?x=1"); }); });
    test("Warm mired conversion", [] { Require(light::ToMired(2900) == 344); });
    test("Cool mired conversion", [] { Require(light::ToMired(7000) == 143); });
    test("Neutral conversion", [] { Require(light::ToMired(4500) == 222); });
    test("Actual temperature display", [] { Require(light::ToKelvin(277) == 3600); });
    test("Invalid Kelvin rejected", [] { Reject([] { light::ToMired(0); }); });
    test("Invalid mired rejected", [] { Reject([] { light::ToKelvin(0); }); });
    test("Sparse change JSON", [] {
        Require(light::ChangeJson({{}, 50}) == R"({"numberOfLights":1,"lights":[{"brightness":50}]})");
    });
    test("False and zero are not missing fields", [] {
        Require(light::ChangeJson({false, 0}) == R"({"numberOfLights":1,"lights":[{"on":0,"brightness":0}]})");
    });
    test("Empty change rejected", [] { Reject([] { light::ChangeJson({}); }); });
    test("Invalid change rejected", [] { Reject([] { light::ChangeJson({{}, 101}); }); });
    test("Merged changes preserve untouched settings", [] {
        light::Change change{true, 20, 200};
        change.Merge({{}, 70});
        Require(change.on == true && change.brightness == 70 && change.temperature == 200);
    });
    test("Bounded /24 discovery", [] {
        const auto addresses = light::SubnetCandidates(0xc0000230, 24);
        Require(addresses.size() == 253);
        Require(std::find(addresses.begin(), addresses.end(), L"192.0.2.48") == addresses.end());
        Require(std::find(addresses.begin(), addresses.end(), L"192.0.2.101") != addresses.end());
        Require(addresses.front() == L"192.0.2.1" && addresses.back() == L"192.0.2.254");
    });
    test("Respects /25 boundaries", [] {
        const auto addresses = light::SubnetCandidates(0xc0000281, 25);
        Require(addresses.size() == 125 && addresses.front() == L"192.0.2.130");
    });
    test("Large networks capped to /24", [] {
        const auto addresses = light::SubnetCandidates(0x0a050607, 8);
        Require(addresses.size() == 253 && addresses.front() == L"10.5.6.1" && addresses.back() == L"10.5.6.254");
    });
    test("No point-to-point scan", [] { Require(light::SubnetCandidates(0xc0000202, 31).empty()); });
    test("Invalid prefix rejected", [] { Reject([] { light::SubnetCandidates(0xc0000202, 0); }); });
    test("UTF-8 round trip", [] { Require(light::Wide(light::Utf8(L"Light \u00e9")) == L"Light \u00e9"); });
    test("Invalid UTF-8 rejected", [] {
        try { light::Wide("\xff"); } catch (const std::runtime_error&) { return; }
        throw std::runtime_error("Expected UTF-8 error.");
    });
    test("GlowTray app data directory", [] { Require(light::DataDirectory().filename() == L"GlowTray"); });
    test("Settings round trip, replacement, Unicode and nullable service ID", [] {
        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        const auto directory = std::filesystem::path(temporary) /
            (L"GlowTray-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(directory);
        const auto path = directory / L"settings.json";
        try {
            Require(light::LoadSettings(path).devices.empty());
            light::Settings settings{{{L"127.0.0.1", 19124, L"Light \u00e9"}}, L"127.0.0.1:19124"};
            light::SaveSettings(path, settings);
            Require(light::LoadSettings(path).devices == settings.devices);
            settings.selectedKey.clear();
            light::SaveSettings(path, settings);
            Require(light::LoadSettings(path).selectedKey.empty());
            Require(!std::filesystem::exists(path.wstring() + L".tmp"));
            {
                std::ofstream file(path);
                file << R"({"Devices":[{"Address":"127.0.0.1","Port":19124,"Name":"Old light","ServiceId":null}],"SelectedKey":null})";
            }
            Require(light::LoadSettings(path).devices[0].serviceId.empty());
            {
                std::ofstream file(path);
                file << R"({"Devices":null})";
            }
            Reject([&] { light::LoadSettings(path); });
        } catch (...) {
            std::filesystem::remove(path);
            std::filesystem::remove(directory);
            throw;
        }
        std::filesystem::remove(path);
        std::filesystem::remove(directory);
    });
    int failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
        catch (const winrt::hresult_error& error) { ++failed; std::wcerr << L"FAIL: " << error.message().c_str() << '\n'; }
    }
    std::cout << tests.size() - failed << '/' << tests.size() << " native tests passed.\n";
    return failed ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    int code = 0;
    try {
        winrt::init_apartment();
        if (argc == 2 && std::wstring(argv[1]) == L"--serve") code = Serve();
        else if (argc == 2 && std::wstring(argv[1]) == L"--integration") {
            const auto device = light::ParseAddress(L"127.0.0.1:19124");
            const auto initial = light::Probe(device);
            Require(initial.device.name == L"C++ Test Ring Light");
            Require(light::Write(device, {true, 64, 179}) == light::State{true, 64, 179});
            Require(light::Write(device, {initial.state.on, initial.state.brightness, initial.state.temperature}) == initial.state);
            std::cout << "PASS Native WinHTTP integration: fixed Content-Length, sparse writes and empty PUT read-back.\n";
        } else if (argc >= 3 && std::wstring(argv[1]) == L"--live") {
            const auto device = light::ParseAddress(argv[2]);
            const auto probe = light::Probe(device);
            std::wcout << L"LIVE " << probe.device.name << L": on=" << probe.state.on << L", brightness=" <<
                probe.state.brightness << L", mired=" << probe.state.temperature << L'\n';
            if (argc == 4 && std::wstring(argv[3]) == L"--verify-write") {
                Require(light::Write(device, {{}, probe.state.brightness}) == probe.state);
                std::cout << "PASS Real no-op write; full state unchanged.\n";
            }
        } else if (argc == 2 && std::wstring(argv[1]) == L"--discover") {
            const auto result = light::Discover();
            for (const auto& device : result.devices) std::wcout << L"FOUND " << device.Key() << L'\n';
            std::wcout << result.notice << L'\n';
            code = result.devices.empty() ? 2 : 0;
        } else code = Tests();
        winrt::uninit_apartment();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; code = 1; }
    catch (const winrt::hresult_error& error) { std::wcerr << error.message().c_str() << '\n'; code = 1; }
    WSACleanup();
    return code;
}
