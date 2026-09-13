#pragma once

#include <Forms/TESBoundObject.h>
#include <Forms/EffectSetting.h>
#include <Forms/BGSKeyword.h>
#include <Magic/EffectItem.h>
#include <Components/BGSKeywordForm.h>
#include <Components/TESFullName.h>
#include <Games/Magic/MagicSystem.h>

#include <optional>

struct MagicItem : TESBoundObject
{
    bool IsWardSpell() const noexcept;
    bool IsInvisibilitySpell() const noexcept;
    bool IsHealingSpell() const noexcept;
    bool IsBuffSpell() const noexcept;
    bool IsBoundWeaponSpell() noexcept;

    // Casting type of spells and staff enchantments, which decides how a cast is synced. Other magic items
    // (scrolls, potions, ingredients) carry no casting type here and return nothing.
    std::optional<MagicSystem::CastingType> GetCastingType() const noexcept;

    EffectItem* GetEffect(const uint32_t aEffectId) noexcept;

    TESFullName fullName;
    BGSKeywordForm keyword;
    GameArray<EffectItem*> listOfEffects;
    int32_t iHostileCount;
    EffectSetting* pAVEffectSetting;
    uint32_t uiPreloadCount;
    void* pPreloadItem;
};

static_assert(sizeof(MagicItem) == 0x90);
