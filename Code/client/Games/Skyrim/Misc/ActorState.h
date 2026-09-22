#pragma once

#include <Misc/IMovementState.h>

struct ActorState : IMovementState
{
    virtual ~ActorState();

    uint32_t flags1;
    uint32_t flags2;

    bool IsWeaponDrawn() const noexcept { return (flags2 >> 5 & 7) >= 3; }

    bool IsWeaponFullyDrawn() const noexcept { return (flags2 >> 5 & 7) == 3; }

    bool IsBleedingOut() const noexcept { return (flags1 & 0x1E00000) == 0x1000000 || (flags1 & 0x1E00000) == 0xE00000; }

    // Bits 14-17 of flags1: 0 is the normal standing state, anything else is sitting, sleeping or a transition
    static constexpr uint32_t kSitSleepStateMask = 0xF << 14;
    static bool IsSittingOrSleeping(uint32_t aFlags1) noexcept { return (aFlags1 & kSitSleepStateMask) != 0; }
    bool IsSittingOrSleeping() const noexcept { return IsSittingOrSleeping(flags1); }

    bool SetWeaponDrawn(bool aDraw) noexcept;
};
