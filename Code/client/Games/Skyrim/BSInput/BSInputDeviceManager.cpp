
#include <BSGraphics/BSGraphicsRenderer.h>
#include <LocalSplitscreen.h>

struct BSInputDeviceManager;

void (*BSInputDeviceManager_PollInputDevices)(BSInputDeviceManager*, float) = nullptr;

void Hook_BSInputDeviceManager_PollInputDevices(BSInputDeviceManager* inputDeviceMgr, float afDelta)
{
    // The split screen gamepad instance never has focus: XInput still reports the pad there, while the
    // DirectInput keyboard and mouse are foreground devices and stay with the focused instance.
    if (!BSGraphics::GetMainWindow()->IsForeground() && !LocalSplitscreen::AllowBackgroundInput())
        return;

    BSInputDeviceManager_PollInputDevices(inputDeviceMgr, afDelta);
}

static TiltedPhoques::Initializer s_initInputDeviceManager(
    []()
    {
        const VersionDbPtr<void> pollInputDevices(68617);

        BSInputDeviceManager_PollInputDevices = static_cast<decltype(BSInputDeviceManager_PollInputDevices)>(pollInputDevices.GetPtr());

        TP_HOOK_IMMEDIATE(&BSInputDeviceManager_PollInputDevices, &Hook_BSInputDeviceManager_PollInputDevices);
    });
