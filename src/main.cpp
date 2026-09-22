// ============================================================================
// SkyNet
// ============================================================================
//
// SKSE plugin with an embedded cpp-httplib HTTP server.
//
// ============================================================================

// ----------------------------------------------------------------------------
// CommonLibSSE-NG / SKSE
// ----------------------------------------------------------------------------

#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>

// ----------------------------------------------------------------------------
// PrismaUI
// ----------------------------------------------------------------------------

#include "PrismaUI_API.h"
#include "SKSEMenuFramework.h"
#include "webview_browser.h"

// ----------------------------------------------------------------------------
// HTTP Server
//
// CPPHTTPLIB_OPENSSL_SUPPORT is supplied by CMake.
// ----------------------------------------------------------------------------

#include "httplib.h"

// ----------------------------------------------------------------------------
// Standard Library
// ----------------------------------------------------------------------------

#include <memory>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

// ============================================================================
// SkyNet browser integration
// ============================================================================

namespace
{
    constexpr auto kBrowserViewPath = "SkyNet/index.html";

    PRISMA_UI_API::IVPrismaUI2* g_prismaUI = nullptr;
    PrismaView g_browserView = 0;
    SKSEMenuFramework::Model::InputEvent* g_inputEvent = nullptr;
    bool g_browserIsOpen = false;

    void CloseBrowser();

    void SetSkyNetPauseState(bool a_shouldPause)
    {
        using SetWindowsPauseGameFunction =
            SKSEMenuFramework::Model::SetWindowsPauseGameFuction;
        static auto setWindowsPauseGame =
            SKSEMenuFramework::Model::Internal::GetFunction<SetWindowsPauseGameFunction>(
                "SetWindowsPauseGame"
            );

        if (setWindowsPauseGame) {
            setWindowsPauseGame(a_shouldPause);
        }
    }

    void OnBrowserDomReady(PrismaView a_view)
    {
        g_browserView = a_view;

        // Keep the dependency view hidden. SkyNet Browser owns the visible browser
        // window and its mouse input, preventing the two UI systems from
        // fighting over the cursor.
        g_prismaUI->Hide(g_browserView);
        SKSE::log::info("SkyNet PrismaUI dependency view is ready.");
    }

    void InitializeBrowser()
    {
        if (!g_prismaUI) {
            g_prismaUI = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI2>();
            if (!g_prismaUI) {
                SKSE::log::warn("PrismaUI was not found; the SkyNet browser is unavailable.");
                return;
            }
        }

        if (g_browserView && g_prismaUI->IsValid(g_browserView)) {
            return;
        }

        g_browserView = g_prismaUI->CreateView(kBrowserViewPath, OnBrowserDomReady);
        if (!g_prismaUI->IsValid(g_browserView)) {
            SKSE::log::error(
                "Could not create SkyNet browser view. Expected Data/PrismaUI/views/{}.",
                kBrowserViewPath
            );
            g_browserView = 0;
            return;
        }

        // PrismaUI exposes registered listeners as window functions. Register
        // this before DOM ready so the Close button is available immediately.
        g_prismaUI->RegisterJSListener(
            g_browserView,
            "closeBrowser",
            [](const char*) { CloseBrowser(); }
        );

        // New PrismaUI views begin visible; wait until the player opens SkyNet.
        g_prismaUI->Hide(g_browserView);
        SKSE::log::info("SkyNet browser initialized.");
    }

    void OpenBrowser()
    {
        InitializeBrowser();

        if (!g_prismaUI) {
            SKSE::log::warn("Cannot open SkyNet browser: PrismaUI is unavailable.");
            return;
        }

        // SkyNet is launched from SKSE Menu Framework. Close that menu before
        // giving PrismaUI normal gameplay focus; otherwise both frameworks try
        // to own the cursor and mouse input appears frozen until another key is
        // pressed.
        if (auto* menu = SKSEMenuFramework::GetMainWindow()) {
            menu->IsOpen = false;
        }

        // Leave Menu Framework's own hotkey dispatcher enabled.  SkyNet uses a
        // separate F2 restore handler while minimized, and disabling the
        // dispatcher here also disabled that handler on the first minimize.
        // F1 therefore continues to open Menu Framework as it normally does.

        g_browserIsOpen = true;
        SetSkyNetPauseState(true);
        if (!SkyNetBrowser::Open()) {
            SKSE::log::error("SkyNet Browser could not create its page surface.");
            g_browserIsOpen = false;
            SetSkyNetPauseState(false);
        }
    }

    void CloseBrowser()
    {
        SkyNetBrowser::Close();
        SetSkyNetPauseState(false);

        if (g_prismaUI && g_prismaUI->IsValid(g_browserView)) {
            g_prismaUI->Hide(g_browserView);
        }
        g_browserIsOpen = false;
    }

    void __stdcall RenderSkyNetSettings()
    {
        ImGuiMCP::Text("SkyNet Browser Settings");
        ImGuiMCP::Separator();
        ImGuiMCP::TextWrapped(
            "Open SkyNet Browser. Press Escape to close it. When minimized, press F2 to restore it."
        );

        if (ImGuiMCP::Button("Open SkyNet")) {
            OpenBrowser();
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text("Startup appearance");

        auto splashEnabled = SkyNetBrowser::IsSplashEnabled();
        if (ImGuiMCP::Checkbox("Show SkyNet startup splash", &splashEnabled)) {
            SkyNetBrowser::SetSplashEnabled(splashEnabled);
        }

        auto videoSplashEnabled = SkyNetBrowser::IsSplashVideoEnabled();
        if (ImGuiMCP::Checkbox("Use custom video splash", &videoSplashEnabled)) {
            SkyNetBrowser::SetSplashVideoEnabled(videoSplashEnabled);
        }

        ImGuiMCP::TextWrapped(
            "A custom video must be named Splash.mp4 and placed in "
            "Data/SKSE/Plugins/SkyNet/. SkyNet stays on the splash until the full video ends."
        );
        ImGuiMCP::TextWrapped(
            "Bookmarks, browsing history, password autofill, and the built-in "
            "SkyNet AdBlocker is managed from SkyNet's browser menu bar."
        );

        if (!g_prismaUI) {
            ImGuiMCP::Text("PrismaUI and the SkyNet Browser Runtime are required.");
        }
    }

    void RegisterSettingsMenu()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            SKSE::log::info("SKSE Menu Framework not found; SkyNet settings page was not registered.");
            return;
        }

        SKSEMenuFramework::SetSection("SkyNet");
        SKSEMenuFramework::AddSectionItem("SkyNet", RenderSkyNetSettings);

        if (!g_inputEvent) {
            g_inputEvent = SKSEMenuFramework::AddInputEvent(
                [](RE::InputEvent* a_event) -> bool
                {
                    if (!a_event || !g_browserIsOpen ||
                        a_event->device != RE::INPUT_DEVICE::kKeyboard) {
                        return false;
                    }

                    auto* button = a_event->AsButtonEvent();
                    if (button && button->IsDown()) {
                        if (button->GetIDCode() == RE::BSWin32KeyboardDevice::Key::kEscape) {
                            CloseBrowser();
                            return true;
                        }
                        if (button->GetIDCode() == RE::BSWin32KeyboardDevice::Key::kF2) {
                            SkyNetBrowser::Restore();
                            return true;
                        }
                    }

                    return false;
                }
            );
        }
        SKSE::log::info("SkyNet settings page registered with SKSE Menu Framework.");
    }
}

// ============================================================================
// Global HTTP Server
// ============================================================================

static std::unique_ptr<httplib::Server> g_httpServer = nullptr;
static std::thread g_serverThread;

namespace
{
    std::filesystem::path GetSkyNetAssetDirectory()
    {
        wchar_t executablePath[MAX_PATH]{};
        const auto length = GetModuleFileNameW(
            nullptr, executablePath, static_cast<DWORD>(std::size(executablePath))
        );
        if (length == 0 || length == std::size(executablePath)) {
            return {};
        }
        // This file access is made inside the injected Skyrim process, where
        // Mod Organizer's virtual Data filesystem is available. Chromium's
        // child process cannot access that virtual path directly, so it gets
        // assets through this local server instead.
        return std::filesystem::path(executablePath).parent_path() /
            L"Data" / L"SKSE" / L"Plugins" / L"SkyNet";
    }

    std::string ReadSkyNetAsset(const char* a_fileName)
    {
        const auto path = GetSkyNetAssetDirectory() / a_fileName;
        std::ifstream file(path, std::ios::binary);
        return file ? std::string(std::istreambuf_iterator<char>(file), {}) : std::string{};
    }

    void ServeSkyNetAsset(const httplib::Request& a_request, httplib::Response& a_response,
        const char* a_fileName, const char* a_contentType)
    {
        const auto content = ReadSkyNetAsset(a_fileName);
        if (content.empty()) {
            a_response.status = 404;
            a_response.set_content("SkyNet asset was not found.", "text/plain; charset=utf-8");
            return;
        }

        a_response.set_header("Cache-Control", "no-store");
        a_response.set_header("Accept-Ranges", "bytes");
        const auto range = a_request.get_header_value("Range");
        if (range.rfind("bytes=", 0) == 0) {
            const auto dash = range.find('-', 6);
            try {
                const auto start = static_cast<std::size_t>(std::stoull(range.substr(6, dash - 6)));
                const auto requestedEnd = dash == std::string::npos || dash + 1 >= range.size() ?
                    content.size() - 1 : static_cast<std::size_t>(std::stoull(range.substr(dash + 1)));
                if (start < content.size()) {
                    const auto end = (std::min)(requestedEnd, content.size() - 1);
                    a_response.status = 206;
                    a_response.set_header("Content-Range", "bytes " + std::to_string(start) + "-" +
                        std::to_string(end) + "/" + std::to_string(content.size()));
                    a_response.set_content(content.substr(start, end - start + 1), a_contentType);
                    return;
                }
            } catch (const std::exception&) {
                // Fall through to the complete response for malformed range headers.
            }
        }

        a_response.set_content(content, a_contentType);
    }
}

// ============================================================================
// Start HTTP Proxy
// ============================================================================

void StartHttpProxy()
{
    g_httpServer = std::make_unique<httplib::Server>();

    // ------------------------------------------------------------------------
    // Status Endpoint
    // ------------------------------------------------------------------------

    g_httpServer->Get(
        "/skynet/status",
        [](const httplib::Request&, httplib::Response& res)
        {
            res.set_content(
                R"({"status":"ok","plugin":"SkyNet"})",
                "application/json"
            );
        }
    );

    // Serve locally hosted browser assets. This keeps Home, About, and the
    // splash video compatible with Mod Organizer, whose virtual files are not
    // visible to Chromium's sandboxed child process.
    g_httpServer->Get("/skynet/assets/Home.html",
        [](const httplib::Request& req, httplib::Response& res)
        { ServeSkyNetAsset(req, res, "Home.html", "text/html; charset=utf-8"); });
    g_httpServer->Get("/skynet/assets/About.html",
        [](const httplib::Request& req, httplib::Response& res)
        { ServeSkyNetAsset(req, res, "About.html", "text/html; charset=utf-8"); });
    g_httpServer->Get("/skynet/assets/Splash.html",
        [](const httplib::Request& req, httplib::Response& res)
        { ServeSkyNetAsset(req, res, "Splash.html", "text/html; charset=utf-8"); });
    g_httpServer->Get("/skynet/assets/SkyNetLogo.png",
        [](const httplib::Request& req, httplib::Response& res)
        { ServeSkyNetAsset(req, res, "SkyNetLogo.png", "image/png"); });
    g_httpServer->Get("/skynet/assets/Splash.mp4",
        [](const httplib::Request& req, httplib::Response& res)
        { ServeSkyNetAsset(req, res, "Splash.mp4", "video/mp4"); });

    // ------------------------------------------------------------------------
    // Start Server
    // ------------------------------------------------------------------------

    g_serverThread = std::thread(
        []()
        {
            if (g_httpServer)
            {
                g_httpServer->listen("127.0.0.1", 3030);
            }
        }
    );

    // The server runs independently from the Skyrim thread.
    g_serverThread.detach();
}

// ============================================================================
// Stop HTTP Proxy
// ============================================================================

void StopHttpProxy()
{
    if (g_httpServer)
    {
        g_httpServer->stop();
        g_httpServer.reset();
    }
}

// ============================================================================
// SKSE Messaging
// ============================================================================

void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
{
    if (!a_msg)
    {
        return;
    }

    if (a_msg->type == SKSE::MessagingInterface::kDataLoaded)
    {
        SKSE::log::info(
            "Skyrim data loaded. Initializing SkyNet HTTP Proxy..."
        );

        StartHttpProxy();
    }
    else if (a_msg->type == SKSE::MessagingInterface::kPostLoad)
    {
        SkyNetBrowser::SetCloseCallback(CloseBrowser);
        InitializeBrowser();
        RegisterSettingsMenu();
    }
}

// ============================================================================
// SKSE Plugin Entry Point
// ============================================================================

// Export the version-independent metadata SKSE uses to recognize and load a
// modern Address Library plugin on current SE/AE runtimes.
SKSEPluginInfo(
    .Name = "SkyNet"
)

SKSEPluginLoad(
    const SKSE::LoadInterface* a_skse
)
{
    SKSE::Init(a_skse);

    SKSE::log::info("SkyNet plugin loaded successfully.");

    auto* messaging = SKSE::GetMessagingInterface();

    if (messaging)
    {
        messaging->RegisterListener(OnSKSEMessage);
    }

    return true;
}
