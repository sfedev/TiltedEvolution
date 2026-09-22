#pragma once

// Local split screen playtest support: two clients on one machine, one played with keyboard and mouse
// (focused window) and the other with a gamepad (unfocused window). Configured per process through
// environment variables set by Tools/Playtest/Jugar-Splitscreen.ps1; without them nothing changes.
//
//   STR_SPLITSCREEN_INPUT   = kbm | pad           pad: keep polling input devices without window focus
//   STR_SPLITSCREEN_RECT    = x,y,width,height    borderless window placement in virtual screen pixels
//                                                 (half a monitor or a whole one)
//   STR_SPLITSCREEN_CONNECT = <ip>:<port>         connect automatically once a save is loaded
namespace LocalSplitscreen
{
[[nodiscard]] bool AllowBackgroundInput() noexcept;
[[nodiscard]] bool HasWindowRect() noexcept;
[[nodiscard]] const std::string& GetAutoConnectEndpoint() noexcept;

void ArrangeWindow(HWND aWindow) noexcept;
} // namespace LocalSplitscreen
