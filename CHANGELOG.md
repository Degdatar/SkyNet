# SkyNet Changelog

## 1.0.1 — 2026-09-22

### Added

- Dark SkyNet Home page with a Google search field, five editable website shortcuts, channel log, and SkyNet branding.
- Full-duration optional `Splash.mp4` startup video, followed by the SkyNet Home page.
- Browser tab strip, tab creation and closing controls, toolbar navigation, bookmark star, and minimize/maximize controls.
- Browser menus for bookmarks, history, password settings, extensions, and About SkyNet.
- Detailed unpacked-extension manager showing status, version, permissions, description, and installation path.
- Bundled SkyNet AdBlocker based on uBlock Origin Lite, plus SkyNet's built-in network and cosmetic ad shield.
- SkyNet About page and local asset delivery that work correctly with Mod Organizer 2's virtual file system.

### Changed

- SkyNet uses `SkyNetBrowser.dll` as its plugin filename.
- Home, Splash, About, logo, and splash video are now served by SkyNet at localhost so they do not fail with `ERR_FILE_NOT_FOUND` under MO2.
- Unpacked extensions are copied into SkyNet's local browser profile before loading, allowing the browser process to use them reliably.
- Press **F2** to restore SkyNet after minimizing. **F1** remains available for SKSE Menu Framework. Press **Escape** to close SkyNet.
- F2 restoration no longer loses the first key press after minimizing because SkyNet leaves the Menu Framework input dispatcher active.
- YouTube's desktop masthead/search controls are kept visible when an ad filter hides the header, and Google's Chrome download promotion is removed from the page.
- SkyNet version updated to 1.0.1.
- F2-only minimize mode no longer leaves a native cursor or a separate logo window over Skyrim.
- SkyNet's normal window can be resized from its borders; its size is retained while switching between windowed and maximized modes.
- A bundled `Splash.mp4` now plays by default and replays when SkyNet is closed and opened again (unless splash video is disabled in MCM).
- The splash page now shows the SkyNet logo as a visible fallback while the video is loading or if the video cannot be decoded.
- Added a functional SkyNet fallback search bar on YouTube pages when the site's masthead is hidden by page or extension styling.

### Notes

- The ad blocker reduces advertising and common promotional overlays, but no browser extension can promise to remove every ad from every site.
- Saved passwords remain encrypted in the current Windows user's browser profile and are not readable by SkyNet.

## 1.0.0

- Initial SkyNet browser release.
