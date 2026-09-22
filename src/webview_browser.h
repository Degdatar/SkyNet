#pragma once

// Native Chromium browser host used by SkyNet.  The view is hosted in the
// Skyrim window so remote pages are top-level documents, not iframe content.
namespace SkyNetBrowser
{
    using CloseCallback = void (*)();

    void SetCloseCallback(CloseCallback a_callback);
    bool Open();
    void Close();
    void Restore();
    bool IsOpen();

    // These settings are persisted in the browser's local SkyNet profile.
    void SetSplashEnabled(bool a_enabled);
    bool IsSplashEnabled();
    void SetSplashVideoEnabled(bool a_enabled);
    bool IsSplashVideoEnabled();
}
