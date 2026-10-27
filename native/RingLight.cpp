#include "LightCore.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {
constexpr wchar_t WindowClass[] = L"RingLight.Native.Flyout";
constexpr UINT ResultMessage = WM_APP + 1, TrayMessage = WM_APP + 2, ShowMessage = WM_APP + 3;
constexpr UINT TickTimer = 1, PollTimer = 2, DiscoverTimer = 3;
constexpr int Choice = 1001, Power = 1002, Brightness = 1003, Temperature = 1004,
    Warm = 1005, Neutral = 1006, Daylight = 1007, DiscoverButton = 1008, AddButton = 1009,
    Status = 1010, AddressLabel = 1011, BrightnessValue = 1012, TemperatureValue = 1013,
    CloseButton = 1014;
constexpr int MenuOpen = 2001, MenuDiscover = 2002, MenuAdd = 2003, MenuStartup = 2004,
    MenuFolder = 2005, MenuForget = 2006, MenuExit = 2007;
constexpr COLORREF Background = RGB(16, 23, 34), Panel = RGB(30, 43, 61),
    Text = RGB(241, 245, 249), Muted = RGB(154, 171, 192), Accent = RGB(135, 230, 188),
    Warning = RGB(255, 166, 153);
constexpr GUID TrayGuid = {0xdad7e130, 0x88aa, 0x489c, {0xb3, 0xf6, 0x2a, 0x0d, 0xbc, 0x87, 0x49, 0x71}};

enum class Kind { Probe, Read, Write, Discovery };
struct Job {
    Kind kind = Kind::Read;
    light::Device device;
    light::Change change;
    std::uint64_t revision = 0;
    bool select = false;
    bool notify = false;
};
struct Result {
    Job job;
    light::State state;
    light::DiscoveryResult discovery;
    std::wstring error;
};

void PostResult(HWND window, std::unique_ptr<Result> result) {
    if (PostMessageW(window, ResultMessage, 0, reinterpret_cast<LPARAM>(result.get())))
        result.release();
}

class Worker {
    HWND window_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::jthread thread_;
public:
    explicit Worker(HWND window) : window_(window), thread_([this] { Run(); }) {}
    ~Worker() { Finish(); }
    void Add(Job job) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        const auto key = job.device.Key();
        if (job.kind == Kind::Write) {
            std::erase_if(jobs_, [&](const Job& queued) {
                return queued.kind == Kind::Read && queued.device.Key() == key;
            });
            for (auto& queued : jobs_) {
                if (queued.kind == Kind::Write && queued.device.Key() == key) {
                    queued.change.Merge(job.change);
                    queued.revision = job.revision;
                    return;
                }
            }
            jobs_.push_front(std::move(job));
        } else {
            for (auto& queued : jobs_) {
                if (queued.device.Key() == key && queued.kind != Kind::Write) {
                    queued.select = queued.select || job.select;
                    queued.notify = queued.notify || job.notify;
                    return;
                }
            }
            jobs_.push_back(std::move(job));
        }
        ready_.notify_one();
    }
    void DropWrites(const std::wstring& key) {
        std::lock_guard lock(mutex_);
        std::erase_if(jobs_, [&](const Job& job) {
            return job.kind == Kind::Write && job.device.Key() == key;
        });
    }
    void Finish() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            std::erase_if(jobs_, [](const Job& job) { return job.kind != Kind::Write; });
        }
        ready_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
private:
    void Run() {
        try {
            winrt::init_apartment();
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                    if (jobs_.empty() && stopping_) break;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                auto result = std::make_unique<Result>();
                result->job = job;
                try {
                    if (job.kind == Kind::Probe) {
                        const auto probe = light::Probe(job.device);
                        result->job.device = probe.device;
                        result->state = probe.state;
                    } else if (job.kind == Kind::Read) {
                        result->state = light::Read(job.device);
                    } else if (job.kind == Kind::Write) {
                        result->state = light::Write(job.device, job.change);
                    }
                } catch (const std::exception& error) {
                    result->error = light::Describe(error);
                } catch (const winrt::hresult_error& error) {
                    result->error = error.message();
                }
                PostResult(window_, std::move(result));
            }
            winrt::uninit_apartment();
        } catch (const winrt::hresult_error& error) {
            auto result = std::make_unique<Result>();
            result->error = L"Networking worker initialization failed: " + std::wstring(error.message());
            result->job.notify = true;
            PostResult(window_, std::move(result));
        }
    }
};

struct Light {
    light::Device device;
    light::State actual, desired;
    bool hasState = false;
    bool online = false;
    std::uint64_t revision = 0;
};
struct Pending { Job job; ULONGLONG changed; };

class App {
    HINSTANCE instance_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    HFONT font_ = nullptr, titleFont_ = nullptr, smallFont_ = nullptr;
    HBRUSH background_ = CreateSolidBrush(Background), panel_ = CreateSolidBrush(Panel);
    std::vector<Light> lights_;
    std::map<std::wstring, Pending> pending_;
    std::wstring selected_, status_ = L"Looking for lights on your Wi-Fi...";
    bool error_ = false, discovering_ = false, exiting_ = false, shutdown_ = false;
    int modal_ = 0;
    UINT dpi_ = 96, taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    ULONGLONG hiddenAt_ = 0;
    std::unique_ptr<Worker> worker_;
    std::jthread discovery_;
    std::filesystem::path settingsPath_;
    NOTIFYICONDATAW tray_{};
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    ~App() {
        Stop();
        if (font_) DeleteObject(font_);
        if (titleFont_) DeleteObject(titleFont_);
        if (smallFont_) DeleteObject(smallFont_);
        if (icon_) DestroyIcon(icon_);
        DeleteObject(background_);
        DeleteObject(panel_);
    }
    HWND Window() const { return window_; }

    void Create() {
        WNDCLASSEXW type{sizeof(type)};
        type.hInstance = instance_;
        type.lpfnWndProc = WindowProc;
        type.lpszClassName = WindowClass;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.style = CS_DROPSHADOW;
        if (!RegisterClassExW(&type)) throw light::Error(L"Registering window: " + light::ErrorText(GetLastError()));
        icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, 0));
        if (!icon_) throw light::Error(L"Loading application icon failed.");
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, WindowClass, L"RingLight Native",
            WS_POPUP | WS_CLIPCHILDREN, 0, 0, 368, 520, nullptr, nullptr, instance_, this);
        if (!window_) throw light::Error(L"Creating popup: " + light::ErrorText(GetLastError()));
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
        const DWORD rounded = 2;
        DwmSetWindowAttribute(window_, 33, &rounded, sizeof(rounded));
        CreateControls();
        dpi_ = GetDpiForWindow(window_);
        Fonts();
        Layout();
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID | NIF_SHOWTIP;
        tray_.guidItem = TrayGuid;
        tray_.uCallbackMessage = TrayMessage;
        tray_.hIcon = icon_;
        wcscpy_s(tray_.szTip, L"RingLight Native - click for controls");
        AddTray();
    }

    void Start(const std::vector<std::wstring>& arguments) {
        settingsPath_ = light::DataDirectory() / L"settings.json";
        light::Settings settings;
        bool imported = false;
        try {
            if (std::filesystem::exists(settingsPath_)) settings = light::LoadSettings(settingsPath_);
            else {
                const auto historical = settingsPath_.parent_path().parent_path() / L"RingLight" / L"settings.json";
                if (std::filesystem::exists(historical)) {
                    try {
                        settings = light::LoadSettings(historical);
                        imported = true;
                    } catch (const std::exception& error) {
                        light::Log(L"C# settings import skipped: " + light::Describe(error));
                        SetStatus(L"Could not import old settings; discovering lights instead.", true);
                    }
                }
            }
        } catch (const std::exception& error) {
            light::Log(light::Describe(error));
            if (std::filesystem::exists(settingsPath_)) {
                const auto backup = settingsPath_.wstring() + L".invalid-" + std::to_wstring(GetTickCount64());
                if (!MoveFileW(settingsPath_.c_str(), backup.c_str()))
                    throw light::Error(L"Cannot preserve invalid settings: " + light::ErrorText(GetLastError()));
            }
            SetStatus(L"Invalid settings preserved. Rediscovering lights.", true);
        }
        selected_ = settings.selectedKey;
        for (auto& device : settings.devices) lights_.push_back({std::move(device)});
        RefreshChoice();
        worker_ = std::make_unique<Worker>(window_);
        for (const auto& entry : lights_) worker_->Add({Kind::Probe, entry.device, {}, 0, false, true});
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (arguments[i] == L"--add") {
                if (i + 1 == arguments.size()) throw light::Error(L"--add requires an address.");
                worker_->Add({Kind::Probe, light::ParseAddress(arguments[++i]), {}, 0, true, true});
            }
        }
        if (imported) Save();
        SetTimer(window_, PollTimer, 5000, nullptr);
        SetTimer(window_, DiscoverTimer, 45000, nullptr);
        StartDiscovery();
        Render();
        if (std::find(arguments.begin(), arguments.end(), L"--tray") == arguments.end()) ShowPopup();
    }

    void Stop() {
        if (shutdown_) return;
        shutdown_ = true;
        KillTimer(window_, TickTimer);
        KillTimer(window_, PollTimer);
        KillTimer(window_, DiscoverTimer);
        if (discovery_.joinable()) { discovery_.request_stop(); discovery_.join(); }
        Flush(true);
        if (worker_) worker_->Finish();
        if (window_) {
            MSG message{};
            while (PeekMessageW(&message, window_, ResultMessage, ResultMessage, PM_REMOVE))
                delete reinterpret_cast<Result*>(message.lParam);
        }
        Shell_NotifyIconW(NIM_DELETE, &tray_);
    }

private:
    int S(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    HWND Control(int id) const { return GetDlgItem(window_, id); }
    Light* Selected() {
        const auto found = std::find_if(lights_.begin(), lights_.end(), [&](const Light& light) {
            return light.device.Key() == selected_;
        });
        return found == lights_.end() ? nullptr : &*found;
    }
    void AddTray() {
        if (!Shell_NotifyIconW(NIM_ADD, &tray_))
            throw light::Error(L"Adding tray icon: " + light::ErrorText(GetLastError()));
        tray_.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &tray_))
            throw light::Error(L"Configuring tray icon: " + light::ErrorText(GetLastError()));
    }
    HWND Make(const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        const auto control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!control) throw light::Error(L"Creating a control: " + light::ErrorText(GetLastError()));
        return control;
    }
    void CreateControls() {
        Make(WC_COMBOBOXW, L"Your lights", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, Choice);
        Make(WC_STATICW, L"No light selected", SS_LEFT, AddressLabel);
        Make(WC_BUTTONW, L"Turn light on", BS_OWNERDRAW | WS_TABSTOP, Power);
        Make(TRACKBAR_CLASSW, L"Brightness", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, Brightness);
        Make(TRACKBAR_CLASSW, L"Colour temperature", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, Temperature);
        Make(WC_STATICW, L"-- %", SS_RIGHT, BrightnessValue);
        Make(WC_STATICW, L"-- K", SS_RIGHT, TemperatureValue);
        Make(WC_BUTTONW, L"Warm", BS_OWNERDRAW | WS_TABSTOP, Warm);
        Make(WC_BUTTONW, L"Neutral", BS_OWNERDRAW | WS_TABSTOP, Neutral);
        Make(WC_BUTTONW, L"Daylight", BS_OWNERDRAW | WS_TABSTOP, Daylight);
        Make(WC_BUTTONW, L"Discover", BS_OWNERDRAW | WS_TABSTOP, DiscoverButton);
        Make(WC_BUTTONW, L"Add by IP", BS_OWNERDRAW | WS_TABSTOP, AddButton);
        Make(WC_BUTTONW, L"x", BS_OWNERDRAW | WS_TABSTOP, CloseButton);
        Make(WC_STATICW, status_.c_str(), SS_LEFT, Status);
        SendMessageW(Control(Brightness), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(Control(Brightness), TBM_SETPAGESIZE, 0, 10);
        SendMessageW(Control(Temperature), TBM_SETRANGE, TRUE, MAKELONG(0, 82));
        SendMessageW(Control(Temperature), TBM_SETPAGESIZE, 0, 10);
        for (const auto id : {Brightness, Temperature}) {
            if (!SetWindowSubclass(Control(id), SliderProc, 1, reinterpret_cast<DWORD_PTR>(this)))
                throw light::Error(L"Initializing slider rendering failed.");
        }
    }
    void Fonts() {
        if (font_) DeleteObject(font_);
        if (titleFont_) DeleteObject(titleFont_);
        if (smallFont_) DeleteObject(smallFont_);
        auto font = [&](int size, int weight) {
            return CreateFontW(-S(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        font_ = font(14, FW_NORMAL);
        titleFont_ = font(21, FW_SEMIBOLD);
        smallFont_ = font(11, FW_NORMAL);
        if (!font_ || !titleFont_ || !smallFont_) throw light::Error(L"Creating interface fonts failed.");
        for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(Control(Status), WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        SendMessageW(Control(AddressLabel), WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    void Layout() {
        auto move = [&](int id, int x, int y, int width, int height) {
            MoveWindow(Control(id), S(x), S(y), S(width), S(height), TRUE);
        };
        move(CloseButton, 320, 18, 28, 27);
        move(Choice, 20, 72, 328, 200);
        move(AddressLabel, 23, 105, 320, 19);
        move(Power, 218, 145, 128, 37);
        move(BrightnessValue, 244, 211, 100, 23);
        move(Brightness, 17, 239, 334, 32);
        move(TemperatureValue, 244, 295, 100, 23);
        move(Temperature, 17, 323, 334, 32);
        move(Warm, 20, 390, 104, 35);
        move(Neutral, 132, 390, 104, 35);
        move(Daylight, 244, 390, 104, 35);
        move(AddButton, 20, 443, 158, 33);
        move(DiscoverButton, 190, 443, 158, 33);
        move(Status, 23, 490, 322, 35);
        SetWindowPos(window_, nullptr, 0, 0, S(368), S(540), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window_, nullptr, TRUE);
    }
    RECT TrayRect() const {
        NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
        identifier.hWnd = window_;
        identifier.uID = 1;
        identifier.guidItem = TrayGuid;
        RECT rectangle{};
        if (FAILED(Shell_NotifyIconGetRect(&identifier, &rectangle))) {
            POINT cursor{};
            GetCursorPos(&cursor);
            rectangle = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
        }
        return rectangle;
    }
    void ShowPopup() {
        if (exiting_) return;
        const auto icon = TrayRect();
        if (MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST) != MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST)) {
            // Enter the tray's monitor while hidden so WM_DPICHANGED runs before measuring the flyout.
            SetWindowPos(window_, nullptr, icon.left, icon.top, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        const POINT anchor{icon.right, icon.top - S(8)};
        const SIZE size{S(368), S(540)};
        RECT location{};
        auto exclude = icon;
        if (!CalculatePopupWindowPosition(&anchor, &size, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_WORKAREA,
            &exclude, &location))
            throw light::Error(L"Positioning popup: " + light::ErrorText(GetLastError()));
        SetWindowPos(window_, HWND_TOPMOST, location.left, location.top, size.cx, size.cy, SWP_SHOWWINDOW);
        SetForegroundWindow(window_);
        SetFocus(Control(Choice));
    }
    void HidePopup() {
        if (IsWindowVisible(window_)) {
            ShowWindow(window_, SW_HIDE);
            hiddenAt_ = GetTickCount64();
        }
    }
    void RefreshChoice() {
        SendMessageW(Control(Choice), CB_RESETCONTENT, 0, 0);
        if (lights_.empty()) {
            SendMessageW(Control(Choice), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No lights found yet"));
            SendMessageW(Control(Choice), CB_SETCURSEL, 0, 0);
            selected_.clear();
        } else {
            int selected = -1;
            for (std::size_t i = 0; i < lights_.size(); ++i) {
                SendMessageW(Control(Choice), CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(lights_[i].device.name.c_str()));
                if (lights_[i].device.Key() == selected_) selected = static_cast<int>(i);
            }
            if (selected < 0) { selected = 0; selected_ = lights_[0].device.Key(); }
            SendMessageW(Control(Choice), CB_SETCURSEL, selected, 0);
        }
        EnableWindow(Control(Choice), !lights_.empty());
    }
    void Render() {
        auto* entry = Selected();
        const bool enabled = entry && entry->online;
        for (const auto id : {Power, Brightness, Temperature, Warm, Neutral, Daylight})
            EnableWindow(Control(id), enabled);
        SetWindowTextW(Control(AddressLabel), entry ? entry->device.Key().c_str() : L"No light selected");
        if (entry && entry->hasState) {
            SetWindowTextW(Control(Power), entry->desired.on ? L"Turn light off" : L"Turn light on");
            SendMessageW(Control(Brightness), TBM_SETPOS, TRUE, entry->desired.brightness);
            SendMessageW(Control(Temperature), TBM_SETPOS, TRUE, (light::ToKelvin(entry->desired.temperature) - 2900) / 50);
            SetWindowTextW(Control(BrightnessValue), (std::to_wstring(entry->desired.brightness) + L"%").c_str());
            SetWindowTextW(Control(TemperatureValue), (std::to_wstring(light::ToKelvin(entry->desired.temperature)) + L" K").c_str());
        } else {
            SetWindowTextW(Control(Power), L"Not connected");
            SetWindowTextW(Control(BrightnessValue), L"-- %");
            SetWindowTextW(Control(TemperatureValue), L"-- K");
        }
        const auto tooltip = entry ? L"RingLight - " + entry->device.name +
            (entry->online ? (entry->desired.on ? L" (on)" : L" (off)") : L" (offline)") : L"RingLight Native";
        wcsncpy_s(tray_.szTip, tooltip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
        InvalidateRect(window_, nullptr, FALSE);
        InvalidateRect(Control(Power), nullptr, TRUE);
    }
    void SetStatus(const std::wstring& text, bool error = false) {
        status_ = text;
        error_ = error;
        SetWindowTextW(Control(Status), text.c_str());
        InvalidateRect(Control(Status), nullptr, TRUE);
    }
    void Save() {
        if (settingsPath_.empty()) return;
        try {
            light::Settings settings;
            for (const auto& entry : lights_) settings.devices.push_back(entry.device);
            settings.selectedKey = selected_;
            light::SaveSettings(settingsPath_, settings);
        } catch (const std::exception& error) {
            const auto text = L"Settings could not be saved: " + light::Describe(error);
            light::Log(text);
            SetStatus(text, true);
        } catch (const winrt::hresult_error& error) {
            light::Log(error.message().c_str());
            SetStatus(L"Settings could not be saved: " + std::wstring(error.message()), true);
        }
    }
    void Edit(light::Change change, bool immediate = false) {
        auto* entry = Selected();
        if (!entry || !entry->online || exiting_) return;
        if (change.on) entry->desired.on = *change.on;
        if (change.brightness) entry->desired.brightness = *change.brightness;
        if (change.temperature) entry->desired.temperature = *change.temperature;
        ++entry->revision;
        const auto key = entry->device.Key();
        auto [position, inserted] = pending_.try_emplace(key, Pending{
            {Kind::Write, entry->device, {}, entry->revision, false, true}, GetTickCount64()});
        (void)inserted;
        position->second.job.change.Merge(change);
        position->second.job.revision = entry->revision;
        position->second.changed = immediate ? 0 : GetTickCount64();
        SetTimer(window_, TickTimer, 180, nullptr);
        SetStatus(L"Applying settings...");
        Render();
        if (immediate) Flush();
    }
    void Flush(bool all = false) {
        if (!worker_) return;
        const auto now = GetTickCount64();
        for (auto item = pending_.begin(); item != pending_.end();) {
            if (all || now - item->second.changed >= 180) {
                worker_->Add(std::move(item->second.job));
                item = pending_.erase(item);
            } else ++item;
        }
        if (pending_.empty()) KillTimer(window_, TickTimer);
    }
    void StartDiscovery() {
        if (discovering_ || exiting_) return;
        if (discovery_.joinable()) discovery_.join();
        discovering_ = true;
        EnableWindow(Control(DiscoverButton), FALSE);
        SetWindowTextW(Control(DiscoverButton), L"Searching...");
        if (lights_.empty()) SetStatus(L"Searching your Wi-Fi for Elgato lights...");
        discovery_ = std::jthread([window = window_](std::stop_token stop) {
            auto result = std::make_unique<Result>();
            result->job.kind = Kind::Discovery;
            try { result->discovery = light::Discover(stop); }
            catch (const std::exception& error) { result->error = light::Describe(error); }
            PostResult(window, std::move(result));
        });
    }
    void Receive(std::unique_ptr<Result> result) {
        if (exiting_) return;
        if (result->job.kind == Kind::Discovery) {
            discovering_ = false;
            EnableWindow(Control(DiscoverButton), TRUE);
            SetWindowTextW(Control(DiscoverButton), L"Discover");
            if (!result->error.empty()) {
                light::Log(L"Discovery: " + result->error);
                SetStatus(L"Discovery failed: " + result->error, true);
                return;
            }
            for (const auto& candidate : result->discovery.devices) {
                auto existing = std::find_if(lights_.begin(), lights_.end(), [&](const Light& entry) {
                    return entry.device.Key() == candidate.Key();
                });
                if (existing == lights_.end() || !existing->online)
                    worker_->Add({Kind::Probe, candidate});
                else if (!candidate.serviceId.empty() && candidate.serviceId != existing->device.serviceId) {
                    existing->device.serviceId = candidate.serviceId;
                    Save();
                }
            }
            if (result->discovery.devices.empty() && lights_.empty())
                SetStatus(result->discovery.notice.empty()
                    ? L"No lights found. Check Wi-Fi, or add by IP." : result->discovery.notice, true);
            else if (!result->discovery.notice.empty()) SetStatus(result->discovery.notice);
            return;
        }
        const auto key = result->job.device.Key();
        auto entry = std::find_if(lights_.begin(), lights_.end(), [&](const Light& candidate) {
            return candidate.device.Key() == key || (!result->job.device.serviceId.empty() &&
                candidate.device.serviceId == result->job.device.serviceId);
        });
        if (!result->error.empty()) {
            light::Log(key + L": " + result->error);
            if (entry != lights_.end()) {
                entry->online = false;
                entry->desired = entry->actual;
                pending_.erase(key);
                worker_->DropWrites(key);
                Render();
            }
            if (result->job.notify || selected_ == key)
                SetStatus((result->job.kind == Kind::Write
                    ? L"Write failed; state unconfirmed. " : L"Cannot connect. ") +
                    result->error + L" Retrying automatically.", true);
            return;
        }
        bool changed = false;
        if (entry == lights_.end()) {
            if (result->job.kind != Kind::Probe) return; // A forgotten light must not be re-added by a late read.
            if (lights_.size() >= 64) { SetStatus(L"Saved-light limit reached.", true); return; }
            lights_.push_back({result->job.device});
            entry = std::prev(lights_.end());
            changed = true;
        }
        if (entry->device != result->job.device && result->job.kind == Kind::Probe) {
            const auto oldKey = entry->device.Key();
            if (selected_ == oldKey) selected_ = key;
            pending_.erase(oldKey);
            entry->device = result->job.device;
            changed = true;
        }
        const bool wasOnline = entry->online;
        entry->actual = result->state;
        entry->hasState = true;
        entry->online = true;
        if (entry->revision == result->job.revision) entry->desired = result->state;
        if (result->job.select || selected_.empty()) { selected_ = key; changed = true; }
        if (changed) { RefreshChoice(); Save(); }
        if (selected_ == key && !pending_.contains(key) && entry->revision == result->job.revision)
            SetStatus(result->job.kind == Kind::Write ? L"Settings applied." :
                (wasOnline ? L"Connected. Local control only." : L"Connected to " + entry->device.name + L"."));
        Render();
    }
    void Poll() {
        if (!worker_ || exiting_) return;
        // Poll the selected light first; reconnect other remembered devices without blocking the UI.
        if (auto* entry = Selected(); entry && !pending_.contains(entry->device.Key()))
            worker_->Add({entry->hasState ? Kind::Read : Kind::Probe, entry->device, {}, entry->revision});
        for (const auto& entry : lights_) {
            if (entry.device.Key() != selected_ && !pending_.contains(entry.device.Key()))
                worker_->Add({entry.hasState ? Kind::Read : Kind::Probe, entry.device, {}, entry.revision});
        }
    }
    bool StartupEnabled() const {
        DWORD type = 0, size = 0;
        const auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"RingLightNative", RRF_RT_REG_SZ, &type, nullptr, &size);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
            throw light::Error(L"Reading startup preference: " + light::ErrorText(result));
        return result == ERROR_SUCCESS;
    }
    void ToggleStartup() {
        HKEY key = nullptr;
        auto status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) throw light::Error(L"Updating startup: " + light::ErrorText(status));
        const bool enabled = StartupEnabled();
        if (enabled) status = RegDeleteValueW(key, L"RingLightNative");
        else {
            wchar_t path[32768]{};
            if (!GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)))) {
                RegCloseKey(key);
                throw light::Error(L"Cannot locate this executable.");
            }
            const auto value = L"\"" + std::wstring(path) + L"\" --tray";
            status = RegSetValueExW(key, L"RingLightNative", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key);
        if (status != ERROR_SUCCESS) throw light::Error(L"Updating startup: " + light::ErrorText(status));
        SetStatus(enabled ? L"Start with Windows disabled." : L"Will start in the tray with Windows.");
    }
    void Menu() {
        const HMENU menu = CreatePopupMenu();
        if (!menu) throw light::Error(L"Creating tray menu failed.");
        AppendMenuW(menu, MF_STRING, MenuOpen, L"Open controls");
        AppendMenuW(menu, MF_STRING, MenuDiscover, L"Discover lights");
        AppendMenuW(menu, MF_STRING, MenuAdd, L"Add by IP address...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), MenuStartup, L"Start with Windows");
        AppendMenuW(menu, MF_STRING, MenuFolder, L"Open data folder");
        AppendMenuW(menu, MF_STRING | (Selected() ? 0 : MF_GRAYED), MenuForget, L"Forget selected light");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuExit, L"Exit");
        POINT cursor{};
        GetCursorPos(&cursor);
        ++modal_;
        SetForegroundWindow(window_);
        const auto command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            cursor.x, cursor.y, window_, nullptr);
        --modal_;
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        Command(command);
    }
    struct AddressInput { light::Device device; App* app; };
    static INT_PTR CALLBACK AddressProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_INITDIALOG) {
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
            SendDlgItemMessageW(dialog, 3001, EM_SETLIMITTEXT, 253, 0);
            return TRUE;
        }
        if (message == WM_COMMAND) {
            if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, 0); return TRUE; }
            if (LOWORD(wParam) == IDOK) {
                wchar_t address[256]{};
                GetDlgItemTextW(dialog, 3001, address, static_cast<int>(std::size(address)));
                try {
                    auto* input = reinterpret_cast<AddressInput*>(GetWindowLongPtrW(dialog, DWLP_USER));
                    input->device = light::ParseAddress(address);
                    EndDialog(dialog, 1);
                } catch (const std::exception& error) {
                    MessageBoxW(dialog, light::Describe(error).c_str(), L"Invalid address", MB_OK | MB_ICONWARNING);
                }
                return TRUE;
            }
        }
        return FALSE;
    }
    void AddAddress() {
        AddressInput input{{}, this};
        ++modal_;
        const auto result = DialogBoxParamW(instance_, MAKEINTRESOURCEW(102), window_, AddressProc,
            reinterpret_cast<LPARAM>(&input));
        --modal_;
        if (result == -1) throw light::Error(L"Opening address dialog: " + light::ErrorText(GetLastError()));
        if (result == 1) {
            SetStatus(L"Connecting to " + input.device.address + L"...");
            worker_->Add({Kind::Probe, input.device, {}, 0, true, true});
        }
        ShowPopup();
    }
    void Command(UINT command) {
        try {
            switch (command) {
            case Power: if (auto* entry = Selected()) Edit({!entry->desired.on}, true); break;
            case Warm: Edit({{}, {}, light::ToMired(3200)}); break;
            case Neutral: Edit({{}, {}, light::ToMired(4500)}); break;
            case Daylight: Edit({{}, {}, light::ToMired(5600)}); break;
            case DiscoverButton: case MenuDiscover: StartDiscovery(); break;
            case AddButton: case MenuAdd: AddAddress(); break;
            case MenuOpen: ShowPopup(); break;
            case CloseButton: case IDCANCEL: HidePopup(); break;
            case MenuStartup: ToggleStartup(); break;
            case MenuFolder: {
                std::filesystem::create_directories(settingsPath_.parent_path());
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open",
                    settingsPath_.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32) throw light::Error(L"Could not open the data folder.");
                break;
            }
            case MenuForget:
                if (auto* entry = Selected()) {
                    const auto key = entry->device.Key();
                    pending_.erase(key);
                    worker_->DropWrites(key);
                    std::erase_if(lights_, [&](const Light& value) { return value.device.Key() == key; });
                    selected_.clear();
                    RefreshChoice();
                    Save();
                    Render();
                    SetStatus(L"Light forgotten. Discovery can find it again.");
                }
                break;
            case MenuExit:
                exiting_ = true;
                HidePopup();
                Stop();
                DestroyWindow(window_);
                break;
            }
        } catch (const std::exception& error) {
            light::Log(light::Describe(error));
            SetStatus(light::Describe(error), true);
        }
    }
    void TextAt(HDC dc, const wchar_t* text, int x, int y, int width, int height,
                HFONT font, COLORREF color = Text, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
        auto old = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        RECT rectangle{S(x), S(y), S(x + width), S(y + height)};
        DrawTextW(dc, text, -1, &rectangle, flags);
        SelectObject(dc, old);
    }
    void Paint(HDC target = nullptr) {
        PAINTSTRUCT paint{};
        const HDC dc = target ? target : BeginPaint(window_, &paint);
        RECT area{};
        GetClientRect(window_, &area);
        FillRect(dc, &area, background_);
        const auto pen = CreatePen(PS_SOLID, S(4), Accent);
        const auto oldPen = SelectObject(dc, pen);
        const auto oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Ellipse(dc, S(23), S(25), S(45), S(47));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        TextAt(dc, L"RingLight", 58, 18, 200, 30, titleFont_);
        TextAt(dc, L"NATIVE  /  LOCAL CONTROL", 59, 48, 240, 14, smallFont_, Muted);
        auto* entry = Selected();
        const auto ringPen = CreatePen(PS_SOLID, S(6),
            entry && entry->online && entry->desired.on ? Accent : RGB(69, 84, 105));
        SelectObject(dc, ringPen);
        Ellipse(dc, S(26), S(147), S(61), S(182));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(ringPen);
        TextAt(dc, entry && entry->online ? (entry->desired.on ? L"Light is on" : L"Light is off") : L"Not connected",
            78, 144, 136, 24, font_);
        TextAt(dc, entry && entry->online ? L"CONNECTED" : L"RECONNECTING", 79, 170, 130, 16, smallFont_,
            entry && entry->online ? Accent : Muted);
        TextAt(dc, L"Brightness", 23, 208, 190, 25, font_);
        TextAt(dc, L"0%", 24, 270, 100, 16, smallFont_, Muted);
        TextAt(dc, L"100%", 270, 270, 74, 16, smallFont_, Muted, DT_RIGHT | DT_SINGLELINE);
        TextAt(dc, L"Colour temperature", 23, 292, 210, 25, font_);
        TextAt(dc, L"Warm  2900 K", 24, 354, 150, 16, smallFont_, Muted);
        TextAt(dc, L"7000 K  Cool", 194, 354, 150, 16, smallFont_, Muted, DT_RIGHT | DT_SINGLELINE);
        if (!target) EndPaint(window_, &paint);
    }
    void DrawButton(const DRAWITEMSTRUCT& item) {
        const auto dc = item.hDC;
        FillRect(dc, &item.rcItem, background_);
        const auto* entry = Selected();
        const bool power = item.CtlID == Power && entry && entry->desired.on && entry->online;
        const COLORREF color = power ? RGB(64, 112, 88) :
            (item.itemState & ODS_SELECTED ? RGB(50, 67, 87) : Panel);
        const HBRUSH brush = CreateSolidBrush(color);
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, S(12), S(12));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
        wchar_t text[128]{};
        GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
        const auto oldFont = SelectObject(dc, font_);
        SetTextColor(dc, item.itemState & ODS_DISABLED ? Muted : Text);
        SetBkMode(dc, TRANSPARENT);
        auto rectangle = item.rcItem;
        DrawTextW(dc, text, -1, &rectangle, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (item.itemState & ODS_FOCUS) {
            InflateRect(&rectangle, -S(4), -S(4));
            DrawFocusRect(dc, &rectangle);
        }
        SelectObject(dc, oldFont);
    }
    void PaintSlider(HWND slider, HDC dc) {
        RECT area{}, channel{}, thumb{};
        GetClientRect(slider, &area);
        FillRect(dc, &area, background_);
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const int center = (channel.top + channel.bottom) / 2;
        channel.top = center - S(3);
        channel.bottom = center + S(3);
        if (GetDlgCtrlID(slider) == Temperature) {
            TRIVERTEX colors[2]{
                {channel.left, channel.top, 0xf500, 0xac00, 0x6600, 0},
                {channel.right, channel.bottom, 0xa500, 0xc800, 0xf500, 0}};
            GRADIENT_RECT gradient{0, 1};
            GradientFill(dc, colors, 2, &gradient, 1, GRADIENT_FILL_RECT_H);
        } else {
            const auto brush = CreateSolidBrush(RGB(70, 116, 95));
            FillRect(dc, &channel, brush);
            DeleteObject(brush);
        }
        thumb.top = center - S(7);
        thumb.bottom = center + S(7);
        const auto brush = CreateSolidBrush(IsWindowEnabled(slider) ? Text : Muted);
        const auto oldBrush = SelectObject(dc, brush);
        const auto pen = CreatePen(PS_SOLID, S(1), Background);
        const auto oldPen = SelectObject(dc, pen);
        Ellipse(dc, thumb.left, thumb.top, thumb.right, thumb.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        if (GetFocus() == slider) {
            InflateRect(&area, -S(1), -S(1));
            DrawFocusRect(dc, &area);
        }
    }
    static LRESULT CALLBACK SliderProc(HWND slider, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR subclass, DWORD_PTR context) {
        auto* app = reinterpret_cast<App*>(context);
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(slider, &paint);
            app->PaintSlider(slider, dc);
            EndPaint(slider, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT) {
            app->PaintSlider(slider, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_NCDESTROY) RemoveWindowSubclass(slider, SliderProc, subclass);
        return DefSubclassProc(slider, message, wParam, lParam);
    }
    LRESULT Message(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == taskbarCreated_) { AddTray(); return 0; }
        switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(wParam)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, reinterpret_cast<HWND>(lParam) == Control(Status) && error_ ? Warning :
                (reinterpret_cast<HWND>(lParam) == Control(BrightnessValue) ||
                 reinterpret_cast<HWND>(lParam) == Control(TemperatureValue) ? Accent : Muted));
            SetBkColor(dc, Background);
            return reinterpret_cast<LRESULT>(background_);
        }
        case WM_DRAWITEM:
            if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON) {
                DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == Control(Brightness))
                Edit({{}, static_cast<int>(SendMessageW(Control(Brightness), TBM_GETPOS, 0, 0))});
            else if (reinterpret_cast<HWND>(lParam) == Control(Temperature))
                Edit({{}, {}, light::ToMired(2900 + 50 *
                    static_cast<int>(SendMessageW(Control(Temperature), TBM_GETPOS, 0, 0)))});
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == Choice && HIWORD(wParam) == CBN_SELCHANGE) {
                const auto index = SendMessageW(Control(Choice), CB_GETCURSEL, 0, 0);
                if (index >= 0 && static_cast<std::size_t>(index) < lights_.size()) {
                    selected_ = lights_[static_cast<std::size_t>(index)].device.Key();
                    Save();
                    Render();
                }
            } else if (HIWORD(wParam) == BN_CLICKED) Command(LOWORD(wParam));
            return 0;
        case WM_TIMER:
            if (wParam == TickTimer) Flush();
            else if (wParam == PollTimer) Poll();
            else if (wParam == DiscoverTimer) StartDiscovery();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !modal_) {
                const auto other = reinterpret_cast<HWND>(lParam);
                if (!other || GetAncestor(other, GA_ROOTOWNER) != window_) HidePopup();
            }
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            Fonts();
            Layout();
            const auto* rectangle = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, rectangle->left, rectangle->top, S(368), S(540),
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE: HidePopup(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case ShowMessage: ShowPopup(); return 0;
        case ResultMessage: Receive(std::unique_ptr<Result>(reinterpret_cast<Result*>(lParam))); return 0;
        case TrayMessage:
            switch (LOWORD(lParam)) {
            case NIN_SELECT: case NIN_KEYSELECT:
                if (IsWindowVisible(window_)) HidePopup();
                else if (GetTickCount64() - hiddenAt_ > 250) ShowPopup();
                break;
            case WM_CONTEXTMENU: Menu(); break;
            }
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wParam, lParam);
        try { return app->Message(message, wParam, lParam); }
        catch (const std::exception& error) {
            light::Log(light::Describe(error));
            MessageBoxW(window, light::Describe(error).c_str(), L"RingLight error", MB_OK | MB_ICONERROR);
        } catch (const winrt::hresult_error& error) {
            light::Log(error.message().c_str());
            MessageBoxW(window, error.message().c_str(), L"RingLight error", MB_OK | MB_ICONERROR);
        }
        PostQuitMessage(1);
        return 0;
    }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\RingLight.Native");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const auto existing = FindWindowW(WindowClass, nullptr)) PostMessageW(existing, ShowMessage, 0, 0);
        CloseHandle(mutex);
        return 0;
    }
    WSADATA sockets{};
    const int started = WSAStartup(MAKEWORD(2, 2), &sockets);
    if (started != 0) {
        MessageBoxW(nullptr, light::ErrorText(started).c_str(), L"Network initialization failed", MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }
    int code = 1;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&controls)) throw light::Error(L"Initializing Windows controls failed.");
        int count = 0;
        auto** raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw) throw light::Error(L"Could not parse command line.");
        std::vector<std::wstring> arguments;
        for (int i = 1; i < count; ++i) arguments.emplace_back(raw[i]);
        LocalFree(raw);
        {
            App app(instance);
            app.Create();
            app.Start(arguments);
            MSG message{};
            int result = 0;
            while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
                if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
                    (message.hwnd == app.Window() || IsChild(app.Window(), message.hwnd))) {
                    SendMessageW(app.Window(), WM_CLOSE, 0, 0);
                    continue;
                }
                if (!IsDialogMessageW(app.Window(), &message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            code = result == -1 ? 1 : static_cast<int>(message.wParam);
        }
        winrt::uninit_apartment();
    } catch (const std::exception& error) {
        light::Log(light::Describe(error));
        MessageBoxW(nullptr, light::Describe(error).c_str(), L"RingLight could not start", MB_OK | MB_ICONERROR);
    } catch (const winrt::hresult_error& error) {
        light::Log(error.message().c_str());
        MessageBoxW(nullptr, error.message().c_str(), L"RingLight could not start", MB_OK | MB_ICONERROR);
    }
    WSACleanup();
    CloseHandle(mutex);
    return code;
}
