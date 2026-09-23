#include "webview_browser.h"

#include <SKSE/SKSE.h>

#include <windows.h>
#include <shlwapi.h>
#include <wrl.h>

#include <gdiplus.h>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Microsoft::WRL::Callback;
    using Microsoft::WRL::ComPtr;

    constexpr wchar_t kHostClassName[] = L"SkyNetWebView2Host";
    constexpr wchar_t kGoogleHomePage[] = L"https://www.google.com/";
    constexpr wchar_t kLocalAssetRoot[] = L"http://127.0.0.1:3030/skynet/assets/";
    constexpr wchar_t kLocalHomePage[] = L"http://127.0.0.1:3030/skynet/assets/Home.html";
    constexpr wchar_t kLocalAboutPage[] = L"http://127.0.0.1:3030/skynet/assets/About.html";
    constexpr wchar_t kLocalSplashPage[] = L"http://127.0.0.1:3030/skynet/assets/Splash.html";
    constexpr int kTabStripHeight = 38;
    constexpr int kToolbarHeight = 56;
    constexpr int kChromeHeight = kTabStripHeight + kToolbarHeight;
    constexpr int kButtonWidth = 42;
    constexpr int kStarButtonWidth = 38;
    constexpr int kCloseWidth = 42;
    constexpr int kControlHeight = 38;
    constexpr int kControlTop = 9;
    constexpr int kMargin = 12;
    constexpr std::size_t kMaxStoredEntries = 200;
    constexpr UINT kOpenBrowserMessage = WM_APP + 0x521;
    constexpr UINT kCloseBrowserMessage = WM_APP + 0x522;

    enum ControlId : int
    {
        kMenuButton = 1001,
        kHomeButton,
        kBackButton,
        kForwardButton,
        kReloadButton,
        kAddressBar,
        kStarButton,
        kMinimizeButton,
        kMaximizeButton,
        kCloseButton,
        kNewTabButton = 1100,
        kTabButtonStart = 2000,
        kTabCloseButtonStart = 3000,
    };

    enum MenuCommand : UINT
    {
        kNewTabCommand = 5001,
        kAddBookmarkCommand,
        kManageBookmarksCommand,
        kSearchBookmarksCommand,
        kClearBookmarksCommand,
        kClearHistoryCommand,
        kRecentHistoryCommand,
        kRestoreSessionCommand,
        kSearchHistoryCommand,
        kManageHistoryCommand,
        kPasswordManagerCommand,
        kClearPasswordsCommand,
        kManageExtensionsCommand,
        kAboutCommand,
        kBookmarkCommandStart = 5100,
        kHistoryCommandStart = 5400,
        kExtensionCommandStart = 5700
    };

    struct Bookmark
    {
        std::wstring title;
        std::wstring url;
    };

    struct HistoryEntry
    {
        std::int64_t visitedAt{};
        std::wstring url;
    };

    struct Shortcut
    {
        std::wstring title;
        std::wstring url;
        std::wstring iconUrl;
    };

    struct BrowserTab
    {
        std::wstring title{ L"New Tab" };
        std::wstring url;
    };

    struct InstalledExtension
    {
        std::wstring name;
        std::wstring folderName;
        ComPtr<ICoreWebView2BrowserExtension> extension;
        bool enabled{ true };
        bool isUnpacked{ false };
        std::wstring version;
        std::wstring description;
        std::wstring permissions;
        std::wstring manifestVersion;
    };

    // Everything below is owned by the dedicated browser STA thread, apart
    // from the atomic values used to send it Open/Close requests from Skyrim.
    ComPtr<ICoreWebView2Environment> g_environment;
    ComPtr<ICoreWebView2Controller> g_controller;
    ComPtr<ICoreWebView2> g_webView;
    ComPtr<ICoreWebView2EnvironmentOptions> g_environmentOptions;

    HWND g_gameWindow = nullptr;
    std::atomic<HWND> g_hostWindow = nullptr;
    HWND g_backButton = nullptr;
    HWND g_forwardButton = nullptr;
    HWND g_reloadButton = nullptr;
    HWND g_addressBar = nullptr;
    HWND g_starButton = nullptr;
    HWND g_closeButton = nullptr;
    HWND g_minimizeButton = nullptr;
    HWND g_maximizeButton = nullptr;
    HWND g_newTabButton = nullptr;
    HWND g_statusLabel = nullptr;
    HWND g_menuButton = nullptr;
    HWND g_homeButton = nullptr;

    HBRUSH g_hostBrush = nullptr;
    HBRUSH g_buttonBrush = nullptr;
    HBRUSH g_buttonPressedBrush = nullptr;
    HBRUSH g_addressBrush = nullptr;
    HFONT g_font = nullptr;
    WNDPROC g_addressBarProc = nullptr;
    ULONG_PTR g_gdiplusToken = 0;
    std::unique_ptr<Gdiplus::Image> g_splashLogo;

    bool g_classRegistered = false;
    bool g_initializing = false;
    bool g_showSplash = true;
    bool g_videoSplashPlaying = false;
    bool g_adBlockFallbackInstalled = false;
    bool g_browserMinimized = false;
    bool g_isMaximized = true;
    bool g_hasNormalBounds = false;
    RECT g_normalBounds{};
    std::atomic_bool g_splashEnabled = true;
    // A bundled Splash.mp4 is the expected default. The MCM setting can
    // still explicitly turn video splash off for players who prefer a fast
    // launch.
    std::atomic_bool g_splashVideoEnabled = true;
    bool g_adBlockInstalled = false;
    bool g_passwordAutosaveEnabled = true;
    std::atomic_bool g_isOpen = false;
    std::atomic_bool g_browserThreadStarted = false;
    std::atomic_bool g_browserThreadReady = false;
    std::atomic_bool g_browserThreadFailed = false;
    std::mutex g_startMutex;
    std::condition_variable g_startCondition;
    SkyNetBrowser::CloseCallback g_closeCallback = nullptr;
    std::vector<Bookmark> g_bookmarks;
    std::vector<HistoryEntry> g_history;
    std::vector<Shortcut> g_shortcuts(5);
    std::vector<BrowserTab> g_tabs{ { L"SkyNet Home", L"" } };
    std::vector<std::pair<HWND, HWND>> g_tabControls;
    std::size_t g_activeTab = 0;
    HWND g_minimizedWindow = nullptr;
    std::vector<InstalledExtension> g_extensions;
    std::vector<std::pair<std::wstring, bool>> g_extensionPreferences;
    bool g_extensionPreferencesLoaded = false;
    std::wstring g_lastSessionUrl;
    EventRegistrationToken g_adBlockRequestedToken{};

    void CloseBrowserWindow();
    void LayoutControls();
    void NavigateFromAddressBar();
    void NavigateTo(const std::wstring& a_target);
    std::wstring NormalizeAddress(const std::wstring& a_address);
    void ShowHomePage();
    void CreateNewTab();
    void ActivateTab(std::size_t a_index);
    void CloseTab(std::size_t a_index);
    void RefreshTabStrip();
    void MinimizeBrowserWindow();
    void RestoreBrowserWindow();
    bool StartConfiguredVideoSplash();
    std::wstring FilePathToUri(std::wstring a_path);
    std::wstring GetInstalledSkyNetDirectory();
    void ShowBookmarksManager();
    void ShowHistoryManager();
    void ShowExtensionsManager();
    void ShowPasswordManagerInfo();
    void ShowAbout();
    void ClearProfileData(COREWEBVIEW2_BROWSING_DATA_KINDS a_dataKinds);
    void ScanUnpackedExtensions();
    void RefreshInstalledExtensions();
    void ToggleExtension(std::size_t a_index, bool a_showRestartMessage);
    void InstallAdBlockFallback();
    std::vector<std::wstring> SplitMessage(const std::wstring& a_message, wchar_t a_delimiter);

    void MakeCursorVisible()
    {
        while (ShowCursor(TRUE) < 0) {}
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }

    void ReturnCursorToGame()
    {
        while (ShowCursor(FALSE) >= 0) {}
    }

    void LogWebViewError(const char* a_operation, HRESULT a_result)
    {
        SKSE::log::error(
            "SkyNet browser engine {} failed (HRESULT 0x{:08X}).",
            a_operation,
            static_cast<std::uint32_t>(a_result)
        );
    }

    void SetBrowserStatus(const wchar_t* a_message)
    {
        g_showSplash = g_splashEnabled.load();
        if (g_statusLabel && a_message) {
            SetWindowTextW(g_statusLabel, a_message);
            ShowWindow(g_statusLabel, SW_SHOW);
        }
        if (const auto hostWindow = g_hostWindow.load()) {
            InvalidateRect(hostWindow, nullptr, TRUE);
        }
    }

    void LoadSplashLogo()
    {
        Gdiplus::GdiplusStartupInput startupInput{};
        if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
            SKSE::log::warn("SkyNet could not initialize the startup logo renderer.");
            return;
        }

        wchar_t executablePath[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (length == 0 || length == std::size(executablePath)) {
            SKSE::log::warn("SkyNet could not locate its installed startup logo.");
            return;
        }

        const auto logoPath = std::filesystem::path(executablePath).parent_path() /
            L"Data" / L"SKSE" / L"Plugins" / L"SkyNet" / L"SkyNetLogo.png";
        auto logo = std::make_unique<Gdiplus::Image>(logoPath.c_str());
        if (logo->GetLastStatus() != Gdiplus::Ok) {
            SKSE::log::warn("SkyNet startup logo was not found at {}.", logoPath.string());
            return;
        }

        g_splashLogo = std::move(logo);
    }

    void DrawSplashLogo(HDC a_deviceContext, const RECT& a_clientRect)
    {
        if (!g_splashEnabled.load() || !g_showSplash || !g_splashLogo) {
            return;
        }

        const auto contentWidth = static_cast<int>(a_clientRect.right - a_clientRect.left);
        const auto contentHeight = static_cast<int>(a_clientRect.bottom - a_clientRect.top) - kChromeHeight;
        if (contentWidth <= 0 || contentHeight <= 0) {
            return;
        }

        const auto logoWidth = static_cast<int>(g_splashLogo->GetWidth());
        const auto logoHeight = static_cast<int>(g_splashLogo->GetHeight());
        if (logoWidth <= 0 || logoHeight <= 0) {
            return;
        }

        const auto scale = (std::min)(
            1.0,
            (std::min)(
                static_cast<double>((std::max)(160, contentWidth - 80)) / logoWidth,
                static_cast<double>((std::max)(120, contentHeight - 140)) / logoHeight
            )
        );
        const auto drawWidth = (std::max)(1, static_cast<int>(logoWidth * scale));
        const auto drawHeight = (std::max)(1, static_cast<int>(logoHeight * scale));
        const auto drawX = (contentWidth - drawWidth) / 2;
        const auto drawY = kChromeHeight + (contentHeight - drawHeight - 46) / 2;

        Gdiplus::Graphics graphics(a_deviceContext);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(g_splashLogo.get(), drawX, drawY, drawWidth, drawHeight);
    }

    void PositionBrowserOverGame()
    {
        const auto hostWindow = g_hostWindow.load();
        if (!g_gameWindow || !hostWindow) {
            return;
        }

        RECT gameClientRect{};
        GetClientRect(g_gameWindow, &gameClientRect);

        POINT clientOrigin{ gameClientRect.left, gameClientRect.top };
        ClientToScreen(g_gameWindow, &clientOrigin);

        auto width = static_cast<int>(gameClientRect.right - gameClientRect.left);
        auto height = static_cast<int>(gameClientRect.bottom - gameClientRect.top);
        auto x = static_cast<int>(clientOrigin.x);
        auto y = static_cast<int>(clientOrigin.y);
        if (!g_isMaximized && g_hasNormalBounds) {
            x = g_normalBounds.left;
            y = g_normalBounds.top;
            width = g_normalBounds.right - g_normalBounds.left;
            height = g_normalBounds.bottom - g_normalBounds.top;

            // Keep a player-resized SkyNet window usable when the game moves
            // between monitors or changes resolution.
            const auto gameWidth = static_cast<int>(gameClientRect.right - gameClientRect.left);
            const auto gameHeight = static_cast<int>(gameClientRect.bottom - gameClientRect.top);
            width = (std::max)(720, width);
            height = (std::max)(460, height);
            width = (std::min)(width, gameWidth);
            height = (std::min)(height, gameHeight);
            const auto maxX = static_cast<int>(clientOrigin.x) + gameWidth - width;
            const auto maxY = static_cast<int>(clientOrigin.y) + gameHeight - height;
            x = (std::max)(static_cast<int>(clientOrigin.x), (std::min)(x, maxX));
            y = (std::max)(static_cast<int>(clientOrigin.y), (std::min)(y, maxY));
        } else if (!g_isMaximized) {
            const auto normalWidth = (std::max)(720, static_cast<int>(width * 4 / 5));
            const auto normalHeight = (std::max)(460, static_cast<int>(height * 4 / 5));
            x += (width - normalWidth) / 2;
            y += (height - normalHeight) / 2;
            width = normalWidth;
            height = normalHeight;
        }

        SetWindowPos(
            hostWindow,
            HWND_TOP,
            x,
            y,
            width,
            height,
            SWP_SHOWWINDOW
        );
    }

    HWND FindSkyrimWindow()
    {
        struct FindWindowData
        {
            DWORD processId;
            HWND window = nullptr;
        } data{ GetCurrentProcessId() };

        EnumWindows(
            [](HWND a_window, LPARAM a_data) -> BOOL
            {
                auto& data = *reinterpret_cast<FindWindowData*>(a_data);
                DWORD processId = 0;
                GetWindowThreadProcessId(a_window, &processId);

                if (processId == data.processId && IsWindowVisible(a_window) &&
                    GetWindow(a_window, GW_OWNER) == nullptr) {
                    data.window = a_window;
                    return FALSE;
                }

                return TRUE;
            },
            reinterpret_cast<LPARAM>(&data)
        );

        return data.window;
    }

    std::wstring GetWebViewUserDataFolder()
    {
        const auto required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (required == 0) {
            return {};
        }

        std::wstring localAppData(required, L'\0');
        const auto written = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            required
        );
        localAppData.resize(written);

        const auto skyNetDirectory = localAppData + L"\\SkyNet";
        const auto browserDirectory = skyNetDirectory + L"\\BrowserProfile";
        CreateDirectoryW(skyNetDirectory.c_str(), nullptr);
        CreateDirectoryW(browserDirectory.c_str(), nullptr);
        return browserDirectory;
    }

    std::filesystem::path GetSkyNetStorageFolder()
    {
        const auto required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (required == 0) {
            return {};
        }

        std::wstring localAppData(required, L'\0');
        const auto written = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            required
        );
        localAppData.resize(written);

        const auto directory = std::filesystem::path(localAppData) / L"SkyNet";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return error ? std::filesystem::path{} : directory;
    }

    std::filesystem::path GetBrowserExtensionDirectory()
    {
        const auto storage = GetSkyNetStorageFolder();
        if (storage.empty()) {
            return {};
        }
        const auto directory = storage / L"Extensions";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return error ? std::filesystem::path{} : directory;
    }

    void StageUnpackedExtensionsForBrowser()
    {
        const auto source = std::filesystem::path(GetInstalledSkyNetDirectory()) / L"Extensions";
        const auto destination = GetBrowserExtensionDirectory();
        if (source.empty() || destination.empty()) {
            return;
        }

        std::error_code error;
        if (!std::filesystem::exists(source, error)) {
            SKSE::log::warn("SkyNet extension source directory could not be found.");
            return;
        }
        std::filesystem::copy(source, destination,
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::update_existing,
            error);
        if (error) {
            SKSE::log::error("SkyNet could not stage unpacked extensions for the browser ({}).", error.message());
        } else {
            SKSE::log::info("SkyNet staged unpacked extensions into its private browser profile.");
        }
    }

    void SaveBrowserSettings()
    {
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wofstream file(directory / L"SkyNet.ini", std::ios::trunc);
        file << L"SplashScreen=" << (g_splashEnabled.load() ? 1 : 0) << L'\n';
        file << L"VideoSplash=" << (g_splashVideoEnabled.load() ? 1 : 0) << L'\n';
    }

    void LoadBrowserSettings()
    {
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wifstream file(directory / L"SkyNet.ini");
        std::wstring line;
        while (std::getline(file, line)) {
            if (line == L"SplashScreen=0") {
                g_splashEnabled.store(false);
            } else if (line == L"SplashScreen=1") {
                g_splashEnabled.store(true);
            } else if (line == L"VideoSplash=1") {
                g_splashVideoEnabled.store(true);
            } else if (line == L"VideoSplash=0") {
                g_splashVideoEnabled.store(false);
            }
        }
    }

    std::wstring GetInstalledSkyNetDirectory()
    {
        wchar_t executablePath[MAX_PATH]{};
        const auto length = GetModuleFileNameW(
            nullptr,
            executablePath,
            static_cast<DWORD>(std::size(executablePath))
        );
        if (length == 0 || length == std::size(executablePath)) {
            return {};
        }

        return (std::filesystem::path(executablePath).parent_path() /
            L"Data" / L"SKSE" / L"Plugins" / L"SkyNet").wstring();
    }

    std::wstring GetBundledRuntimeDirectory()
    {
        const auto directory = std::filesystem::path(GetInstalledSkyNetDirectory()) / L"SkyNetRuntime";
        std::error_code error;
        return std::filesystem::exists(directory / L"msedgewebview2.exe", error) ? directory.wstring() : std::wstring{};
    }

    std::wstring TrimLine(std::wstring a_value)
    {
        a_value.erase(std::remove(a_value.begin(), a_value.end(), L'\r'), a_value.end());
        a_value.erase(std::remove(a_value.begin(), a_value.end(), L'\n'), a_value.end());
        std::replace(a_value.begin(), a_value.end(), L'\t', L' ');
        return a_value;
    }

    std::wstring MakeBookmarkTitle(const std::wstring& a_url)
    {
        if (g_webView) {
            LPWSTR documentTitle = nullptr;
            if (SUCCEEDED(g_webView->get_DocumentTitle(&documentTitle)) && documentTitle) {
                const auto title = TrimLine(documentTitle);
                CoTaskMemFree(documentTitle);
                if (!title.empty()) {
                    return title;
                }
            }
        }
        return a_url;
    }

    void SaveBookmarks()
    {
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wofstream file(directory / L"bookmarks.txt", std::ios::trunc);
        for (const auto& bookmark : g_bookmarks) {
            file << TrimLine(bookmark.title) << L'\t' << TrimLine(bookmark.url) << L'\n';
        }
    }

    void SaveHistory()
    {
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wofstream file(directory / L"history.txt", std::ios::trunc);
        for (const auto& entry : g_history) {
            file << entry.visitedAt << L'\t' << TrimLine(entry.url) << L'\n';
        }
    }

    void LoadBookmarks()
    {
        g_bookmarks.clear();
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wifstream file(directory / L"bookmarks.txt");
        std::wstring line;
        while (std::getline(file, line) && g_bookmarks.size() < kMaxStoredEntries) {
            line = TrimLine(line);
            if (line.empty()) {
                continue;
            }
            const auto separator = line.find(L'\t');
            if (separator == std::wstring::npos) {
                // Migrate the early SkyNet URL-only bookmark format in-place.
                g_bookmarks.push_back({ line, line });
            } else {
                auto title = TrimLine(line.substr(0, separator));
                auto url = TrimLine(line.substr(separator + 1));
                if (!url.empty()) {
                    g_bookmarks.push_back({ title.empty() ? url : title, url });
                }
            }
        }
    }

    void LoadHistory()
    {
        g_history.clear();
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wifstream file(directory / L"history.txt");
        std::wstring line;
        while (std::getline(file, line) && g_history.size() < kMaxStoredEntries) {
            line = TrimLine(line);
            if (line.empty()) {
                continue;
            }
            const auto separator = line.find(L'\t');
            if (separator == std::wstring::npos) {
                // Migrate the early SkyNet URL-only history format.
                g_history.push_back({ 0, line });
                continue;
            }

            try {
                const auto timeStamp = std::stoll(line.substr(0, separator));
                const auto url = TrimLine(line.substr(separator + 1));
                if (!url.empty()) {
                    g_history.push_back({ timeStamp, url });
                }
            } catch (const std::exception&) {
                const auto url = TrimLine(line.substr(separator + 1));
                if (!url.empty()) {
                    g_history.push_back({ 0, url });
                }
            }
        }
    }

    void SaveShortcuts()
    {
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wofstream file(directory / L"shortcuts.txt", std::ios::trunc);
        for (std::size_t index = 0; index < g_shortcuts.size(); ++index) {
            const auto& shortcut = g_shortcuts[index];
            file << index << L'\t' << TrimLine(shortcut.title) << L'\t'
                 << TrimLine(shortcut.url) << L'\t' << TrimLine(shortcut.iconUrl) << L'\n';
        }
    }

    void LoadShortcuts()
    {
        g_shortcuts.assign(5, {});
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wifstream file(directory / L"shortcuts.txt");
        std::wstring line;
        while (std::getline(file, line)) {
            const auto parts = SplitMessage(TrimLine(line), L'\t');
            if (parts.size() < 4) {
                continue;
            }
            try {
                const auto index = static_cast<std::size_t>(std::stoul(parts[0]));
                if (index < g_shortcuts.size()) {
                    g_shortcuts[index] = { TrimLine(parts[1]), TrimLine(parts[2]), TrimLine(parts[3]) };
                }
            } catch (const std::exception&) {
                SKSE::log::warn("SkyNet ignored an invalid saved shortcut.");
            }
        }
    }

    void LoadBrowserData()
    {
        LoadBookmarks();
        LoadHistory();
        LoadShortcuts();
    }

    void LoadExtensionPreferences()
    {
        if (g_extensionPreferencesLoaded) {
            return;
        }
        g_extensionPreferencesLoaded = true;
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }

        std::wifstream file(directory / L"extensions.ini");
        std::wstring line;
        while (std::getline(file, line)) {
            const auto equals = line.find(L'=');
            if (equals == std::wstring::npos) {
                continue;
            }
            const auto folderName = TrimLine(line.substr(0, equals));
            const auto setting = TrimLine(line.substr(equals + 1));
            if (!folderName.empty() && (setting == L"0" || setting == L"1")) {
                g_extensionPreferences.emplace_back(folderName, setting == L"1");
            }
        }
    }

    void SaveExtensionPreferences()
    {
        const auto directory = GetSkyNetStorageFolder();
        if (directory.empty()) {
            return;
        }
        std::wofstream file(directory / L"extensions.ini", std::ios::trunc);
        for (const auto& [folderName, enabled] : g_extensionPreferences) {
            file << TrimLine(folderName) << L'=' << (enabled ? L'1' : L'0') << L'\n';
        }
    }

    bool IsExtensionEnabled(const std::wstring& a_folderName)
    {
        LoadExtensionPreferences();
        const auto found = std::find_if(g_extensionPreferences.begin(), g_extensionPreferences.end(),
            [&a_folderName](const auto& a_preference) { return a_preference.first == a_folderName; });
        return found == g_extensionPreferences.end() || found->second;
    }

    void SetExtensionPreference(const std::wstring& a_folderName, bool a_enabled)
    {
        LoadExtensionPreferences();
        const auto found = std::find_if(g_extensionPreferences.begin(), g_extensionPreferences.end(),
            [&a_folderName](const auto& a_preference) { return a_preference.first == a_folderName; });
        if (found == g_extensionPreferences.end()) {
            g_extensionPreferences.emplace_back(a_folderName, a_enabled);
        } else {
            found->second = a_enabled;
        }
        SaveExtensionPreferences();
    }

    std::wstring ReadJsonStringValue(const std::wstring& a_json, const std::wstring& a_key)
    {
        const auto nameKey = a_json.find(L"\"" + a_key + L"\"");
        if (nameKey == std::wstring::npos) {
            return {};
        }
        const auto colon = a_json.find(L':', nameKey + a_key.size() + 2);
        const auto firstQuote = a_json.find(L'\"', colon == std::wstring::npos ? nameKey + a_key.size() + 2 : colon + 1);
        if (firstQuote == std::wstring::npos) {
            return {};
        }
        const auto secondQuote = a_json.find(L'\"', firstQuote + 1);
        return secondQuote == std::wstring::npos ? std::wstring{} :
            a_json.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    }

    std::wstring ReadUnpackedExtensionName(const std::filesystem::path& a_manifestPath)
    {
        std::wifstream file(a_manifestPath);
        const std::wstring manifest((std::istreambuf_iterator<wchar_t>(file)), {});
        auto name = ReadJsonStringValue(manifest, L"name");
        constexpr std::wstring_view localePrefix = L"__MSG_";
        if (!name.starts_with(localePrefix) || !name.ends_with(L"__")) {
            return name;
        }

        const auto messageKey = name.substr(localePrefix.size(), name.size() - localePrefix.size() - 2);
        std::wifstream messagesFile(a_manifestPath.parent_path() / L"_locales" / L"en" / L"messages.json");
        const std::wstring messages((std::istreambuf_iterator<wchar_t>(messagesFile)), {});
        const auto messageIndex = messages.find(L"\"" + messageKey + L"\"");
        if (messageIndex == std::wstring::npos) {
            return name;
        }
        const auto messageObjectEnd = messages.find(L'}', messageIndex);
        return messageObjectEnd == std::wstring::npos ? name :
            ReadJsonStringValue(messages.substr(messageIndex, messageObjectEnd - messageIndex + 1), L"message");
    }

    std::wstring ReadJsonValueText(const std::wstring& a_json, const std::wstring& a_key)
    {
        const auto key = a_json.find(L"\"" + a_key + L"\"");
        if (key == std::wstring::npos) {
            return {};
        }
        const auto colon = a_json.find(L':', key + a_key.size() + 2);
        if (colon == std::wstring::npos) {
            return {};
        }
        const auto start = a_json.find_first_not_of(L" \t\r\n", colon + 1);
        if (start == std::wstring::npos) {
            return {};
        }
        const auto end = a_json.find_first_of(L",}\r\n", start);
        return TrimLine(a_json.substr(start, end == std::wstring::npos ? end : end - start));
    }

    std::wstring ReadJsonStringArray(const std::wstring& a_json, const std::wstring& a_key)
    {
        const auto key = a_json.find(L"\"" + a_key + L"\"");
        if (key == std::wstring::npos) {
            return {};
        }
        const auto open = a_json.find(L'[', key);
        const auto close = open == std::wstring::npos ? std::wstring::npos : a_json.find(L']', open);
        if (open == std::wstring::npos || close == std::wstring::npos) {
            return {};
        }
        const auto array = a_json.substr(open + 1, close - open - 1);
        std::wstring result;
        std::size_t cursor = 0;
        while (true) {
            const auto firstQuote = array.find(L'\"', cursor);
            if (firstQuote == std::wstring::npos) { break; }
            const auto secondQuote = array.find(L'\"', firstQuote + 1);
            if (secondQuote == std::wstring::npos) { break; }
            if (!result.empty()) { result += L", "; }
            result += array.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            cursor = secondQuote + 1;
        }
        return result;
    }

    void PopulateExtensionManifestDetails(InstalledExtension& a_extension,
        const std::filesystem::path& a_manifestPath)
    {
        std::wifstream file(a_manifestPath);
        const std::wstring manifest((std::istreambuf_iterator<wchar_t>(file)), {});
        a_extension.version = ReadJsonStringValue(manifest, L"version");
        a_extension.description = ReadJsonStringValue(manifest, L"description");
        a_extension.permissions = ReadJsonStringArray(manifest, L"permissions");
        a_extension.manifestVersion = ReadJsonValueText(manifest, L"manifest_version");
        if (a_extension.description.empty()) {
            a_extension.description = L"No description was supplied by this extension.";
        }
        if (a_extension.permissions.empty()) {
            a_extension.permissions = L"No declared permissions";
        }
    }

    void ScanUnpackedExtensions()
    {
        LoadExtensionPreferences();
        const auto extensionsDirectory = GetBrowserExtensionDirectory();
        std::error_code error;
        if (!std::filesystem::exists(extensionsDirectory, error)) {
            return;
        }

        std::vector<InstalledExtension> unpacked;
        for (const auto& entry : std::filesystem::directory_iterator(extensionsDirectory, error)) {
            if (error || !entry.is_directory(error)) {
                continue;
            }
            const auto manifest = entry.path() / L"manifest.json";
            if (!std::filesystem::exists(manifest, error)) {
                continue;
            }
            const auto folderName = entry.path().filename().wstring();
            auto name = ReadUnpackedExtensionName(manifest);
            if (name.empty()) {
                name = folderName;
            }

            const auto existing = std::find_if(g_extensions.begin(), g_extensions.end(),
                [&folderName](const InstalledExtension& a_extension)
                { return a_extension.folderName == folderName; });
            InstalledExtension unpackedExtension{ name, folderName, nullptr, IsExtensionEnabled(folderName), true };
            PopulateExtensionManifestDetails(unpackedExtension, manifest);
            if (existing != g_extensions.end()) {
                unpackedExtension.extension = existing->extension;
            }
            unpacked.push_back(std::move(unpackedExtension));
        }

        // Always preserve a runtime-only extension in the menu too, but ensure
        // unpacked folders are the primary source of the visible list.
        for (const auto& extension : g_extensions) {
            if (!extension.isUnpacked) {
                unpacked.push_back(extension);
            }
        }
        g_extensions = std::move(unpacked);
    }

    void RecordHistory(const std::wstring& a_url)
    {
        if (a_url.empty() || a_url == L"about:blank" || a_url.starts_with(L"file:///") ||
            a_url.starts_with(kLocalAssetRoot)) {
            return;
        }

        if (!g_history.empty() && g_history.back().url == a_url) {
            return;
        }

        g_history.push_back({ static_cast<std::int64_t>(std::time(nullptr)), a_url });
        g_lastSessionUrl = a_url;
        if (g_history.size() > kMaxStoredEntries) {
            g_history.erase(g_history.begin(),
                g_history.begin() + static_cast<std::ptrdiff_t>(g_history.size() - kMaxStoredEntries));
        }
        SaveHistory();
    }

    void AddCurrentPageToBookmarks()
    {
        if (!g_webView) {
            return;
        }

        LPWSTR source = nullptr;
        if (FAILED(g_webView->get_Source(&source)) || !source) {
            return;
        }

        const std::wstring url(source);
        CoTaskMemFree(source);
        if (url.empty() || url == L"about:blank") {
            return;
        }

        const auto found = std::find_if(g_bookmarks.begin(), g_bookmarks.end(),
            [&url](const Bookmark& a_bookmark) { return a_bookmark.url == url; });
        if (found == g_bookmarks.end()) {
            g_bookmarks.push_back({ MakeBookmarkTitle(url), url });
            SaveBookmarks();
        }
        if (g_starButton) {
            SetWindowTextW(g_starButton, L"\x2605");
        }
    }

    void UpdateBookmarkStar(const std::wstring& a_url)
    {
        if (!g_starButton) {
            return;
        }
        const auto found = std::any_of(g_bookmarks.begin(), g_bookmarks.end(),
            [&a_url](const Bookmark& a_bookmark) { return a_bookmark.url == a_url; });
        SetWindowTextW(g_starButton, found ? L"\x2605" : L"\x2606");
    }

    void SetAddressBarText(const wchar_t* a_text)
    {
        if (g_addressBar && a_text) {
            SetWindowTextW(g_addressBar, a_text);
        }
    }

    void SyncAddressBar()
    {
        if (!g_webView || !g_addressBar) {
            return;
        }

        LPWSTR source = nullptr;
        if (SUCCEEDED(g_webView->get_Source(&source)) && source) {
            const std::wstring url(source);
            SetAddressBarText(url.c_str());
            UpdateBookmarkStar(url);
            if (g_activeTab < g_tabs.size()) {
                g_tabs[g_activeTab].url = url;
            }
            CoTaskMemFree(source);
        }
    }

    std::wstring GetAddressBarText()
    {
        if (!g_addressBar) {
            return {};
        }

        const auto length = GetWindowTextLengthW(g_addressBar);
        std::wstring address(length + 1, L'\0');
        const auto copied = GetWindowTextW(g_addressBar, address.data(), length + 1);
        address.resize(copied);
        return address;
    }

    std::wstring Trim(std::wstring a_value)
    {
        const auto first = a_value.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) {
            return {};
        }

        const auto last = a_value.find_last_not_of(L" \t\r\n");
        return a_value.substr(first, last - first + 1);
    }

    std::wstring EncodeGoogleQuery(const std::wstring& a_query)
    {
        std::wstring encoded;
        encoded.reserve(a_query.size() * 3);

        constexpr wchar_t kHexDigits[] = L"0123456789ABCDEF";
        for (const auto character : a_query) {
            if ((character >= L'a' && character <= L'z') ||
                (character >= L'A' && character <= L'Z') ||
                (character >= L'0' && character <= L'9') ||
                character == L'-' || character == L'_' || character == L'.' ||
                character == L'~') {
                encoded.push_back(character);
            } else if (character == L' ') {
                encoded.push_back(L'+');
            } else if (character <= 0x7F) {
                encoded.push_back(L'%');
                encoded.push_back(kHexDigits[(character >> 4) & 0x0F]);
                encoded.push_back(kHexDigits[character & 0x0F]);
            } else {
                encoded.push_back(character);
            }
        }

        return encoded;
    }

    std::wstring NormalizeAddress(const std::wstring& a_address)
    {
        const auto address = Trim(a_address);
        if (address.empty()) {
            return {};
        }

        const auto hasScheme = address.find(L"://") != std::wstring::npos;
        const auto looksLikeSearch = address.find_first_of(L" \t\r\n") != std::wstring::npos ||
            (address.find(L'.') == std::wstring::npos && !hasScheme);

        if (looksLikeSearch) {
            return L"https://www.google.com/search?q=" + EncodeGoogleQuery(address);
        }

        return hasScheme ? address : L"https://" + address;
    }

    void NavigateFromAddressBar()
    {
        if (!g_webView) {
            SetBrowserStatus(L"SkyNet Browser is still starting. Please wait a moment.");
            return;
        }

        const auto target = NormalizeAddress(GetAddressBarText());
        if (!target.empty()) {
            NavigateTo(target);
        }
    }

    void NavigateTo(const std::wstring& a_target)
    {
        if (!g_webView || a_target.empty()) {
            return;
        }

        g_showSplash = false;
        g_videoSplashPlaying = false;
        if (g_statusLabel) {
            ShowWindow(g_statusLabel, SW_HIDE);
        }
        if (g_activeTab < g_tabs.size()) {
            g_tabs[g_activeTab].url = a_target;
        }

        const auto result = g_webView->Navigate(a_target.c_str());
        if (FAILED(result)) {
            LogWebViewError("navigation", result);
            SetBrowserStatus(L"Navigation failed. Check SkyNet.log for the error code.");
        }
    }

    void ShowHomePage()
    {
        if (!g_webView) {
            return;
        }

        g_showSplash = false;
        g_videoSplashPlaying = false;
        if (g_statusLabel) {
            ShowWindow(g_statusLabel, SW_HIDE);
        }
        if (g_activeTab < g_tabs.size()) {
            g_tabs[g_activeTab] = { L"SkyNet Home", kLocalHomePage };
        }
        const auto result = g_webView->Navigate(kLocalHomePage);
        if (FAILED(result)) {
            LogWebViewError("home page navigation", result);
            NavigateTo(kGoogleHomePage);
        }
    }

    void CreateNewTab()
    {
        if (g_tabs.size() >= 8) {
            MessageBoxW(g_hostWindow.load(), L"SkyNet supports up to eight open tabs.",
                L"SkyNet", MB_OK | MB_ICONINFORMATION);
            return;
        }
        g_tabs.push_back({ L"New Tab", L"" });
        g_activeTab = g_tabs.size() - 1;
        RefreshTabStrip();
        ShowHomePage();
    }

    void ActivateTab(std::size_t a_index)
    {
        if (a_index >= g_tabs.size() || a_index == g_activeTab) {
            return;
        }
        g_activeTab = a_index;
        RefreshTabStrip();
        if (g_tabs[a_index].url.empty()) {
            ShowHomePage();
        } else {
            NavigateTo(g_tabs[a_index].url);
        }
    }

    void CloseTab(std::size_t a_index)
    {
        if (a_index >= g_tabs.size()) {
            return;
        }
        if (a_index < g_activeTab) {
            --g_activeTab;
        }
        g_tabs.erase(g_tabs.begin() + static_cast<std::ptrdiff_t>(a_index));
        if (g_tabs.empty()) {
            g_tabs.push_back({ L"New Tab", L"" });
        }
        g_activeTab = (std::min)(g_activeTab, g_tabs.size() - 1);
        RefreshTabStrip();
        if (g_tabs[g_activeTab].url.empty()) {
            ShowHomePage();
        } else {
            NavigateTo(g_tabs[g_activeTab].url);
        }
    }

    void ShowPopupMenu(HMENU a_menu)
    {
        const auto hostWindow = g_hostWindow.load();
        if (!hostWindow || !a_menu) {
            return;
        }

        POINT point{};
        GetCursorPos(&point);
        const auto command = TrackPopupMenu(
            a_menu,
            TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
            point.x,
            point.y,
            0,
            hostWindow,
            nullptr
        );
        DestroyMenu(a_menu);

        if (command == kNewTabCommand) {
            CreateNewTab();
        } else if (command == kAddBookmarkCommand) {
            AddCurrentPageToBookmarks();
        } else if (command == kClearBookmarksCommand) {
            g_bookmarks.clear();
            SaveBookmarks();
        } else if (command == kClearHistoryCommand) {
            g_history.clear();
            SaveHistory();
            ClearProfileData(COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY);
        } else if (command == kRecentHistoryCommand || command == kManageHistoryCommand ||
            command == kSearchHistoryCommand) {
            ShowHistoryManager();
        } else if (command == kSearchBookmarksCommand || command == kManageBookmarksCommand) {
            ShowBookmarksManager();
        } else if (command == kManageExtensionsCommand) {
            ShowExtensionsManager();
        } else if (command == kPasswordManagerCommand) {
            ShowPasswordManagerInfo();
        } else if (command == kClearPasswordsCommand) {
            if (MessageBoxW(g_hostWindow.load(),
                    L"Delete all saved SkyNet passwords and autofill data for this Windows player? This cannot be undone.",
                    L"SkyNet Password Manager", MB_YESNO | MB_ICONWARNING) == IDYES) {
                ClearProfileData(
                    COREWEBVIEW2_BROWSING_DATA_KINDS_PASSWORD_AUTOSAVE |
                    COREWEBVIEW2_BROWSING_DATA_KINDS_GENERAL_AUTOFILL
                );
            }
        } else if (command == kAboutCommand) {
            ShowAbout();
        } else if (command == kRestoreSessionCommand) {
            if (!g_lastSessionUrl.empty()) {
                NavigateTo(g_lastSessionUrl);
            } else if (!g_history.empty()) {
                NavigateTo(g_history.back().url);
            }
        } else if (command >= kBookmarkCommandStart &&
            command < kBookmarkCommandStart + g_bookmarks.size()) {
            NavigateTo(g_bookmarks[command - kBookmarkCommandStart].url);
        } else if (command >= kHistoryCommandStart &&
            command < kHistoryCommandStart + g_history.size()) {
            NavigateTo(g_history[command - kHistoryCommandStart].url);
        } else if (command >= kExtensionCommandStart &&
            command < kExtensionCommandStart + g_extensions.size()) {
            const auto index = command - kExtensionCommandStart;
            ToggleExtension(index, true);
        }
    }

    std::wstring MakeMenuLabel(const std::wstring& a_url)
    {
        constexpr std::size_t kMaxLabelLength = 68;
        if (a_url.size() <= kMaxLabelLength) {
            return a_url;
        }
        return a_url.substr(0, kMaxLabelLength - 3) + L"...";
    }

    bool IsInHistoryPeriod(const HistoryEntry& a_entry, int a_days)
    {
        if (a_entry.visitedAt == 0) {
            return a_days == 3650;
        }

        const auto seconds = std::difftime(std::time(nullptr), static_cast<std::time_t>(a_entry.visitedAt));
        return seconds >= 0 && seconds < static_cast<double>(a_days) * 24.0 * 60.0 * 60.0;
    }

    void AppendHistoryItems(HMENU a_menu, int a_days, std::size_t a_limit = 20)
    {
        std::size_t added = 0;
        for (std::size_t index = g_history.size(); index > 0 && added < a_limit; --index) {
            const auto historyIndex = index - 1;
            const auto& entry = g_history[historyIndex];
            if (!IsInHistoryPeriod(entry, a_days)) {
                continue;
            }
            const auto label = MakeMenuLabel(entry.url);
            AppendMenuW(a_menu, MF_STRING,
                kHistoryCommandStart + static_cast<UINT>(historyIndex), label.c_str());
            ++added;
        }

        if (added == 0) {
            AppendMenuW(a_menu, MF_GRAYED | MF_STRING, 0, L"No entries");
        }
    }

    HMENU CreateBookmarksSubmenu()
    {
        const auto menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kAddBookmarkCommand, L"Bookmark current page");
        AppendMenuW(menu, MF_STRING, kSearchBookmarksCommand, L"Search bookmarks");
        AppendMenuW(menu, MF_STRING, kManageBookmarksCommand, L"Manage bookmarks...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        if (g_bookmarks.empty()) {
            AppendMenuW(menu, MF_GRAYED | MF_STRING, 0, L"No bookmarks saved");
        } else {
            for (std::size_t index = 0; index < g_bookmarks.size() && index < 400; ++index) {
                const auto label = MakeMenuLabel(g_bookmarks[index].title);
                AppendMenuW(menu, MF_STRING, kBookmarkCommandStart + static_cast<UINT>(index), label.c_str());
            }
        }
        return menu;
    }

    HMENU CreateHistorySubmenu()
    {
        const auto menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kClearHistoryCommand, L"Clear history");
        AppendMenuW(menu, MF_STRING, kRecentHistoryCommand, L"Recent history");
        AppendMenuW(menu, MF_STRING, kRestoreSessionCommand, L"Restore previous session");
        AppendMenuW(menu, MF_STRING, kSearchHistoryCommand, L"Search history");
        AppendMenuW(menu, MF_STRING, kManageHistoryCommand, L"Manage history...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        const auto today = CreatePopupMenu();
        AppendHistoryItems(today, 1);
        const auto yesterday = CreatePopupMenu();
        // "Yesterday" has a dedicated label and is included in the manager
        // overview. Its recent URL set follows the last 48 hours.
        AppendHistoryItems(yesterday, 2);
        const auto week = CreatePopupMenu();
        AppendHistoryItems(week, 7);
        const auto month = CreatePopupMenu();
        AppendHistoryItems(month, 30);
        const auto year = CreatePopupMenu();
        AppendHistoryItems(year, 365);
        const auto all = CreatePopupMenu();
        AppendHistoryItems(all, 3650, 60);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(today), L"Today");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(yesterday), L"Yesterday");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(week), L"Last 7 days");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(month), L"Last 30 days");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(year), L"This year");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(all), L"All history");
        return menu;
    }

    HMENU CreatePasswordsSubmenu()
    {
        const auto menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kPasswordManagerCommand, L"Password manager");
        AppendMenuW(menu, MF_STRING, kClearPasswordsCommand, L"Delete all saved passwords...");
        return menu;
    }

    HMENU CreateExtensionsSubmenu()
    {
        ScanUnpackedExtensions();
        const auto menu = CreatePopupMenu();
        if (g_extensions.empty()) {
            AppendMenuW(menu, MF_GRAYED | MF_STRING, 0, L"No unpacked extensions found");
        } else {
            for (std::size_t index = 0; index < g_extensions.size(); ++index) {
                const auto& extension = g_extensions[index];
                const auto flags = MF_STRING | (extension.enabled ? MF_CHECKED : MF_UNCHECKED);
                AppendMenuW(menu, flags, kExtensionCommandStart + static_cast<UINT>(index), extension.name.c_str());
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kManageExtensionsCommand, L"Manage unpacked extensions...");
        AppendMenuW(menu, MF_GRAYED | MF_STRING, 0, L"Click an extension name to turn it on or off");
        return menu;
    }

    void ShowPasswordManagerInfo()
    {
        const auto message = g_passwordAutosaveEnabled ?
            L"SkyNet uses the current Windows player's private browser profile for saved passwords and autofill.\n\n"
            L"Credentials remain encrypted and are never exposed to the plugin, other Windows accounts, or a SkyNet list. "
            L"Website sign-in forms can offer to save and fill the player's own credentials as in a browser.\n\n"
            L"For safety, SkyNet cannot reveal stored password text." :
            L"Password saving and autofill are disabled for the SkyNet browser profile.";
        MessageBoxW(g_hostWindow.load(), message, L"SkyNet Password Manager", MB_OK | MB_ICONINFORMATION);
    }

    void ShowAbout()
    {
        if (!g_webView) {
            return;
        }
        NavigateTo(kLocalAboutPage);
    }

    void ShowSkyNetMenu()
    {
        const auto menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kNewTabCommand, L"New tab");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(CreateHistorySubmenu()), L"History");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(CreateBookmarksSubmenu()), L"Bookmarks");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(CreatePasswordsSubmenu()), L"Passwords");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(CreateExtensionsSubmenu()), L"Extensions");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kAboutCommand, L"About SkyNet");
        ShowPopupMenu(menu);
    }

    std::wstring EscapeJavaScriptString(const std::wstring& a_value)
    {
        std::wstring escaped;
        escaped.reserve(a_value.size() + 12);
        for (const auto character : a_value) {
            switch (character) {
            case L'\\': escaped += L"\\\\"; break;
            case L'\'': escaped += L"\\'"; break;
            case L'\r': break;
            case L'\n': escaped += L"\\n"; break;
            default: escaped += character; break;
            }
        }
        return escaped;
    }

    std::wstring EscapeJsonString(const std::wstring& a_value)
    {
        std::wstring escaped;
        escaped.reserve(a_value.size() + 12);
        for (const auto character : a_value) {
            switch (character) {
            case L'\\': escaped += L"\\\\"; break;
            case L'\"': escaped += L"\\\""; break;
            case L'\r': escaped += L"\\r"; break;
            case L'\n': escaped += L"\\n"; break;
            case L'\t': escaped += L"\\t"; break;
            default: escaped += character; break;
            }
        }
        return escaped;
    }

    void PostHomeShortcuts()
    {
        if (!g_webView) {
            return;
        }
        std::wstringstream message;
        message << L"{\"type\":\"shortcuts\",\"items\":[";
        for (std::size_t index = 0; index < g_shortcuts.size(); ++index) {
            if (index != 0) { message << L","; }
            const auto& shortcut = g_shortcuts[index];
            message << L"{\"i\":" << index << L",\"t\":\"" << EscapeJsonString(shortcut.title)
                    << L"\",\"u\":\"" << EscapeJsonString(shortcut.url)
                    << L"\",\"l\":\"" << EscapeJsonString(shortcut.iconUrl) << L"\"}";
        }
        message << L"]}";
        g_webView->PostWebMessageAsJson(message.str().c_str());
    }

    std::wstring BuildManagerPage(const wchar_t* a_title, const std::wstring& a_data,
        const wchar_t* a_script)
    {
        constexpr wchar_t header[] = LR"SKY(
<!doctype html><html><head><meta charset="utf-8"><title>
)SKY";
        constexpr wchar_t middle[] = LR"SKY(
</title><style>
  :root{color-scheme:dark;font-family:"Segoe UI",sans-serif;background:#111;color:#ececec}
  body{margin:0;background:radial-gradient(circle at top,#24262d,#111 44%);min-height:100vh}
  header{display:flex;align-items:center;gap:16px;padding:22px 6vw;border-bottom:1px solid #393b44;background:#17181d}
  .brand{font-weight:700;letter-spacing:.08em;color:#e18849}.mark{font-size:28px;color:#efb562}
  h1{font-size:22px;margin:0}.content{max-width:980px;margin:28px auto;padding:0 24px}
  .tools{display:flex;gap:10px;align-items:center;margin:18px 0}.search{flex:1;min-width:180px}
  input{box-sizing:border-box;border:1px solid #555b68;border-radius:9px;padding:11px 13px;background:#202126;color:#fff;font:inherit}
  button{border:1px solid #626775;border-radius:8px;padding:9px 13px;background:#2a2c33;color:#f5f5f5;font:inherit;cursor:pointer}
  button:hover{background:#3b3e49}button.danger{border-color:#9d4949;color:#ffb7b7}.rows{display:grid;gap:8px}
  .row{display:flex;align-items:center;gap:12px;border:1px solid #393b44;border-radius:10px;padding:13px 14px;background:#1b1c21}
  .row:hover{border-color:#e18849}.entry{flex:1;min-width:0}.title{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.url{font-size:12px;color:#a5a9b4;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-top:4px}
  .tabs{display:flex;flex-wrap:wrap;gap:8px;margin:18px 0}.tabs button.active{background:#a45831;border-color:#e18849}.empty{color:#a5a9b4;padding:30px;text-align:center}
  #editor,#context{position:fixed;z-index:10;display:none;background:#202126;border:1px solid #676d7c;box-shadow:0 18px 48px #000b;border-radius:12px;padding:18px;min-width:310px}
  #editor{left:50%;top:50%;transform:translate(-50%,-50%);width:min(520px,82vw)}#editor label{display:block;font-size:13px;margin:11px 0 5px;color:#bec2cc}#editor input{width:100%}.actions{display:flex;justify-content:flex-end;gap:9px;margin-top:16px}
</style></head><body><header><span class="mark">✦</span><div class="brand">SKYNET</div><h1>
)SKY";
        constexpr wchar_t lower[] = LR"SKY(
</h1></header><main class="content"><div id="app"></div></main><div id="editor"><h2>Edit bookmark</h2><label>Title</label><input id="editTitle"><label>URL</label><input id="editUrl"><div class="actions"><button onclick="closeEditor()">Cancel</button><button onclick="saveEditor()">Save bookmark</button></div></div><div id="context"></div><script>let data=
)SKY";
        return std::wstring(header) + a_title + middle + a_title + lower + a_data +
            L";\n" + a_script + L"</script></body></html>";
    }

    void ShowBookmarksManager()
    {
        if (!g_webView) {
            return;
        }

        std::wstringstream data;
        data << L"[";
        for (std::size_t index = 0; index < g_bookmarks.size(); ++index) {
            if (index != 0) { data << L","; }
            const auto& bookmark = g_bookmarks[index];
            data << L"{i:" << index << L",t:'" << EscapeJavaScriptString(bookmark.title)
                << L"',u:'" << EscapeJavaScriptString(bookmark.url) << L"'}";
        }
        data << L"]";

        constexpr wchar_t script[] = LR"SKY(
const app=document.getElementById('app'), editor=document.getElementById('editor'), context=document.getElementById('context');
let selected=null, editing=null;
const send=m=>chrome.webview.postMessage(m);
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function render(){const q=(document.getElementById('search')?.value||'').toLowerCase();const rows=data.filter(x=>(x.t+' '+x.u).toLowerCase().includes(q));app.innerHTML=`<div class="tools"><input class="search" id="search" placeholder="Search bookmarks" value="${esc(document.getElementById('search')?.value||'')}"><button onclick="send('bookmark:add')">☆ Bookmark current page</button></div><div class="rows">${rows.length?rows.map(x=>`<div class="row" data-id="${x.i}" tabindex="0"><div class="entry"><div class="title">${esc(x.t)}</div><div class="url">${esc(x.u)}</div></div><button onclick="openBookmark(${x.i})">Open</button><button onclick="editBookmark(${x.i})">Edit</button></div>`).join(''):'<div class="empty">No matching bookmarks</div>'}</div>`;document.getElementById('search').oninput=render;document.querySelectorAll('.row').forEach(r=>{r.onclick=()=>selected=Number(r.dataset.id);r.oncontextmenu=e=>{e.preventDefault();selected=Number(r.dataset.id);context.innerHTML='<button onclick="openBookmark(selected)">Open</button> <button onclick="editBookmark(selected)">Edit bookmark</button> <button class="danger" onclick="removeBookmark(selected)">Delete bookmark</button>';context.style.display='block';context.style.left=e.clientX+'px';context.style.top=e.clientY+'px';};});}
function openBookmark(i){const x=data.find(v=>v.i===i);if(x)send('bookmark:open:'+i)}
function editBookmark(i){const x=data.find(v=>v.i===i);if(!x)return;editing=i;editTitle.value=x.t;editUrl.value=x.u;editor.style.display='block';editTitle.focus()}
function closeEditor(){editor.style.display='none'}
function saveEditor(){const x=data.find(v=>v.i===editing);if(!x)return;const t=editTitle.value.replaceAll('|',' '),u=editUrl.value.replaceAll('|',' ');if(!u.trim())return;x.t=t||u;x.u=u;send('bookmark:edit:'+editing+'|'+t+'|'+u);closeEditor();render()}
function removeBookmark(i){const p=data.findIndex(v=>v.i===i);if(p<0)return;data.splice(p,1);data.forEach((v,n)=>v.i=n);send('bookmark:delete:'+i);context.style.display='none';render()}
document.addEventListener('click',e=>{if(!context.contains(e.target))context.style.display='none'});document.addEventListener('keydown',e=>{if(e.key==='Delete'&&selected!==null){e.preventDefault();removeBookmark(selected)}});render();
)SKY";
        const auto page = BuildManagerPage(L"Bookmarks", data.str(), script);
        g_webView->NavigateToString(page.c_str());
    }

    void ShowHistoryManager()
    {
        if (!g_webView) {
            return;
        }

        std::wstringstream data;
        data << L"[";
        for (std::size_t index = 0; index < g_history.size(); ++index) {
            if (index != 0) { data << L","; }
            const auto& entry = g_history[index];
            data << L"{i:" << index << L",t:" << entry.visitedAt << L",u:'"
                << EscapeJavaScriptString(entry.url) << L"'}";
        }
        data << L"]";

        constexpr wchar_t script[] = LR"SKY(
const app=document.getElementById('app');let period='today';
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function inPeriod(x){if(period==='all'||!x.t)return true;const age=(Date.now()/1000-x.t)/86400;if(period==='today')return age<1;if(period==='yesterday')return age>=1&&age<2;if(period==='week')return age<7;if(period==='month')return age<30;if(period==='year')return age<365;return true}
function render(){const q=(document.getElementById('search')?.value||'').toLowerCase(),rows=data.filter(x=>inPeriod(x)&&x.u.toLowerCase().includes(q)).reverse();app.innerHTML=`<div class="tools"><input class="search" id="search" placeholder="Search history" value="${esc(document.getElementById('search')?.value||'')}"><button class="danger" onclick="clearHistory()">Clear history</button></div><div class="tabs">${[['today','Today'],['yesterday','Yesterday'],['week','Week'],['month','Month'],['year','Year'],['all','All history']].map(x=>`<button class="${period===x[0]?'active':''}" onclick="period='${x[0]}';render()">${x[1]}</button>`).join('')}</div><div class="rows">${rows.length?rows.map(x=>`<div class="row"><div class="entry"><div class="title">${esc(x.u)}</div><div class="url">${x.t?new Date(x.t*1000).toLocaleString():'Earlier SkyNet history'}</div></div><button onclick="chrome.webview.postMessage('history:open:'+x.i)">Open</button></div>`).join(''):'<div class="empty">No history in this period</div>'}</div>`;document.getElementById('search').oninput=render}
function clearHistory(){if(confirm('Clear all SkyNet history?')){data=[];chrome.webview.postMessage('history:clear');render()}}render();
)SKY";
        const auto page = BuildManagerPage(L"History", data.str(), script);
        g_webView->NavigateToString(page.c_str());
    }

    void ShowExtensionsManager()
    {
        if (!g_webView) {
            return;
        }
        ScanUnpackedExtensions();

        std::wstringstream data;
        data << L"[";
        for (std::size_t index = 0; index < g_extensions.size(); ++index) {
            if (index != 0) { data << L","; }
            const auto& extension = g_extensions[index];
            data << L"{i:" << index << L",n:'" << EscapeJavaScriptString(extension.name)
                << L"',e:" << (extension.enabled ? L"true" : L"false")
                << L",l:" << (extension.extension ? L"true" : L"false")
                << L",v:'" << EscapeJavaScriptString(extension.version)
                << L"',d:'" << EscapeJavaScriptString(extension.description)
                << L"',p:'" << EscapeJavaScriptString(extension.permissions)
                << L"',m:'" << EscapeJavaScriptString(extension.manifestVersion)
                << L"',f:'" << EscapeJavaScriptString(extension.folderName) << L"'}";
        }
        data << L"]";

        constexpr wchar_t script[] = LR"SKY(
const app=document.getElementById('app');let expanded=null;
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function render(){app.innerHTML=`<div class="tools"><div class="entry"><div class="title">SkyNet extension controls</div><div class="url">Every unpacked extension has its own enabled state. Use Details to review its manifest, version, declared permissions, and load state.</div></div></div><div class="rows"><div class="row" style="align-items:flex-start"><div class="entry"><div class="title">SkyNet Native Ad Shield <span style="color:#83d39b;font-size:12px">Active</span></div><div class="url">Built into SkyNet. It blocks known advertising requests and hides common ad overlays even when an external extension cannot load through Mod Organizer.</div></div><button disabled>Always on</button></div>${data.length?data.map(x=>`<div class="row" style="align-items:flex-start"><div class="entry"><div class="title">${esc(x.n)} ${x.v?`<span style="color:#e6ad77;font-size:12px">v${esc(x.v)}</span>`:''}</div><div class="url">${x.l?'Loaded in this SkyNet session':'Saved for next Skyrim restart'} · ${x.m?'Manifest V'+esc(x.m):'Runtime extension'}</div>${expanded===x.i?`<div style="margin-top:12px;padding-top:11px;border-top:1px solid #3b3d45"><div class="url"><b>Description</b><br>${esc(x.d)}</div><div class="url" style="margin-top:9px"><b>Declared permissions</b><br>${esc(x.p)}</div><div class="url" style="margin-top:9px"><b>Folder</b><br>${esc(x.f||'Browser profile')}</div></div>`:''}</div><div style="display:grid;gap:7px"><button onclick="details(${x.i})">${expanded===x.i?'Hide details':'Details'}</button><button class="${x.e?'':'danger'}" onclick="toggle(${x.i})">${x.e?'Enabled':'Disabled'}</button></div></div>`).join(''):'<div class="empty">No unpacked extension folders with manifest.json were found.</div>'}</div>`}
function details(i){expanded=expanded===i?null:i;render()}function toggle(i){const x=data.find(v=>v.i===i);if(!x)return;x.e=!x.e;chrome.webview.postMessage('extension:toggle:'+i);render()}render();
)SKY";
        const auto page = BuildManagerPage(L"Extensions", data.str(), script);
        g_webView->NavigateToString(page.c_str());
    }

    std::vector<std::wstring> SplitMessage(const std::wstring& a_message, wchar_t a_delimiter)
    {
        std::vector<std::wstring> parts;
        std::size_t start = 0;
        while (start <= a_message.size()) {
            const auto end = a_message.find(a_delimiter, start);
            parts.push_back(a_message.substr(start, end == std::wstring::npos ? end : end - start));
            if (end == std::wstring::npos) { break; }
            start = end + 1;
        }
        return parts;
    }

    void ToggleExtension(std::size_t a_index, bool a_showRestartMessage)
    {
        if (a_index >= g_extensions.size()) {
            return;
        }

        auto& extension = g_extensions[a_index];
        const auto enabled = !extension.enabled;
        extension.enabled = enabled;
        if (extension.isUnpacked) {
            SetExtensionPreference(extension.folderName, enabled);
        }

        if (extension.extension) {
            extension.extension->Enable(enabled ? TRUE : FALSE,
                Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
                    [a_index, enabled](HRESULT a_result) -> HRESULT
                    {
                        if (SUCCEEDED(a_result) && a_index < g_extensions.size()) {
                            g_extensions[a_index].enabled = enabled;
                            g_adBlockInstalled = std::any_of(g_extensions.begin(), g_extensions.end(),
                                [](const InstalledExtension& a_extension)
                                { return a_extension.folderName == L"SkyNetAdBlocker" && a_extension.enabled; });
                        } else if (FAILED(a_result) && a_index < g_extensions.size()) {
                            SKSE::log::error("SkyNet could not change the enabled state of extension {} (HRESULT 0x{:08X}).",
                                std::filesystem::path(g_extensions[a_index].name).string(),
                                static_cast<std::uint32_t>(a_result));
                        }
                        return S_OK;
                    }
                ).Get());
        } else if (a_showRestartMessage) {
            const auto message = std::wstring(L"SkyNet saved this extension's state as ") +
                (enabled ? L"enabled" : L"disabled") +
                L". Restart Skyrim to load the unpacked extension into SkyNet Browser.";
            MessageBoxW(g_hostWindow.load(), message.c_str(), L"SkyNet Extensions", MB_OK | MB_ICONINFORMATION);
        }
    }

    void HandleSkyNetWebMessage(const std::wstring& a_message)
    {
        if (a_message == L"splash:finished") {
            if (g_videoSplashPlaying) {
                ShowHomePage();
            }
            return;
        }
        if (a_message == L"bookmark:add") {
            AddCurrentPageToBookmarks();
            return;
        }
        if (a_message == L"history:clear") {
            g_history.clear();
            SaveHistory();
            ClearProfileData(COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY);
            return;
        }

        const auto colon = a_message.find(L':');
        if (colon == std::wstring::npos) { return; }
        const auto command = a_message.substr(0, colon);
        const auto rest = a_message.substr(colon + 1);
        const auto parts = SplitMessage(rest, L'|');
        try {
            if (command == L"bookmark") {
                const auto secondColon = rest.find(L':');
                if (secondColon == std::wstring::npos) { return; }
                const auto operation = rest.substr(0, secondColon);
                const auto payload = rest.substr(secondColon + 1);
                const auto editParts = SplitMessage(payload, L'|');
                const auto index = static_cast<std::size_t>(std::stoul(editParts[0]));
                if (index >= g_bookmarks.size()) { return; }
                if (operation == L"open") {
                    NavigateTo(g_bookmarks[index].url);
                } else if (operation == L"delete") {
                    g_bookmarks.erase(g_bookmarks.begin() + static_cast<std::ptrdiff_t>(index));
                    SaveBookmarks();
                } else if (operation == L"edit" && editParts.size() >= 3) {
                    const auto title = TrimLine(editParts[1]);
                    const auto url = Trim(editParts[2]);
                    if (!url.empty()) {
                        g_bookmarks[index] = { title.empty() ? url : title, url };
                        SaveBookmarks();
                    }
                }
            } else if (command == L"history" && parts.size() >= 1) {
                const auto secondColon = rest.find(L':');
                if (secondColon == std::wstring::npos) { return; }
                const auto operation = rest.substr(0, secondColon);
                const auto index = static_cast<std::size_t>(std::stoul(rest.substr(secondColon + 1)));
                if (operation == L"open" && index < g_history.size()) {
                    NavigateTo(g_history[index].url);
                }
            } else if (command == L"extension") {
                const auto secondColon = rest.find(L':');
                if (secondColon == std::wstring::npos || rest.substr(0, secondColon) != L"toggle") {
                    return;
                }
                ToggleExtension(static_cast<std::size_t>(std::stoul(rest.substr(secondColon + 1))), false);
            } else if (command == L"home") {
                if (rest == L"ready") {
                    PostHomeShortcuts();
                    return;
                }
                if (rest.starts_with(L"search:")) {
                    NavigateTo(NormalizeAddress(rest.substr(7)));
                    return;
                }
                if (rest.starts_with(L"open:")) {
                    NavigateTo(NormalizeAddress(rest.substr(5)));
                    return;
                }
                if (rest.starts_with(L"shortcut:remove:")) {
                    const auto index = static_cast<std::size_t>(std::stoul(rest.substr(16)));
                    if (index < g_shortcuts.size()) {
                        g_shortcuts[index] = {};
                        SaveShortcuts();
                    }
                    return;
                }
                if (rest.starts_with(L"shortcut:set:")) {
                    const auto shortcutParts = SplitMessage(rest.substr(13), L'|');
                    if (shortcutParts.size() < 4) {
                        return;
                    }
                    const auto index = static_cast<std::size_t>(std::stoul(shortcutParts[0]));
                    const auto url = NormalizeAddress(shortcutParts[2]);
                    if (index < g_shortcuts.size() && !url.empty()) {
                        const auto title = TrimLine(shortcutParts[1]);
                        g_shortcuts[index] = { title.empty() ? url : title, url, Trim(shortcutParts[3]) };
                        SaveShortcuts();
                    }
                }
            }
        } catch (const std::exception&) {
            SKSE::log::warn("SkyNet ignored an invalid browser manager action.");
        }
    }

    void ClearProfileData(COREWEBVIEW2_BROWSING_DATA_KINDS a_dataKinds)
    {
        ComPtr<ICoreWebView2_13> webView13;
        ComPtr<ICoreWebView2Profile> profile;
        ComPtr<ICoreWebView2Profile7> profile7;
        if (!g_webView || FAILED(g_webView.As(&webView13)) ||
            FAILED(webView13->get_Profile(profile.GetAddressOf())) ||
            FAILED(profile.As(&profile7))) {
            return;
        }
        profile7->ClearBrowsingData(a_dataKinds,
            Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                [](HRESULT a_result) -> HRESULT
                {
                    if (FAILED(a_result)) {
                        LogWebViewError("profile data removal", a_result);
                    }
                    return S_OK;
                }
            ).Get());
    }

    LRESULT CALLBACK AddressBarProc(HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam)
    {
        if (a_message == WM_KEYDOWN && a_wParam == VK_RETURN) {
            NavigateFromAddressBar();
            return 0;
        }

        return CallWindowProcW(g_addressBarProc, a_window, a_message, a_wParam, a_lParam);
    }

    void ApplyControlFont(HWND a_control)
    {
        if (a_control && g_font) {
            SendMessageW(a_control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
    }

    HWND CreateToolbarButton(HWND a_parent, const wchar_t* a_label, int a_id)
    {
        const auto button = CreateWindowExW(
            0, L"BUTTON", a_label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, a_parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(a_id)),
            GetModuleHandleW(nullptr), nullptr
        );
        ApplyControlFont(button);
        return button;
    }

    void RefreshTabStrip()
    {
        const auto hostWindow = g_hostWindow.load();
        if (!hostWindow) {
            return;
        }

        for (const auto& [tabButton, closeButton] : g_tabControls) {
            if (IsWindow(tabButton)) { DestroyWindow(tabButton); }
            if (IsWindow(closeButton)) { DestroyWindow(closeButton); }
        }
        g_tabControls.clear();

        for (std::size_t index = 0; index < g_tabs.size(); ++index) {
            auto label = TrimLine(g_tabs[index].title);
            if (label.empty()) { label = L"New Tab"; }
            if (label.size() > 18) { label = label.substr(0, 17) + L"…"; }
            if (index == g_activeTab) { label = L"• " + label; }
            const auto tabButton = CreateToolbarButton(hostWindow, label.c_str(),
                kTabButtonStart + static_cast<int>(index));
            const auto closeButton = CreateToolbarButton(hostWindow, L"×",
                kTabCloseButtonStart + static_cast<int>(index));
            g_tabControls.emplace_back(tabButton, closeButton);
        }
        LayoutControls();
    }

    void CreateControls(HWND a_parent)
    {
        // Browser-style glyphs keep the chrome compact and leave page space
        // for the actual web content.  These are standard Segoe UI symbols.
        g_menuButton = CreateToolbarButton(a_parent, L"\x2630", kMenuButton);
        g_homeButton = CreateToolbarButton(a_parent, L"\x2302", kHomeButton);
        g_backButton = CreateToolbarButton(a_parent, L"\x2039", kBackButton);
        g_forwardButton = CreateToolbarButton(a_parent, L"\x203A", kForwardButton);
        g_reloadButton = CreateToolbarButton(a_parent, L"\x21BB", kReloadButton);

        g_addressBar = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 1, 1, a_parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddressBar)),
            GetModuleHandleW(nullptr), nullptr
        );
        ApplyControlFont(g_addressBar);
        g_addressBarProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_addressBar, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(AddressBarProc)
        ));

        g_starButton = CreateToolbarButton(a_parent, L"\x2606", kStarButton);
        g_minimizeButton = CreateToolbarButton(a_parent, L"−", kMinimizeButton);
        g_maximizeButton = CreateToolbarButton(a_parent, L"□", kMaximizeButton);
        g_closeButton = CreateToolbarButton(a_parent, L"\x00D7", kCloseButton);
        g_newTabButton = CreateToolbarButton(a_parent, L"+", kNewTabButton);

        g_statusLabel = CreateWindowExW(
            0, L"STATIC", L"Starting SkyNet Browser...",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 1, 1, a_parent, nullptr, GetModuleHandleW(nullptr), nullptr
        );
        ApplyControlFont(g_statusLabel);
        if (!g_splashEnabled.load()) {
            ShowWindow(g_statusLabel, SW_HIDE);
        }
        RefreshTabStrip();
    }

    void LayoutControls()
    {
        const auto hostWindow = g_hostWindow.load();
        if (!hostWindow) {
            return;
        }

        RECT clientRect{};
        GetClientRect(hostWindow, &clientRect);
        const auto width = static_cast<int>(clientRect.right - clientRect.left);
        const auto height = static_cast<int>(clientRect.bottom - clientRect.top);

        auto tabX = kMargin;
        for (const auto& [tabButton, closeButton] : g_tabControls) {
            MoveWindow(tabButton, tabX, 3, 128, kTabStripHeight - 6, TRUE);
            tabX += 128;
            MoveWindow(closeButton, tabX, 3, 26, kTabStripHeight - 6, TRUE);
            tabX += 30;
        }
        MoveWindow(g_newTabButton, tabX, 3, 32, kTabStripHeight - 6, TRUE);

        auto x = kMargin;
        const auto toolbarTop = kTabStripHeight + kControlTop;
        MoveWindow(g_menuButton, x, toolbarTop, kButtonWidth, kControlHeight, TRUE);
        x += kButtonWidth + 6;
        MoveWindow(g_homeButton, x, toolbarTop, kButtonWidth, kControlHeight, TRUE);
        x += kButtonWidth + 6;
        MoveWindow(g_backButton, x, toolbarTop, kButtonWidth, kControlHeight, TRUE);
        x += kButtonWidth + 6;
        MoveWindow(g_forwardButton, x, toolbarTop, kButtonWidth, kControlHeight, TRUE);
        x += kButtonWidth + 6;
        MoveWindow(g_reloadButton, x, toolbarTop, kButtonWidth, kControlHeight, TRUE);
        x += kButtonWidth + 8;

        const auto addressWidth = (std::max)(120, width - x - kStarButtonWidth - (kButtonWidth * 3) - kMargin - 24);
        MoveWindow(g_addressBar, x, toolbarTop, addressWidth, kControlHeight, TRUE);
        x += addressWidth + 6;
        MoveWindow(g_starButton, x, toolbarTop, kStarButtonWidth, kControlHeight, TRUE);
        x += kStarButtonWidth + 6;
        MoveWindow(g_minimizeButton, x, toolbarTop, kButtonWidth, kControlHeight, TRUE);
        x += kButtonWidth + 4;
        MoveWindow(g_maximizeButton, x, toolbarTop, kButtonWidth, kControlHeight, TRUE);
        x += kButtonWidth + 4;
        MoveWindow(g_closeButton, x, toolbarTop, kCloseWidth, kControlHeight, TRUE);

        MoveWindow(g_statusLabel, 0, (std::max)(kChromeHeight, height - 70), width, 42, TRUE);

        if (g_controller) {
            g_controller->put_Bounds(RECT{ 0, kChromeHeight, width, (std::max)(kChromeHeight, height) });
        }
    }

    void DrawMinimizedBrowserButton(HWND a_window)
    {
        PAINTSTRUCT paint{};
        const auto deviceContext = BeginPaint(a_window, &paint);
        RECT clientRect{};
        GetClientRect(a_window, &clientRect);
        HBRUSH background = CreateSolidBrush(RGB(29, 25, 23));
        FillRect(deviceContext, &clientRect, background);
        DeleteObject(background);

        FrameRect(deviceContext, &clientRect, g_addressBrush);
        if (g_splashLogo) {
            Gdiplus::Graphics graphics(deviceContext);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            Gdiplus::ImageAttributes transparentBlack;
            // The supplied square logo has white padding and a black backdrop.
            // Crop to the emblem and treat only the near-black backdrop as
            // transparent, preserving the artwork rather than showing a black
            // square around the minimized SkyNet button.
            transparentBlack.SetColorKey(
                Gdiplus::Color(255, 0, 0, 0),
                Gdiplus::Color(255, 18, 18, 18),
                Gdiplus::ColorAdjustTypeBitmap
            );
            const auto drawWidth = (std::max)(1, static_cast<int>(clientRect.right) - 10);
            const auto drawHeight = (std::max)(1, static_cast<int>(clientRect.bottom) - 10);
            graphics.DrawImage(g_splashLogo.get(),
                Gdiplus::Rect(5, 5, drawWidth, drawHeight),
                382, 40, 636, 700, Gdiplus::UnitPixel, &transparentBlack);
        } else {
            SetBkMode(deviceContext, TRANSPARENT);
            SetTextColor(deviceContext, RGB(235, 181, 113));
            DrawTextW(deviceContext, L"S", -1, &clientRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(a_window, &paint);
    }

    void RestoreBrowserWindow()
    {
        if (g_minimizedWindow && IsWindow(g_minimizedWindow)) {
            const auto minimized = g_minimizedWindow;
            g_minimizedWindow = nullptr;
            DestroyWindow(minimized);
        }
        g_browserMinimized = false;
        const auto hostWindow = g_hostWindow.load();
        if (!hostWindow) {
            return;
        }
        PositionBrowserOverGame();
        ShowWindow(hostWindow, SW_SHOW);
        SetForegroundWindow(hostWindow);
        SetFocus(hostWindow);
        MakeCursorVisible();
        if (g_controller) {
            g_controller->put_IsVisible(TRUE);
            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    }

    void MinimizeBrowserWindow()
    {
        const auto hostWindow = g_hostWindow.load();
        if (!hostWindow || g_browserMinimized) {
            return;
        }

        g_browserMinimized = true;
        if (g_controller) {
            g_controller->put_IsVisible(FALSE);
        }
        ShowWindow(hostWindow, SW_HIDE);
        if (g_gameWindow && IsWindow(g_gameWindow)) {
            SetForegroundWindow(g_gameWindow);
            SetFocus(g_gameWindow);
        }
        // The minimized emblem was a separate native window.  It could force
        // Windows to show the cursor over Skyrim even though the browser was
        // hidden.  F2 is now the sole restore mechanism, so return completely
        // to the game input state while minimized.
        ReturnCursorToGame();
    }

    void ToggleMaximizeBrowserWindow()
    {
        g_isMaximized = !g_isMaximized;
        SetWindowTextW(g_maximizeButton, g_isMaximized ? L"□" : L"❐");
        PositionBrowserOverGame();
    }

    void CloseFromHost()
    {
        // Close the native surface on its own UI thread immediately. The
        // queued task below returns Skyrim to normal menu state afterward.
        CloseBrowserWindow();

        if (const auto taskInterface = SKSE::GetTaskInterface()) {
            taskInterface->AddTask([]()
            {
                if (g_closeCallback) {
                    g_closeCallback();
                }
            });
        } else {
            CloseBrowserWindow();
        }
    }

    void OpenBrowserWindow()
    {
        const auto hostWindow = g_hostWindow.load();
        if (!hostWindow) {
            return;
        }

        if (g_browserMinimized) {
            RestoreBrowserWindow();
            return;
        }

        const auto wasOpen = g_isOpen.exchange(true);
        PositionBrowserOverGame();
        ShowWindow(hostWindow, SW_SHOW);
        SetForegroundWindow(hostWindow);
        SetFocus(hostWindow);
        MakeCursorVisible();

        if (g_controller) {
            g_controller->put_IsVisible(TRUE);
            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);

            // Replay the configured splash on every fresh open. Minimize and
            // F2 restore remain instant and do not restart playback.
            if (!wasOpen) {
                const auto startedVideoSplash = StartConfiguredVideoSplash();
                g_showSplash = startedVideoSplash;
                if (!startedVideoSplash && g_statusLabel) {
                    ShowWindow(g_statusLabel, SW_HIDE);
                }
                InvalidateRect(hostWindow, nullptr, TRUE);
            }
        }
    }

    void CloseBrowserWindow()
    {
        g_isOpen.store(false);
        g_browserMinimized = false;
        if (g_minimizedWindow && IsWindow(g_minimizedWindow)) {
            const auto minimized = g_minimizedWindow;
            g_minimizedWindow = nullptr;
            DestroyWindow(minimized);
        }
        if (g_controller) {
            g_controller->put_IsVisible(FALSE);
        }
        const auto hostWindow = g_hostWindow.load();
        if (hostWindow && IsWindow(hostWindow)) {
            ShowWindow(hostWindow, SW_HIDE);
        }
        if (g_gameWindow && IsWindow(g_gameWindow)) {
            SetForegroundWindow(g_gameWindow);
            SetFocus(g_gameWindow);
        }
        ReturnCursorToGame();
    }

    std::wstring FilePathToUri(std::wstring a_path)
    {
        std::replace(a_path.begin(), a_path.end(), L'\\', L'/');
        return L"file:///" + a_path;
    }

    bool StartConfiguredVideoSplash()
    {
        if (!g_splashEnabled.load() || !g_splashVideoEnabled.load() || !g_webView) {
            return false;
        }

        const auto result = g_webView->Navigate(kLocalSplashPage);
        if (FAILED(result)) {
            LogWebViewError("video splash navigation", result);
            g_videoSplashPlaying = false;
            return false;
        }
        g_videoSplashPlaying = true;
        if (g_statusLabel) {
            ShowWindow(g_statusLabel, SW_HIDE);
        }
        return true;
    }

    LRESULT CALLBACK HostWindowProc(HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam)
    {
        if (a_window == g_minimizedWindow) {
            switch (a_message) {
            case WM_PAINT:
                DrawMinimizedBrowserButton(a_window);
                return 0;
            case WM_LBUTTONUP:
                RestoreBrowserWindow();
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            case WM_CLOSE:
                RestoreBrowserWindow();
                return 0;
            default:
                return DefWindowProcW(a_window, a_message, a_wParam, a_lParam);
            }
        }

        switch (a_message) {
        case kOpenBrowserMessage:
            OpenBrowserWindow();
            return 0;
        case kCloseBrowserMessage:
            CloseBrowserWindow();
            return 0;
        case WM_SIZE:
            LayoutControls();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* minMax = reinterpret_cast<MINMAXINFO*>(a_lParam);
            if (minMax) {
                minMax->ptMinTrackSize.x = 720;
                minMax->ptMinTrackSize.y = 460;
            }
            return 0;
        }
        case WM_EXITSIZEMOVE:
            if (!g_isMaximized) {
                RECT bounds{};
                if (GetWindowRect(a_window, &bounds)) {
                    g_normalBounds = bounds;
                    g_hasNormalBounds = true;
                }
            }
            LayoutControls();
            return 0;
        case WM_SETCURSOR:
            MakeCursorVisible();
            return TRUE;
        case WM_MOUSEMOVE:
            MakeCursorVisible();
            return 0;
        case WM_NCHITTEST:
        {
            const auto result = DefWindowProcW(a_window, a_message, a_wParam, a_lParam);
            if (result == HTCLIENT) {
                POINT point{
                    static_cast<int>(static_cast<short>(LOWORD(a_lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(a_lParam)))
                };
                ScreenToClient(a_window, &point);
                if (point.y >= 0 && point.y < kTabStripHeight) {
                    return HTCAPTION;
                }
            }
            return result;
        }
        case WM_KEYDOWN:
            if (a_wParam == VK_ESCAPE) {
                CloseFromHost();
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            const auto deviceContext = BeginPaint(a_window, &paint);
            RECT clientRect{};
            GetClientRect(a_window, &clientRect);
            FillRect(deviceContext, &clientRect, g_hostBrush);
            EndPaint(a_window, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        {
            const auto deviceContext = reinterpret_cast<HDC>(a_wParam);
            SetTextColor(deviceContext, RGB(235, 235, 235));
            SetBkMode(deviceContext, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_CTLCOLOREDIT:
        {
            const auto deviceContext = reinterpret_cast<HDC>(a_wParam);
            SetTextColor(deviceContext, RGB(235, 235, 235));
            SetBkColor(deviceContext, RGB(25, 25, 25));
            return reinterpret_cast<LRESULT>(g_addressBrush);
        }
        case WM_DRAWITEM:
        {
            const auto draw = reinterpret_cast<DRAWITEMSTRUCT*>(a_lParam);
            if (draw && draw->CtlType == ODT_BUTTON) {
                const auto buttonBrush = (draw->itemState & ODS_SELECTED) ?
                    g_buttonPressedBrush : g_buttonBrush;
                FillRect(draw->hDC, &draw->rcItem, buttonBrush);
                FrameRect(draw->hDC, &draw->rcItem, g_addressBrush);
                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, RGB(238, 238, 238));
                wchar_t label[32]{};
                GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
                DrawTextW(draw->hDC, label, -1, &draw->rcItem,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            if (HIWORD(a_wParam) == BN_CLICKED) {
                switch (LOWORD(a_wParam)) {
                case kMenuButton:
                    ShowSkyNetMenu();
                    return 0;
                case kHomeButton:
                    ShowHomePage();
                    return 0;
                case kBackButton:
                    if (g_webView) { g_webView->GoBack(); }
                    return 0;
                case kForwardButton:
                    if (g_webView) { g_webView->GoForward(); }
                    return 0;
                case kReloadButton:
                    if (g_webView) { g_webView->Reload(); }
                    return 0;
                case kStarButton:
                    AddCurrentPageToBookmarks();
                    SetWindowTextW(g_starButton, L"\x2605");
                    return 0;
                case kMinimizeButton:
                    MinimizeBrowserWindow();
                    return 0;
                case kMaximizeButton:
                    ToggleMaximizeBrowserWindow();
                    return 0;
                case kCloseButton:
                    CloseFromHost();
                    return 0;
                case kNewTabButton:
                    CreateNewTab();
                    return 0;
                default:
                    if (LOWORD(a_wParam) >= kTabButtonStart &&
                        LOWORD(a_wParam) < kTabButtonStart + g_tabs.size()) {
                        ActivateTab(LOWORD(a_wParam) - kTabButtonStart);
                        return 0;
                    }
                    if (LOWORD(a_wParam) >= kTabCloseButtonStart &&
                        LOWORD(a_wParam) < kTabCloseButtonStart + g_tabs.size()) {
                        CloseTab(LOWORD(a_wParam) - kTabCloseButtonStart);
                        return 0;
                    }
                    break;
                }
            }
            break;
        default:
            break;
        }

        return DefWindowProcW(a_window, a_message, a_wParam, a_lParam);
    }

    bool EnsureWindowClass()
    {
        if (g_classRegistered) {
            return true;
        }

        g_hostBrush = CreateSolidBrush(RGB(18, 18, 18));
        g_buttonBrush = CreateSolidBrush(RGB(45, 45, 48));
        g_buttonPressedBrush = CreateSolidBrush(RGB(70, 70, 74));
        g_addressBrush = CreateSolidBrush(RGB(25, 25, 25));
        g_font = CreateFontW(
            17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
        );

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = HostWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kHostClassName;

        const auto classAtom = RegisterClassExW(&windowClass);
        if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            SKSE::log::error("SkyNet could not register the WebView2 host window class (Win32 error {}).", GetLastError());
            return false;
        }

        g_classRegistered = true;
        return true;
    }

    void RefreshInstalledExtensions()
    {
        ComPtr<ICoreWebView2_13> webView13;
        ComPtr<ICoreWebView2Profile> profile;
        ComPtr<ICoreWebView2Profile7> profile7;
        if (!g_webView || FAILED(g_webView.As(&webView13)) ||
            FAILED(webView13->get_Profile(profile.GetAddressOf())) ||
            FAILED(profile.As(&profile7))) {
            return;
        }

        profile7->GetBrowserExtensions(
            Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
                [](HRESULT a_result, ICoreWebView2BrowserExtensionList* a_resultList) -> HRESULT
                {
                    if (FAILED(a_result) || !a_resultList) {
                        LogWebViewError("extension discovery", a_result);
                        return S_OK;
                    }

                    UINT32 count = 0;
                    if (FAILED(a_resultList->get_Count(&count))) {
                        return S_OK;
                    }

                    bool adBlockInstalled = false;
                    for (UINT32 index = 0; index < count; ++index) {
                        ComPtr<ICoreWebView2BrowserExtension> extension;
                        if (FAILED(a_resultList->GetValueAtIndex(index, extension.GetAddressOf())) || !extension) {
                            continue;
                        }
                        LPWSTR name = nullptr;
                        BOOL enabled = FALSE;
                        if (FAILED(extension->get_Name(&name)) || !name) {
                            continue;
                        }
                        extension->get_IsEnabled(&enabled);
                        const std::wstring extensionName(name);
                        CoTaskMemFree(name);

                        // Replace the small legacy SkyNet ad blocker with the
                        // bundled uBO Lite-based extension. This targets only
                        // our old extension name; user-installed extensions
                        // are never altered.
                        if (extensionName == L"SkyNet Ad Block") {
                            extension->Remove(
                                Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>(
                                    [](HRESULT a_removeResult) -> HRESULT
                                    {
                                        if (FAILED(a_removeResult)) {
                                            LogWebViewError("legacy ad blocker removal", a_removeResult);
                                        }
                                        return S_OK;
                                    }
                                ).Get()
                            );
                            continue;
                        }

                        const auto existing = std::find_if(g_extensions.begin(), g_extensions.end(),
                            [&extensionName](const InstalledExtension& a_extension)
                            { return a_extension.name == extensionName; });
                        if (existing != g_extensions.end()) {
                            existing->extension = extension;
                            // The persisted unpacked-folder setting is the
                            // authoritative setting. Apply it to an extension
                            // which WebView2 restored from the previous run.
                            if (existing->isUnpacked && enabled != (existing->enabled ? TRUE : FALSE)) {
                                extension->Enable(existing->enabled ? TRUE : FALSE,
                                    Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
                                        [](HRESULT) -> HRESULT { return S_OK; }
                                    ).Get());
                            } else {
                                existing->enabled = enabled == TRUE;
                            }
                            if (existing->folderName == L"SkyNetAdBlocker" && existing->enabled) {
                                adBlockInstalled = true;
                            }
                        } else {
                            g_extensions.push_back({ extensionName, L"", extension, enabled == TRUE, false });
                            if (extensionName == L"SkyNet AdBlocker (uBO Lite)" && enabled) {
                                adBlockInstalled = true;
                            }
                        }
                    }
                    g_adBlockInstalled = adBlockInstalled;
                    return S_OK;
                }
            ).Get()
        );
    }

    bool IsBlockedAdvertisingRequest(std::wstring a_url)
    {
        std::transform(a_url.begin(), a_url.end(), a_url.begin(),
            [](wchar_t a_character) { return static_cast<wchar_t>(std::towlower(a_character)); });
        static constexpr std::wstring_view blockedHosts[] = {
            L"doubleclick.net", L"googlesyndication.com", L"googleadservices.com",
            L"googletagmanager.com", L"adservice.google.", L"adnxs.com",
            L"amazon-adsystem.com", L"taboola.com", L"outbrain.com", L"criteo.com",
            L"adsrvr.org", L"rubiconproject.com", L"openx.net", L"pubmatic.com",
            L"moatads.com", L"scorecardresearch.com", L"imasdk.googleapis.com"
        };
        for (const auto host : blockedHosts) {
            if (a_url.find(host) != std::wstring::npos) {
                return true;
            }
        }

        static constexpr std::wstring_view blockedPaths[] = {
            L"/pagead/", L"/pagead2/", L"/api/stats/ads", L"/get_midroll_info",
            L"/youtubei/v1/player/ad", L"ad_break", L"adformat="
        };
        return std::any_of(std::begin(blockedPaths), std::end(blockedPaths),
            [&a_url](const auto a_path) { return a_url.find(a_path) != std::wstring::npos; });
    }

    void InstallAdBlockFallback()
    {
        if (!g_webView || !g_environment || g_adBlockFallbackInstalled) {
            return;
        }
        g_adBlockFallbackInstalled = true;

        const auto filterResult = g_webView->AddWebResourceRequestedFilter(
            L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(filterResult)) {
            LogWebViewError("ad blocker request filter", filterResult);
            return;
        }

        g_webView->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a_args) -> HRESULT
                {
                    if (!a_args || !g_environment) {
                        return S_OK;
                    }
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    LPWSTR uri = nullptr;
                    if (FAILED(a_args->get_Request(request.GetAddressOf())) || !request ||
                        FAILED(request->get_Uri(&uri)) || !uri) {
                        return S_OK;
                    }
                    const std::wstring requestUri(uri);
                    CoTaskMemFree(uri);
                    if (!IsBlockedAdvertisingRequest(requestUri)) {
                        return S_OK;
                    }

                    ComPtr<IStream> emptyBody;
                    emptyBody.Attach(SHCreateMemStream(nullptr, 0));
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    if (emptyBody && SUCCEEDED(g_environment->CreateWebResourceResponse(
                            emptyBody.Get(), 204, L"No Content", L"Cache-Control: no-store", response.GetAddressOf()))) {
                        a_args->put_Response(response.Get());
                    }
                    return S_OK;
                }
            ).Get(), &g_adBlockRequestedToken
        );

        constexpr wchar_t cosmeticFilter[] = LR"SKY(
(() => {
  const selector = [
    'iframe[src*="doubleclick"]', 'iframe[src*="googlesyndication"]',
    '[id^="google_ads_"]', '[id*="ad-container"]', '[class*="ad-container"]',
    '[data-ad-client]', '[data-ad-slot]', '.video-ads', '.ytp-ad-overlay-container',
    '.ytp-ad-player-overlay', '#player-ads', 'ytd-ad-slot-renderer',
    'ytd-promoted-sparkles-web-renderer', 'ytd-display-ad-renderer',
    '.ad-showing .ytp-ad-text', '[data-google-query-id]'
  ].join(',');
  const removeAds = () => document.querySelectorAll(selector).forEach(node => {
    node.style.setProperty('display', 'none', 'important');
    node.style.setProperty('visibility', 'hidden', 'important');
  });
  const removeKnownPromoDialogs = () => document.querySelectorAll('body *').forEach(node => {
    const text = (node.innerText || '').trim().toLowerCase();
    if (node.childElementCount > 14 || text.length > 380 ||
        !text.includes('wps office') || !text.includes('free download')) return;
    const container = node.closest('[role="dialog"], [class*="modal"], [class*="popup"], [id*="modal"], [id*="popup"]') || node;
    container.remove();
  });
  const restoreSiteChrome = () => {
    const host = location.hostname.toLowerCase();
    if (host.endsWith('youtube.com') || host.endsWith('youtube-nocookie.com')) {
      // Some filter lists hide the whole masthead while removing ad slots.
      // Keep YouTube's own search controls available without restoring ads.
      [['ytd-masthead', 'block'], ['#masthead-container', 'block'],
       ['#masthead', 'block'], ['ytd-searchbox', 'flex'],
       ['#search-form', 'flex'], ['#search-input-container', 'flex'],
       ['#search', 'block'], ['#search-input', 'block']].forEach(([selector, display]) =>
        document.querySelectorAll(selector).forEach(node =>
          { node.hidden = false; node.removeAttribute('hidden'); node.style.setProperty('display', display, 'important'); node.style.setProperty('visibility', 'visible', 'important'); node.style.setProperty('opacity', '1', 'important'); }));
    }
  };
  const ensureYoutubeSearchFallback = () => {
    const host = location.hostname.toLowerCase();
    if (!(host.endsWith('youtube.com') || host.endsWith('youtube-nocookie.com'))) return;
    const nativeInput = document.querySelector('ytd-masthead input#search, #masthead input#search, input[name="search_query"]');
    const nativeVisible = nativeInput && nativeInput.getBoundingClientRect().width > 100 &&
      getComputedStyle(nativeInput).display !== 'none' && getComputedStyle(nativeInput).visibility !== 'hidden';
    const existing = document.getElementById('skynet-youtube-search-fallback');
    if (nativeVisible) { if (existing) existing.remove(); return; }
    if (existing || !document.body) return;
    const bar = document.createElement('form');
    bar.id = 'skynet-youtube-search-fallback';
    bar.setAttribute('aria-label', 'Search YouTube');
    bar.style.cssText = 'box-sizing:border-box;position:fixed;z-index:2147483647;top:0;left:0;right:0;height:64px;display:flex;align-items:center;gap:12px;padding:10px max(20px,4vw);background:#0f0f0f;border-bottom:1px solid #303030;color:#fff;font:15px Arial,sans-serif';
    const label = document.createElement('strong'); label.textContent = 'YouTube'; label.style.cssText = 'color:#ff0033;font-size:19px;white-space:nowrap';
    const input = document.createElement('input'); input.type = 'search'; input.placeholder = 'Search YouTube'; input.setAttribute('aria-label', 'Search YouTube'); input.style.cssText = 'box-sizing:border-box;min-width:0;flex:1;height:40px;padding:0 14px;border:1px solid #4a4a4a;border-radius:20px;background:#121212;color:#fff;font:16px Arial,sans-serif';
    const submit = document.createElement('button'); submit.type = 'submit'; submit.textContent = 'Search'; submit.style.cssText = 'height:40px;padding:0 18px;border:0;border-radius:20px;background:#272727;color:#fff;font:600 14px Arial,sans-serif;cursor:pointer';
    bar.append(label, input, submit);
    bar.addEventListener('submit', event => { event.preventDefault(); const query = input.value.trim(); if (query) location.assign('/results?search_query=' + encodeURIComponent(query)); });
    document.body.prepend(bar);
  };
  const removeChromeDownloadPrompt = () => {
    if (!location.hostname.toLowerCase().includes('google.')) return;
    const prompt = /download chrome|get chrome|use chrome|don't use chrome|تنزيل chrome|استخدام chrome|المتصفح المقدم من google/i;
    document.querySelectorAll('body *').forEach(node => {
      if (node.childElementCount > 20) return;
      const text = (node.innerText || '').replace(/\s+/g, ' ').trim();
      if (text.length < 20 || text.length > 600 || !prompt.test(text)) return;
      let container = node.closest('[role="dialog"], [aria-modal="true"], [class*="modal"], [class*="popup"]');
      if (!container) {
        // Google's localized promo card does not always expose a dialog role.
        // Walk only through small ancestors so the whole page can never be
        // removed accidentally.
        container = node;
        for (let depth = 0; depth < 5 && container.parentElement; depth++) {
          const parent = container.parentElement;
          const parentText = (parent.innerText || '').trim();
          if (parent === document.body || parentText.length > 600 || parent.childElementCount > 20) break;
          container = parent;
        }
      }
      if (container && container !== document.body) container.remove();
    });
  };
  let pending = false;
  new MutationObserver(() => {
    if (!pending) { pending = true; requestAnimationFrame(() => { pending = false; removeAds(); removeKnownPromoDialogs(); restoreSiteChrome(); ensureYoutubeSearchFallback(); removeChromeDownloadPrompt(); }); }
  }).observe(document, { childList: true, subtree: true });
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => { removeAds(); removeKnownPromoDialogs(); restoreSiteChrome(); ensureYoutubeSearchFallback(); removeChromeDownloadPrompt(); }, { once: true });
  else { removeAds(); removeKnownPromoDialogs(); restoreSiteChrome(); ensureYoutubeSearchFallback(); removeChromeDownloadPrompt(); }
})();
)SKY";
        g_webView->AddScriptToExecuteOnDocumentCreated(cosmeticFilter,
            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [](HRESULT a_result, LPCWSTR) -> HRESULT
                {
                    if (FAILED(a_result)) {
                        LogWebViewError("ad blocker cosmetic filter", a_result);
                    }
                    return S_OK;
                }
            ).Get());
    }

    void ConfigureWebViewProfile()
    {
        // Present the same desktop identity that Google and YouTube expect.
        // WebView2 otherwise advertises its Edge-compatible identity, which
        // causes Google to show its "download Chrome" promotion and can make
        // YouTube omit the desktop masthead/search controls.
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(g_webView->get_Settings(settings.GetAddressOf())) && settings) {
            ComPtr<ICoreWebView2Settings2> settings2;
            if (SUCCEEDED(settings.As(&settings2))) {
                constexpr wchar_t kSkyNetChromeUserAgent[] =
                    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                    L"AppleWebKit/537.36 (KHTML, like Gecko) "
                    L"Chrome/136.0.0.0 Safari/537.36";
                const auto userAgentResult = settings2->put_UserAgent(kSkyNetChromeUserAgent);
                if (FAILED(userAgentResult)) {
                    LogWebViewError("Chrome-compatible user agent", userAgentResult);
                }
            }
        }

        ComPtr<ICoreWebView2_13> webView13;
        if (FAILED(g_webView.As(&webView13))) {
            SKSE::log::warn("SkyNet WebView2 runtime does not expose a browser profile interface.");
            return;
        }

        ComPtr<ICoreWebView2Profile> profile;
        if (FAILED(webView13->get_Profile(profile.GetAddressOf())) || !profile) {
            SKSE::log::warn("SkyNet could not access the Chromium browser profile.");
            return;
        }

        ComPtr<ICoreWebView2Profile6> profile6;
        if (SUCCEEDED(profile.As(&profile6))) {
            profile6->put_IsPasswordAutosaveEnabled(g_passwordAutosaveEnabled ? TRUE : FALSE);
            profile6->put_IsGeneralAutofillEnabled(g_passwordAutosaveEnabled ? TRUE : FALSE);
        }

        ComPtr<ICoreWebView2Profile7> profile7;
        if (FAILED(profile.As(&profile7))) {
            SKSE::log::warn("SkyNet WebView2 runtime does not support browser extensions.");
            return;
        }

        StageUnpackedExtensionsForBrowser();
        ScanUnpackedExtensions();
        RefreshInstalledExtensions();

        for (const auto& extension : g_extensions) {
            if (!extension.isUnpacked || !extension.enabled) {
                continue;
            }
            const auto extensionPath = GetBrowserExtensionDirectory() / extension.folderName;
            const auto extensionName = extension.name;
            const auto addResult = profile7->AddBrowserExtension(
                extensionPath.c_str(),
                Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
                    [extensionName](HRESULT a_result, ICoreWebView2BrowserExtension*) -> HRESULT
                    {
                        if (SUCCEEDED(a_result)) {
                            SKSE::log::info("SkyNet extension {} is enabled.",
                                std::filesystem::path(extensionName).string());
                            RefreshInstalledExtensions();
                        } else {
                            SKSE::log::error("SkyNet extension {} could not be installed (HRESULT 0x{:08X}).",
                                std::filesystem::path(extensionName).string(),
                                static_cast<std::uint32_t>(a_result));
                        }
                        return S_OK;
                    }
                ).Get()
            );
            if (FAILED(addResult)) {
                SKSE::log::error("SkyNet extension {} installation request failed (HRESULT 0x{:08X}).",
                    extensionPath.string(), static_cast<std::uint32_t>(addResult));
            }
        }
    }

    void CreateWebViewController(ICoreWebView2Environment* a_environment)
    {
        const auto result = a_environment->CreateCoreWebView2Controller(
            g_hostWindow.load(),
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT a_result, ICoreWebView2Controller* a_controller) -> HRESULT
                {
                    g_initializing = false;
                    if (FAILED(a_result) || !a_controller) {
                        LogWebViewError("controller creation", a_result);
                        SetBrowserStatus(L"SkyNet Browser could not create its page surface. Check SkyNet.log.");
                        return a_result;
                    }

                    g_controller = a_controller;
                    const auto getWebViewResult = g_controller->get_CoreWebView2(g_webView.GetAddressOf());
                    if (FAILED(getWebViewResult)) {
                        LogWebViewError("CoreWebView2 acquisition", getWebViewResult);
                        SetBrowserStatus(L"SkyNet Browser could not start. Check SkyNet.log.");
                        return getWebViewResult;
                    }

                    ConfigureWebViewProfile();
                    InstallAdBlockFallback();

                    ComPtr<ICoreWebView2Controller2> controller2;
                    if (SUCCEEDED(g_controller.As(&controller2))) {
                        controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 255, 18, 18, 18 });
                    }

                    EventRegistrationToken sourceChangedToken{};
                    g_webView->add_SourceChanged(
                        Callback<ICoreWebView2SourceChangedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT
                            {
                                SyncAddressBar();
                                return S_OK;
                            }
                        ).Get(),
                        &sourceChangedToken
                    );

                    EventRegistrationToken navigationCompletedToken{};
                    g_webView->add_NavigationCompleted(
                        Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT
                            {
                                if (!g_webView) {
                                    return S_OK;
                                }

                                LPWSTR source = nullptr;
                                if (SUCCEEDED(g_webView->get_Source(&source)) && source) {
                                    RecordHistory(source);
                                    CoTaskMemFree(source);
                                }
                                LPWSTR documentTitle = nullptr;
                                if (SUCCEEDED(g_webView->get_DocumentTitle(&documentTitle)) && documentTitle) {
                                    const auto title = TrimLine(documentTitle);
                                    CoTaskMemFree(documentTitle);
                                    if (!title.empty() && g_activeTab < g_tabs.size()) {
                                        g_tabs[g_activeTab].title = title;
                                        RefreshTabStrip();
                                    }
                                }
                                return S_OK;
                            }
                        ).Get(),
                        &navigationCompletedToken
                    );

                    EventRegistrationToken webMessageToken{};
                    g_webView->add_WebMessageReceived(
                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a_args) -> HRESULT
                            {
                                if (!a_args) {
                                    return S_OK;
                                }
                                LPWSTR message = nullptr;
                                if (SUCCEEDED(a_args->TryGetWebMessageAsString(&message)) && message) {
                                    HandleSkyNetWebMessage(message);
                                    CoTaskMemFree(message);
                                }
                                return S_OK;
                            }
                        ).Get(),
                        &webMessageToken
                    );

                    EventRegistrationToken acceleratorKeyToken{};
                    g_controller->add_AcceleratorKeyPressed(
                        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                            [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a_args) -> HRESULT
                            {
                                UINT key = 0;
                                COREWEBVIEW2_KEY_EVENT_KIND keyEvent{};
                                if (a_args && SUCCEEDED(a_args->get_VirtualKey(&key)) &&
                                    SUCCEEDED(a_args->get_KeyEventKind(&keyEvent)) &&
                                    key == VK_ESCAPE &&
                                    (keyEvent == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                        keyEvent == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)) {
                                    a_args->put_Handled(TRUE);
                                    CloseFromHost();
                                }
                                return S_OK;
                            }
                        ).Get(),
                        &acceleratorKeyToken
                    );

                    LayoutControls();
                    g_controller->put_IsVisible(g_isOpen.load() ? TRUE : FALSE);
                    const auto startedVideoSplash = StartConfiguredVideoSplash();
                    g_showSplash = startedVideoSplash;
                    if (!startedVideoSplash) {
                        ShowWindow(g_statusLabel, SW_HIDE);
                    }
                    InvalidateRect(g_hostWindow.load(), nullptr, TRUE);

                    const auto navigateResult = startedVideoSplash ? S_OK : S_OK;
                    if (!startedVideoSplash) {
                        ShowHomePage();
                    }
                    if (FAILED(navigateResult)) {
                        LogWebViewError("initial navigation", navigateResult);
                        SetBrowserStatus(L"SkyNet home page could not start. Check SkyNet.log.");
                    } else {
                        SKSE::log::info("SkyNet browser controller is ready and loading the SkyNet home page.");
                    }

                    if (g_isOpen.load()) {
                        g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                    }
                    return S_OK;
                }
            ).Get()
        );

        if (FAILED(result)) {
            g_initializing = false;
            LogWebViewError("controller request", result);
            SetBrowserStatus(L"SkyNet Browser page request failed. Check SkyNet.log.");
        }
    }

    void StartWebView()
    {
        if (g_initializing || g_controller) {
            return;
        }

        const auto userDataFolder = GetWebViewUserDataFolder();
        if (userDataFolder.empty()) {
            SKSE::log::error("SkyNet could not resolve LOCALAPPDATA for its browser profile.");
            SetBrowserStatus(L"SkyNet Browser profile folder could not be created.");
            return;
        }

        g_initializing = true;
        SKSE::log::info("SkyNet Browser is starting on its dedicated UI thread.");
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if (!options || FAILED(options.As(&g_environmentOptions))) {
            g_initializing = false;
            SKSE::log::error("SkyNet could not create browser engine options.");
            SetBrowserStatus(L"SkyNet Browser could not configure its engine.");
            return;
        }

        ComPtr<ICoreWebView2EnvironmentOptions6> extensionOptions;
        if (SUCCEEDED(g_environmentOptions.As(&extensionOptions))) {
            extensionOptions->put_AreBrowserExtensionsEnabled(TRUE);
        }

        // A release can optionally include an isolated browser runtime under
        // SkyNetRuntime. Otherwise the included SkyNet runtime installer makes
        // sure the shared player runtime is present before the game starts.
        const auto bundledRuntime = GetBundledRuntimeDirectory();
        const auto result = CreateCoreWebView2EnvironmentWithOptions(
            bundledRuntime.empty() ? nullptr : bundledRuntime.c_str(),
            userDataFolder.c_str(),
            g_environmentOptions.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [](HRESULT a_result, ICoreWebView2Environment* a_environment) -> HRESULT
                {
                    if (FAILED(a_result) || !a_environment) {
                        g_initializing = false;
                        LogWebViewError("environment creation", a_result);
                        SetBrowserStatus(L"SkyNet Browser Runtime is unavailable. Install the SkyNet Browser runtime, then restart Skyrim.");
                        return a_result;
                    }

                    g_environment = a_environment;
                    SKSE::log::info("SkyNet browser environment created.");
                    CreateWebViewController(a_environment);
                    return S_OK;
                }
            ).Get()
        );

        if (FAILED(result)) {
            g_initializing = false;
            LogWebViewError("environment request", result);
            SetBrowserStatus(L"SkyNet Browser engine request failed. Check SkyNet.log.");
        }
    }

    void SignalBrowserThreadReady(bool a_success)
    {
        {
            std::lock_guard lock(g_startMutex);
            g_browserThreadReady.store(a_success);
            g_browserThreadFailed.store(!a_success);
        }
        g_startCondition.notify_all();
    }

    void BrowserThreadMain()
    {
        const auto comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(comResult)) {
            LogWebViewError("STA apartment initialization", comResult);
            SignalBrowserThreadReady(false);
            return;
        }

        if (!EnsureWindowClass()) {
            SignalBrowserThreadReady(false);
            CoUninitialize();
            return;
        }

        g_gameWindow = FindSkyrimWindow();
        if (!g_gameWindow) {
            SKSE::log::error("SkyNet could not locate the Skyrim game window for the browser.");
            SignalBrowserThreadReady(false);
            CoUninitialize();
            return;
        }

        const auto hostWindow = CreateWindowExW(
            WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW,
            kHostClassName,
            L"SkyNet",
            // This remains above Skyrim but is owned by a normal Windows UI
            // thread. Chromium therefore receives a real STA message loop.
            WS_POPUP | WS_THICKFRAME | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, g_gameWindow, nullptr, GetModuleHandleW(nullptr), nullptr
        );
        if (!hostWindow) {
            SKSE::log::error("SkyNet could not create the Chromium host window (Win32 error {}).", GetLastError());
            SignalBrowserThreadReady(false);
            CoUninitialize();
            return;
        }

        g_hostWindow.store(hostWindow);
        LoadBrowserSettings();
        LoadBrowserData();
        LoadSplashLogo();
        CreateControls(hostWindow);
        PositionBrowserOverGame();
        LayoutControls();
        StartWebView();
        OpenBrowserWindow();
        SignalBrowserThreadReady(true);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        g_controller.Reset();
        g_webView.Reset();
        g_environment.Reset();
        g_hostWindow.store(nullptr);
        CoUninitialize();
    }

    bool StartBrowserThread()
    {
        if (!g_browserThreadStarted.exchange(true)) {
            try {
                std::thread(BrowserThreadMain).detach();
            } catch (const std::system_error&) {
                g_browserThreadFailed.store(true);
                return false;
            }
        }

        std::unique_lock lock(g_startMutex);
        const auto started = g_startCondition.wait_for(
            lock,
            std::chrono::seconds(3),
            []()
            {
                return g_browserThreadReady.load() || g_browserThreadFailed.load();
            }
        );
        return started && g_browserThreadReady.load() && !g_browserThreadFailed.load();
    }
}

namespace SkyNetBrowser
{
    void SetCloseCallback(CloseCallback a_callback)
    {
        g_closeCallback = a_callback;
    }

    bool Open()
    {
        g_isOpen.store(true);
        if (!StartBrowserThread()) {
            g_isOpen.store(false);
            SKSE::log::error("SkyNet could not start its browser UI thread.");
            return false;
        }

        if (const auto hostWindow = g_hostWindow.load()) {
            PostMessageW(hostWindow, kOpenBrowserMessage, 0, 0);
            return true;
        }

        g_isOpen.store(false);
        return false;
    }

    void Close()
    {
        g_isOpen.store(false);
        if (const auto hostWindow = g_hostWindow.load()) {
            PostMessageW(hostWindow, kCloseBrowserMessage, 0, 0);
        }
    }

    void Restore()
    {
        if (const auto hostWindow = g_hostWindow.load()) {
            PostMessageW(hostWindow, kOpenBrowserMessage, 0, 0);
        }
    }

    bool IsOpen()
    {
        return g_isOpen.load();
    }

    void SetSplashEnabled(bool a_enabled)
    {
        g_splashEnabled.store(a_enabled);
        if (!a_enabled) {
            g_splashVideoEnabled.store(false);
        }
        SaveBrowserSettings();
    }

    bool IsSplashEnabled()
    {
        return g_splashEnabled.load();
    }

    void SetSplashVideoEnabled(bool a_enabled)
    {
        g_splashVideoEnabled.store(a_enabled);
        if (a_enabled) {
            g_splashEnabled.store(true);
        }
        SaveBrowserSettings();
    }

    bool IsSplashVideoEnabled()
    {
        return g_splashVideoEnabled.load();
    }
}
