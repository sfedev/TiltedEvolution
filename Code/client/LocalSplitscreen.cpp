#include <TiltedOnlinePCH.h>

#include <LocalSplitscreen.h>

namespace
{
std::string ReadEnvironment(const char* apName) noexcept
{
    char buffer[256]{};
    const DWORD length = GetEnvironmentVariableA(apName, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return {};

    return std::string(buffer, length);
}

struct Settings
{
    bool BackgroundInput{false};
    bool HasRect{false};
    RECT WindowRect{};
    std::string Endpoint{};

    Settings() noexcept
    {
        const auto input = ReadEnvironment("STR_SPLITSCREEN_INPUT");
        BackgroundInput = _stricmp(input.c_str(), "pad") == 0;

        const auto rect = ReadEnvironment("STR_SPLITSCREEN_RECT");
        int x = 0, y = 0, width = 0, height = 0;
        if (sscanf_s(rect.c_str(), "%d,%d,%d,%d", &x, &y, &width, &height) == 4 && width > 0 && height > 0)
        {
            WindowRect = RECT{x, y, x + width, y + height};
            HasRect = true;
        }

        Endpoint = ReadEnvironment("STR_SPLITSCREEN_CONNECT");
    }
};

const Settings& GetSettings() noexcept
{
    static const Settings s_settings;
    return s_settings;
}
} // namespace

namespace LocalSplitscreen
{
bool AllowBackgroundInput() noexcept
{
    return GetSettings().BackgroundInput;
}

bool HasWindowRect() noexcept
{
    return GetSettings().HasRect;
}

const std::string& GetAutoConnectEndpoint() noexcept
{
    return GetSettings().Endpoint;
}

void ArrangeWindow(HWND aWindow) noexcept
{
    if (!HasWindowRect() || !aWindow)
        return;

    const RECT& target = GetSettings().WindowRect;
    const LONG width = target.right - target.left;
    const LONG height = target.bottom - target.top;

    // The launcher script sets iSize W/H in SkyrimPrefs.ini to this same size, so the swap chain matches the
    // client area once the frame is gone; a different size would be stretched.
    RECT client{};
    GetClientRect(aWindow, &client);
    if (client.right != width || client.bottom != height)
        spdlog::warn("Split screen: back buffer is {}x{} but the requested window is {}x{}; check iSize W/H in SkyrimPrefs.ini", client.right, client.bottom, width, height);

    LONG_PTR style = GetWindowLongPtrW(aWindow, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW);
    style |= WS_POPUP;
    SetWindowLongPtrW(aWindow, GWL_STYLE, style);

    SetWindowPos(aWindow, nullptr, target.left, target.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(GetConsoleWindow(), SW_MINIMIZE);

    spdlog::info("Split screen: window placed at {},{} size {}x{} (background input: {})", target.left, target.top, width, height, AllowBackgroundInput());
}
} // namespace LocalSplitscreen
