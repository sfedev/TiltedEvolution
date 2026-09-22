#include <Games/References.h>
#include <atomic>
#include <Games/Skyrim/EquipManager.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Misc/GameVM.h>
#include <DefaultObjectManager.h>
#include <Forms/TESNPC.h>
#include <Forms/TESFaction.h>
#include <Forms/TESQuest.h>
#include <Components/TESActorBaseData.h>
#include <ExtraData/ExtraFactionChanges.h>
#include <ExtraData/ExtraLeveledCreature.h>
#include <Games/Memory.h>
#include <Combat/CombatController.h>

#include <Events/HealthChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/DroppedItemEvent.h>
#include <DropSync.h>
#include <Events/MountEvent.h>
#include <Events/DialogueEvent.h>
#include <Games/Misc/MenuTopicManager.h>
#include <Events/HitEvent.h>
#include <Events/RemoveSpellEvent.h>

#include <Games/TES.h>
#include <World.h>
#include <Services/PapyrusService.h>

#include <Forms/ActorValueInfo.h>
#include <Forms/TESRace.h>

#include <Effects/ValueModifierEffect.h>

#include <Games/Skyrim/Misc/InventoryEntry.h>
#include <Games/Skyrim/ExtraData/ExtraCount.h>
#include <Games/Misc/ActorKnowledge.h>

#include <ExtraData/ExtraDataList.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <Forms/TESObjectCELL.h>

#include <Structs/Skyrim/AnimationGraphDescriptor_BHR_Master.h>

#include <Games/Overrides.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Havok/hkbStateMachine.h>
#include <Havok/hkbBehaviorGraph.h>
#include <Havok/bhkCharacterController.h>

#include <ModCompat/BehaviorVar.h>

#if TP_DROPSYNC_DIAG
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Camera/PlayerCamera.h>
#include <Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <NetImmerse/NiNode.h>
#endif

namespace
{
// Returns whether the change was queued for the server
bool QueueActorInventoryChange(Actor* apActor, InventoryChangeEvent aEvent, TESObjectREFR* apTransferReference = nullptr)
{
    auto ownershipToken = Utils::GetLocalOwnershipToken(apActor->formID);
    if (!ownershipToken && apTransferReference == PlayerCharacter::Get())
        ownershipToken = Utils::GetRemoteOwnershipToken(apActor->formID);

    if (!ownershipToken)
    {
        // A drop or a ground pickup that never reaches the server leaves the other clients with a stale copy
        if (World::Get().GetTransport().IsOnline() && (aEvent.Drop || aEvent.DropSeq != 0 || aEvent.GroundRefHandle != 0))
            spdlog::warn(
                "[DropSync] event not queued: actor={:X} has no ownership token (base={:X} count={} drop={} seq={} groundHandle={:X})", apActor->formID, aEvent.Item.BaseId.BaseId,
                aEvent.Item.Count, aEvent.Drop, aEvent.DropSeq, aEvent.GroundRefHandle);
        return false;
    }

    aEvent.ServerId = ownershipToken->ServerId;
    aEvent.OwnershipEpoch = ownershipToken->OwnershipEpoch;
    World::Get().GetRunner().Trigger(std::move(aEvent));
    return true;
}

// The game returns the handle of the reference it created through a hidden return argument
uint32_t HandleFromDropResult(void* apReturned, void* apResult) noexcept
{
    const void* pHandle = apReturned ? apReturned : apResult;
    return pHandle ? static_cast<const BSPointerHandle<TESObjectREFR>*>(pHandle)->handle.iBits : 0;
}

// Sequence number that ties the inventory change of a drop to the reference the game creates for it
std::atomic<uint32_t> s_dropSequence{0};

// Actor::GetEyeVector(NiPoint3& aOrigin, NiPoint3& aDirection, bool aIncludeCameraOffset), vtable slot 0xC2 in the same
// numbering where sub_CB is the DropObject hooked in this file. For the player, the game places a drop without an
// explicit location in front of this origin, which it reads from the first person camera node (see DropSync.h).
TP_THIS_FUNCTION(TGetEyeVector, void, Actor, NiPoint3&, NiPoint3&, bool);
constexpr size_t kActorGetEyeVectorSlot = 0xC2;

bool GetEyeVector(Actor* apActor, NiPoint3& aOrigin, NiPoint3& aDirection) noexcept
{
    auto** ppVirtualTable = *reinterpret_cast<TGetEyeVector***>(apActor);
    TGetEyeVector* pGetEyeVector = ppVirtualTable ? ppVirtualTable[kActorGetEyeVectorSlot] : nullptr;
    if (!pGetEyeVector)
        return false;

    // Read only getter: it fills the two points the placement helper uses and touches nothing else
    TiltedPhoques::ThisCall(pGetEyeVector, apActor, aOrigin, aDirection, true);
    return true;
}
}

#if TP_DROPSYNC_DIAG
namespace
{
// Diagnostics for the place the game picks for a dropped reference. Everything here only reads state.
//
// For the player the game does not measure the drop from TESObjectREFR::position: it asks for the eye vector
// (Actor vtable slot 0xC2, in the same numbering where sub_CB is the DropObject hooked below) and creates the
// reference at origin + direction * distance, rotating the direction and retrying when the spot is not free.
// The origin of that eye vector is the world translation of the first person camera node hanging from
// PlayerCamera, never the actor. Every other actor keeps a fixed offset from its own position instead, which is
// what the replicated drops in the logs already show.

// Environment of a drop: none of these reads needs an offset the repository does not already map.
void LogDropEnvironment(Actor* apActor, uint32_t aDropSeq, bool aQueued) noexcept
{
    auto* pWindow = BSGraphics::GetMainWindow(); // null until the renderer is up
    auto* pUI = UI::Get();
    auto* pPlayerCamera = PlayerCamera::Get();

    spdlog::info(
        "[DropSync] drop.env seq={} actor={:X} isPlayer={} queued={} focus={} firstPerson={} invMenu={} contMenu={} favMenu={} sitting={} weaponDrawn={} "
        "actorPos=({:.1f},{:.1f},{:.1f}) actorRot=({:.4f},{:.4f},{:.4f})",
        aDropSeq, apActor->formID, apActor == static_cast<Actor*>(PlayerCharacter::Get()), aQueued,
        pWindow != nullptr && pWindow->IsForeground(), pPlayerCamera != nullptr && pPlayerCamera->IsFirstPerson(),
        pUI != nullptr && pUI->GetMenuOpen(BSFixedString("InventoryMenu")), pUI != nullptr && pUI->GetMenuOpen(BSFixedString("ContainerMenu")),
        pUI != nullptr && pUI->GetMenuOpen(BSFixedString("FavoritesMenu")), apActor->actorState.IsSittingOrSleeping(), apActor->actorState.IsWeaponDrawn(),
        apActor->position.x, apActor->position.y, apActor->position.z, apActor->rotation.x, apActor->rotation.y, apActor->rotation.z);
}

// Where the reference ended up, measured against both candidate bases at once
void LogDropPlacement(uint32_t aDropSeq, TESObjectREFR* apDropped, const NiPoint3& acActorPos, const NiPoint3& acEyeOrigin, bool aHaveEye) noexcept
{
    const glm::vec3 cFromActor = apDropped->position - acActorPos;
    const glm::vec3 cFromEye = apDropped->position - acEyeOrigin;

    spdlog::info(
        "[DropSync] drop.offset seq={} fromActor=({:.1f},{:.1f},{:.1f}) |xy|={:.1f} |xyz|={:.1f} haveEye={} fromEye=({:.1f},{:.1f},{:.1f}) |eyeXY|={:.1f} |eyeXYZ|={:.1f}",
        aDropSeq, cFromActor.x, cFromActor.y, cFromActor.z, glm::length(glm::vec2(cFromActor)), glm::length(cFromActor), aHaveEye, aHaveEye ? cFromEye.x : 0.0f,
        aHaveEye ? cFromEye.y : 0.0f, aHaveEye ? cFromEye.z : 0.0f, aHaveEye ? glm::length(glm::vec2(cFromEye)) : 0.0f, aHaveEye ? glm::length(cFromEye) : 0.0f);
}
} // namespace
#endif

#if TP_DROPSYNC_DIAG && TP_DROPSYNC_DIAG_BASE
namespace
{
// NiAVObject keeps its transforms inside the opaque pad of NetImmerse/NiAVObject.h, whose total size the header
// already pins down with a static_assert:
//   0x48 NiTransform local (NiMatrix3 rotate of 0x24, then the translation, then the scale)
//   0x7C NiTransform world
// No new VersionDb id is involved, only byte offsets inside an object of a size that is already verified. A wrong
// offset shows up in the first healthy drop, because nodeVsPos is then not close to zero.
constexpr size_t kNiAVObjectLocalTranslate = 0x48 + 0x24;
constexpr size_t kNiAVObjectWorldTranslate = 0x7C + 0x24;
static_assert(kNiAVObjectWorldTranslate + sizeof(NiPoint3) <= sizeof(NiAVObject), "NiAVObject transforms fall outside the mapped object");

// PlayerCamera::cameraStates[], whose first entry is the first person state, and the camera node that state owns.
// fpStateIsCurrent cross-checks the first offset against the current state the repository does map.
constexpr size_t kPlayerCameraStates = 0xB8;
constexpr size_t kFirstPersonStateCameraObj = 0x58;

bool LooksLikeObject(const void* apPointer) noexcept
{
    const auto cAddress = reinterpret_cast<uintptr_t>(apPointer);
    return cAddress >= 0x10000 && (cAddress & 7) == 0;
}

const void* ReadPointer(const void* apObject, size_t aOffset) noexcept
{
    if (!LooksLikeObject(apObject))
        return nullptr;

    const void* pValue = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(apObject) + aOffset);
    return LooksLikeObject(pValue) ? pValue : nullptr;
}

NiPoint3 ReadTranslate(const void* apObject, size_t aOffset) noexcept
{
    if (!LooksLikeObject(apObject))
        return NiPoint3{};

    const auto* pFloats = reinterpret_cast<const float*>(static_cast<const uint8_t*>(apObject) + aOffset);
    return NiPoint3(glm::vec3(pFloats[0], pFloats[1], pFloats[2]));
}

// Logs the base the game is about to place the reference on, next to the eye vector the hook already read
void LogDropBase(Actor* apActor, uint32_t aDropSeq, const NiPoint3& acEyeOrigin, const NiPoint3& acEyeDirection, bool aHaveEye) noexcept
{
    const bool cIsPlayerSingleton = apActor == static_cast<Actor*>(PlayerCharacter::Get());

    // PlayerCamera redeclares the TESCamera members, so its own pos/cameraNode/state shadow the real ones at the
    // wrong offsets. Read them through the base class, the way TESCamera::GetNiCamera() does.
    TESCamera* pCamera = PlayerCamera::Get();

    const NiPoint3 cCameraPos = pCamera ? pCamera->pos : NiPoint3{};
    const void* pCameraNode = pCamera ? pCamera->cameraNode : nullptr;
    const void* pCameraState = pCamera ? pCamera->state : nullptr;

    NiNode* pActorNode = apActor->GetNiNode();
    const NiPoint3 cNodeWorld = ReadTranslate(pActorNode, kNiAVObjectWorldTranslate);
    const NiPoint3 cNodeLocal = ReadTranslate(pActorNode, kNiAVObjectLocalTranslate);

    const void* pFirstPersonState = cIsPlayerSingleton ? ReadPointer(pCamera, kPlayerCameraStates) : nullptr;
    const void* pFirstPersonNode = ReadPointer(pFirstPersonState, kFirstPersonStateCameraObj);
    const NiPoint3 cEyeNodeWorld = ReadTranslate(pFirstPersonNode, kNiAVObjectWorldTranslate);
    const NiPoint3 cEyeNodeLocal = ReadTranslate(pFirstPersonNode, kNiAVObjectLocalTranslate);

    spdlog::info(
        "[DropSync] drop.base seq={} node={:X} nodeWorld=({:.1f},{:.1f},{:.1f}) nodeLocal=({:.1f},{:.1f},{:.1f}) nodeVsPos={:.1f} camPos=({:.1f},{:.1f},{:.1f}) camNode={:X} "
        "camState={:X} fpState={:X} fpStateIsCurrent={} fpNode={:X} fpWorld=({:.1f},{:.1f},{:.1f}) fpLocal=({:.1f},{:.1f},{:.1f}) fpWorldVsLocal={:.1f} fpWorldVsPos={:.1f}",
        aDropSeq, reinterpret_cast<uintptr_t>(pActorNode), cNodeWorld.x, cNodeWorld.y, cNodeWorld.z, cNodeLocal.x, cNodeLocal.y, cNodeLocal.z,
        glm::distance(glm::vec3(cNodeWorld), glm::vec3(apActor->position)), cCameraPos.x, cCameraPos.y, cCameraPos.z, reinterpret_cast<uintptr_t>(pCameraNode),
        reinterpret_cast<uintptr_t>(pCameraState), reinterpret_cast<uintptr_t>(pFirstPersonState), pFirstPersonState != nullptr && pFirstPersonState == pCameraState,
        reinterpret_cast<uintptr_t>(pFirstPersonNode), cEyeNodeWorld.x, cEyeNodeWorld.y, cEyeNodeWorld.z, cEyeNodeLocal.x, cEyeNodeLocal.y, cEyeNodeLocal.z,
        glm::distance(glm::vec3(cEyeNodeWorld), glm::vec3(cEyeNodeLocal)), glm::distance(glm::vec3(cEyeNodeWorld), glm::vec3(apActor->position)));

    if (!aHaveEye)
        return;

    spdlog::info(
        "[DropSync] drop.eye seq={} origin=({:.1f},{:.1f},{:.1f}) dir=({:.4f},{:.4f},{:.4f}) originVsActor={:.1f} originVsFpWorld={:.1f} originVsNodeWorld={:.1f}", aDropSeq,
        acEyeOrigin.x, acEyeOrigin.y, acEyeOrigin.z, acEyeDirection.x, acEyeDirection.y, acEyeDirection.z,
        glm::distance(glm::vec3(acEyeOrigin), glm::vec3(apActor->position)), glm::distance(glm::vec3(acEyeOrigin), glm::vec3(cEyeNodeWorld)),
        glm::distance(glm::vec3(acEyeOrigin), glm::vec3(cNodeWorld)));
}
} // namespace
#endif

#ifdef SAVE_STUFF

#include <Games/Skyrim/SaveLoad.h>
#include "Actor.h"

void Actor::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    BGSSaveFormBuffer buffer;

    Save(&buffer);

    AIProcess* pProcess = currentProcess;
    const int32_t handlerId = pProcess != nullptr ? pProcess->handlerId : -1;

    aWriter.WriteBytes((uint8_t*)&handlerId, 4); // TODO: is this needed ?
    aWriter.WriteBytes((uint8_t*)&flags1, 4);

    //     if (!handlerId
    //         && (uint8_t)AIProcess::GetBoolInSubStructure(pProcess))
    //     {
    //         Actor::SaveSkinFar(this);
    //     }

    TESObjectREFR::Save_Reversed(aChangeFlags, aWriter);

    if (pProcess)
        ; // Skyrim saves the process manager state, but we don't give a shit so skip !

    aWriter.WriteBytes((uint8_t*)&unk194, 4);
    aWriter.WriteBytes((uint8_t*)&headTrackingUpdateDelay, 4);
    aWriter.WriteBytes((uint8_t*)&unk9C, 4);
    // We skip 0x180 as it's not something we care about, some timer related data

    aWriter.WriteBytes((uint8_t*)&unk98, 4);
    // skip A8 - related to timers
    // skip AC - related to timers as well
    aWriter.WriteBytes((uint8_t*)&unkB0, 4);
    // skip E4 - never seen this used
    // skip E8 - same as E4
    aWriter.WriteBytes((uint8_t*)&unk84, 4);
    aWriter.WriteBytes((uint8_t*)&unkA4, 4);
    // skip baseForm->weight
    // skip 12C

    // Save actor state sub_6F0FB0
}

#endif

TP_THIS_FUNCTION(TRemoveSpell, bool, Actor, MagicItem*);
TP_THIS_FUNCTION(TCharacterConstructor, Actor*, Actor);
TP_THIS_FUNCTION(TCharacterConstructor2, Actor*, Actor, uint8_t aUnk);
TP_THIS_FUNCTION(TCharacterDestructor, Actor*, Actor);
TP_THIS_FUNCTION(TAddInventoryItem, void, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner);
TP_THIS_FUNCTION(TPickUpObject, void*, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2);
TP_THIS_FUNCTION(TDropObject, void*, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation);
TP_THIS_FUNCTION(TSetPosition, char, Actor, NiPoint3& acPosition);
TP_THIS_FUNCTION(TActorProcess, char, Actor, float aValue);

using TGetLocation = TESForm*(TESForm*);
static TGetLocation* FUNC_GetActorLocation;

TCharacterConstructor* RealCharacterConstructor;
TCharacterConstructor2* RealCharacterConstructor2;
TCharacterDestructor* RealCharacterDestructor;

static TRemoveSpell* RealRemoveSpell = nullptr;
static TAddInventoryItem* RealAddInventoryItem = nullptr;
static TPickUpObject* RealPickUpObject = nullptr;
static TDropObject* RealDropObject = nullptr;
static TSetPosition* RealSetPosition = nullptr;
static TActorProcess* RealActorProcess = nullptr;

float Actor::GetSpeed() noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    float speed = 0.f;
    animationGraphHolder.GetVariableFloat(&speedSampledStr, &speed);

    return speed;
}

void Actor::SetSpeed(float aSpeed) noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    animationGraphHolder.SetVariableFloat(&speedSampledStr, aSpeed);
}

TESNPC* Actor::GetLeveledPick() const noexcept
{
    const auto* pExtra = static_cast<ExtraLeveledCreature*>(extraData.GetByType(ExtraDataType::LeveledCreature));
    TESActorBase* pTemplate = pExtra ? pExtra->templateBase : nullptr;
    if (!pTemplate || pTemplate->formType != FormType::Npc || pTemplate->IsTemporary())
        return nullptr;

    return static_cast<TESNPC*>(pTemplate);
}

uint16_t Actor::GetLevel() const noexcept
{
    TP_THIS_FUNCTION(TGetLevel, uint16_t, const Actor);
    POINTER_SKYRIMSE(TGetLevel, s_getLevel, 37334);
    return TiltedPhoques::ThisCall(s_getLevel, this);
}

void Actor::ForcePosition(const NiPoint3& acPosition) noexcept
{
    ScopedReferencesOverride recursionGuard;

    bool updateController = true;
    if (GetExtension()->IsRemote() && currentProcess)
    {
        if (auto* pController = currentProcess->GetCharController())
        {
            // A newly created controller may be positioned before ActorProcess.
            // Both SetPositionImpl and the following velocity reset need a step
            updateController = pController->UpdateStepTiming();
        }
    }

    // With no usable step yet, update the reference/3D now; interpolation will
    // catch the controller up once physics timing becomes available
    SetPosition(acPosition, updateController);
}

void Actor::QueueUpdate() noexcept
{
    auto* pSetting = INISettingCollection::Get()->GetSetting("bUseFaceGenPreprocessedHeads:General");
    const auto originalValue = pSetting->data;
    pSetting->data = 0;

    TP_THIS_FUNCTION(TQueueUpdate, void, Actor, bool);
    POINTER_SKYRIMSE(TQueueUpdate, QueueUpdate, 40255);

    TiltedPhoques::ThisCall(QueueUpdate, this, true);

    pSetting->data = originalValue;
}

GamePtr<Actor> Actor::Create(TESNPC* apBaseForm) noexcept
{
    auto pActor = New();
    // Prevent saving
    pActor->SetSkipSaveFlag(true);
    pActor->GetExtension()->SetRemote(true);

    const auto pPlayer = static_cast<Actor*>(GetById(0x14));
    auto pCell = pPlayer->parentCell;
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    pActor->SetLevelMod(4);
    pActor->MarkChanged(0x40000000);
    pActor->SetParentCell(pCell);
    pActor->SetObjectReference(apBaseForm);

    auto position = pPlayer->position;
    auto rotation = pPlayer->rotation;

    if (pCell && !(pCell->cellFlags & 1))
        pCell = nullptr;

    ModManager::Get()->Spawn(position, rotation, pCell, pWorldSpace, pActor);

    pActor->ForcePosition(position);

    pActor->GetMagicCaster(MagicSystem::CastingSource::LEFT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::RIGHT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::OTHER);

    pActor->flags &= 0xFFDFFFFF;

    return pActor;
}

GamePtr<Actor> Actor::Spawn(uint32_t aBaseFormId) noexcept
{
    TESNPC* pNpc = Cast<TESNPC>(TESForm::GetById(aBaseFormId));
    return Actor::Create(pNpc);
}

void Actor::SetLevelMod(uint32_t aLevel) noexcept
{
    TP_THIS_FUNCTION(TActorSetLevelMod, void, ExtraDataList, uint32_t);
    POINTER_SKYRIMSE(TActorSetLevelMod, realSetLevelMod, 11806);

    const auto pExtraDataList = &extraData;

    TiltedPhoques::ThisCall(realSetLevelMod, pExtraDataList, aLevel);
}

ActorExtension* Actor::GetExtension() noexcept
{
    if (AsExActor())
    {
        return static_cast<ActorExtension*>(AsExActor());
    }

    if (AsExPlayerCharacter())
    {
        return static_cast<ActorExtension*>(AsExPlayerCharacter());
    }

    return nullptr;
}

ExActor* Actor::AsExActor() noexcept
{
    if (formType == Type && this != PlayerCharacter::Get())
        return static_cast<ExActor*>(this);

    return nullptr;
}

ExPlayerCharacter* Actor::AsExPlayerCharacter() noexcept
{
    if (this == PlayerCharacter::Get())
        return static_cast<ExPlayerCharacter*>(this);

    return nullptr;
}

extern thread_local bool g_forceAnimation;
extern thread_local int g_forcedActionResult;

void Actor::SetWeaponDrawnEx(bool aDraw) noexcept
{
    spdlog::debug("Setting weapon drawn: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());

    // When the actor already reports the requested state, the internal weapon state is flipped so that
    // the native call re-issues the draw/sheathe action and the animation is forced again.
    const bool alreadyMatching = actorState.IsWeaponDrawn() == aDraw;
    if (alreadyMatching)
    {
        actorState.SetWeaponDrawn(!aDraw);

        spdlog::debug("Setting weapon drawn after update: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());
    }

    g_forcedActionResult = -1;
    g_forceAnimation = true;
    SetWeaponDrawn(aDraw);
    g_forceAnimation = false;

    if (alreadyMatching && g_forcedActionResult != 1)
    {
        // The behavior graph refused the forced action (for example "Unequip" while already sheathed), or
        // no action was issued. Without this the actor stays in a transitional state (wants to sheathe /
        // wants to draw) that the graph never completes, and once it gains ownership its AI can no longer
        // draw the weapon (#810). Restore the stable state that was requested instead.
        actorState.SetWeaponDrawn(aDraw);

        spdlog::debug("Forced weapon draw action refused, restoring state: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());
    }
}

static thread_local bool s_execInitPackage = false;

void Actor::SetPackage(TESPackage* apPackage) noexcept
{
    s_execInitPackage = true;
    PutCreatedPackage(apPackage);
    s_execInitPackage = false;
}

void Actor::SetPlayerRespawnMode(bool aSet) noexcept
{
    SetEssentialEx(aSet);
    // Makes the player go in an unrecoverable bleedout state
    SetNoBleedoutRecovery(aSet);

    if (formID != 0x14)
    {
        auto pPlayerFaction = Cast<TESFaction>(TESForm::GetById(0xDB1));
        SetFactionRank(pPlayerFaction, 1);
    }
}

void Actor::SetEssentialEx(bool aSet) noexcept
{
    SetEssential(aSet);
    TESNPC* pBase = Cast<TESNPC>(baseForm);
    if (pBase)
        pBase->actorData.SetEssential(aSet);
}

void Actor::SetNoBleedoutRecovery(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetNoBleedoutRecovery, void, Actor, bool);
    POINTER_SKYRIMSE(TSetNoBleedoutRecovery, s_setNoBleedoutRecovery, 38533);
    TiltedPhoques::ThisCall(s_setNoBleedoutRecovery, this, aSet);
}

void Actor::DispelAllSpells(bool aNow) noexcept
{
    magicTarget.DispelAllSpells(aNow);
}

bool Actor::IsInCombat() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsInCombat);
    return s_pIsInCombat(this);
}

Actor* Actor::GetCombatTarget() const noexcept
{
    PAPYRUS_FUNCTION(Actor*, Actor, GetCombatTarget);
    return s_pGetCombatTarget(this);
}

// TODO: this is a really hacky solution.
// The internal targeting system should be disabled instead.
void Actor::StartCombatEx(Actor* apTarget) noexcept
{
    if (GetCombatTarget() != apTarget)
    {
        StopCombat();
        StartCombat(apTarget);
    }
}

void Actor::SetCombatTargetEx(Actor* apTarget) noexcept
{
    if (pCombatController)
        pCombatController->SetTarget(apTarget);
}

void Actor::StartCombat(Actor* apTarget) noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StartCombat, Actor*);
    s_pStartCombat(this, apTarget);
}

void Actor::StopCombat() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StopCombat);
    s_pStopCombat(this);
}

bool Actor::RemoveSpell(MagicItem* apSpell) noexcept
{
    if (!apSpell)
    {
        spdlog::error(__FUNCTION__ ": apSpell is null");
        return false;
    }
    // spdlog::info(__FUNCTION__ ": removing: {} from actor: {}", apSpell->formID, formID);
    return TiltedPhoques::ThisCall(RealRemoveSpell, this, apSpell);
}

bool Actor::HasPerk(uint32_t aPerkFormId) const noexcept
{
    return GetPerkRank(aPerkFormId) != 0;
}

uint8_t Actor::GetPerkRank(uint32_t aPerkFormId) const noexcept
{
    BGSPerk* pPerk = Cast<BGSPerk>(TESForm::GetById(aPerkFormId));
    if (!pPerk)
        return 0;

    TP_THIS_FUNCTION(TGetPerkRank, uint8_t, const Actor, BGSPerk*);
    POINTER_SKYRIMSE(TGetPerkRank, getPerkRank, 37698);

    return TiltedPhoques::ThisCall(getPerkRank, this, pPerk);
}

bool TP_MAKE_THISCALL(HookRemoveSpell, Actor, MagicItem* apSpell)
{
    bool result = TiltedPhoques::ThisCall(RealRemoveSpell, apThis, apSpell);
    if (apThis->GetExtension()->IsLocalPlayer() && result)
    {
        //spdlog::info(__FUNCTION__ ": spell: {}, ID: {} from local player", apSpell->GetName() , apSpell->formID);
       RemoveSpellEvent removalEvent;

        removalEvent.TargetId = apThis->formID;
        removalEvent.SpellId = apSpell->formID;
        World::Get().GetRunner().Trigger(removalEvent);
    }

    return result;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor, apThis);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor2, Actor, uint8_t aUnk)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor2, apThis, aUnk);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterDestructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    auto pExtension = apThis->GetExtension();

    if (pExtension)
    {
        pExtension->~ActorExtension();
    }

    TiltedPhoques::ThisCall(RealCharacterDestructor, apThis);

    return apThis;
}

GamePtr<Actor> Actor::New() noexcept
{
    auto* const pActor = Memory::Allocate<Actor>();

    TiltedPhoques::ThisCall(RealCharacterConstructor, pActor);

    return {pActor};
}

void Actor::InterruptCast(bool abRefund) noexcept
{
    TP_THIS_FUNCTION(TInterruptCast, void, Actor, bool abRefund);

    POINTER_SKYRIMSE(TInterruptCast, s_interruptCast, 38757);

    TiltedPhoques::ThisCall(s_interruptCast, this, abRefund);
}

TESForm* Actor::GetEquippedWeapon(uint32_t aSlotId) const noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        auto pMiddleProcess = currentProcess->middleProcess;

        if (aSlotId == 0 && pMiddleProcess->leftEquippedObject)
            return pMiddleProcess->leftEquippedObject->pObject;

        else if (aSlotId == 1 && pMiddleProcess->rightEquippedObject)
            return pMiddleProcess->rightEquippedObject->pObject;
    }

    return nullptr;
}

TESForm* Actor::GetEquippedAmmo() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->ammoEquippedObject)
    {
        // TODO: rtti cast to check if is ammo object? or actually, just call AIProcess::GetCurrentAmmo()
        return currentProcess->middleProcess->ammoEquippedObject->pObject;
    }

    return nullptr;
}

// Get owner of a summon or raised corpse
Actor* Actor::GetCommandingActor() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->commandingActor)
    {
        auto handle = currentProcess->middleProcess->commandingActor.handle;
        auto* pOwner = Cast<Actor>(TESObjectREFR::GetByHandle(handle.iBits));
        return pOwner;
    }

    return nullptr;
}

// Get owner of a summon or raised corpse
void Actor::SetCommandingActor(BSPointerHandle<TESObjectREFR> aCommandingActor) noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        currentProcess->middleProcess->commandingActor = aCommandingActor;
        flags2 |= ActorFlags::IS_COMMANDED_ACTOR;
    }
}

bool Actor::IsPlayerSummon() const noexcept
{
    const Actor* pCommandingActor = GetCommandingActor();
    return pCommandingActor && pCommandingActor->formID == 0x14;
}

TESForm* Actor::GetCurrentLocation()
{
    // we use the safe function which also
    // checks the form type
    return FUNC_GetActorLocation(this);
}

Factions Actor::GetFactions() const noexcept
{
    Factions result;

    auto& modSystem = World::Get().GetModSystem();

    auto* pNpc = Cast<TESNPC>(baseForm);
    if (pNpc)
    {
        auto& factions = pNpc->actorData.factions;

        for (auto i = 0u; i < factions.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(factions[i].faction->formID, faction.Id);
            faction.Rank = factions[i].rank;

            result.NpcFactions.push_back(faction);
        }
    }

    auto* pChanges = Cast<ExtraFactionChanges>(extraData.GetByType(ExtraDataType::Faction));
    if (pChanges)
    {
        for (auto i = 0u; i < pChanges->entries.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(pChanges->entries[i].faction->formID, faction.Id);
            faction.Rank = pChanges->entries[i].rank;

            result.ExtraFactions.push_back(faction);
        }
    }

    return result;
}

ActorValues Actor::GetEssentialActorValues() const noexcept
{
    ActorValues actorValues;

    int essentialValues[] = {ActorValueInfo::kHealth, ActorValueInfo::kStamina, ActorValueInfo::kMagicka};
    for (auto i : essentialValues)
    {
        float value = actorValueOwner.GetValue(i);
        actorValues.ActorValuesList.insert({i, value});
        float maxValue = actorValueOwner.GetPermanentValue(i);
        actorValues.ActorMaxValuesList.insert({i, maxValue});
    }

    return actorValues;
}

float Actor::GetActorValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetValue(aId);
}

float Actor::GetActorPermanentValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetPermanentValue(aId);
}

void Actor::SetActorValue(uint32_t aId, float aValue) noexcept
{
    actorValueOwner.SetValue(aId, aValue);
}

void Actor::ForceActorValue(ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue) noexcept
{
    float initialValue = aMode == ActorValueOwner::ForceMode::PERMANENT 
                         ? GetActorPermanentValue(aId)
                         : GetActorValue(aId);

    if (aValue == initialValue)
        return;

    actorValueOwner.ForceCurrent(aMode, aId, aValue - initialValue);
}

Inventory Actor::GetActorInventory() const noexcept
{
    Inventory inventory = GetInventory();

    inventory.CurrentMagicEquipment = GetMagicEquipment();

    return inventory;
}

MagicEquipment Actor::GetMagicEquipment() const noexcept
{
    MagicEquipment equipment;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t mainId = magicItems[0] ? magicItems[0]->formID : 0;
    modSystem.GetServerModId(mainId, equipment.LeftHandSpell);

    uint32_t secondaryId = magicItems[1] ? magicItems[1]->formID : 0;
    modSystem.GetServerModId(secondaryId, equipment.RightHandSpell);

    uint32_t shoutId = equippedShout ? equippedShout->formID : 0;
    modSystem.GetServerModId(shoutId, equipment.Shout);

    return equipment;
}

Inventory Actor::GetEquipment() const noexcept
{
    Inventory inventory = GetInventory();
    inventory.RemoveByFilter([](const auto& entry) { return !entry.IsWorn(); });
    inventory.CurrentMagicEquipment = GetMagicEquipment();
    return inventory;
}

int32_t Actor::GetGoldAmount() const noexcept
{
    TP_THIS_FUNCTION(TGetGoldAmount, int32_t, const Actor);
    POINTER_SKYRIMSE(TGetGoldAmount, s_getGoldAmount, 37527);
    return TiltedPhoques::ThisCall(s_getGoldAmount, this);
}

void Actor::SetActorInventory(const Inventory& acInventory) noexcept
{
    spdlog::info("Setting inventory for actor {:X}", formID);

    // The UnEquipAll() that used to be here is redundant,
    // as RemoveAllItems() unequips every item if needed.
    // Placing this UnEquipAll() here seems to trigger a Skyrim bug/race.

    Inventory currentInventory = GetActorInventory();

    if (!this->GetExtension()->IsPlayer() && currentInventory.ContainsQuestItems())
        SetInventoryRetainingQuestItems(currentInventory, acInventory);
    else
        SetInventory(acInventory);

    SetMagicEquipment(acInventory.CurrentMagicEquipment);
}

void Actor::SetMagicEquipment(const MagicEquipment& acEquipment) noexcept
{
    auto* pEquipManager = EquipManager::Get();
    auto& modSystem = World::Get().GetModSystem();

    if (acEquipment.LeftHandSpell)
    {
        uint32_t mainHandWeaponId = modSystem.GetGameId(acEquipment.LeftHandSpell);
        spdlog::debug("Setting left hand spell: {:X}", mainHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(mainHandWeaponId), 0);
    }

    if (acEquipment.RightHandSpell)
    {
        uint32_t secondaryHandWeaponId = modSystem.GetGameId(acEquipment.RightHandSpell);
        spdlog::debug("Setting right hand spell: {:X}", secondaryHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(secondaryHandWeaponId), 1);
    }

    if (acEquipment.Shout)
    {
        uint32_t shoutId = modSystem.GetGameId(acEquipment.Shout);
        spdlog::debug("Setting shout: {:X}", shoutId);
        pEquipManager->EquipShout(this, TESForm::GetById(shoutId));
    }
}

void Actor::SetActorValues(const ActorValues& acActorValues) noexcept
{
    for (auto& value : acActorValues.ActorMaxValuesList)
        ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, value.first, value.second);

    for (auto& value : acActorValues.ActorValuesList)
        ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, value.first, value.second);
}

void Actor::SetFactions(const Factions& acFactions) noexcept
{
    RemoveFromAllFactions();

    auto& modSystem = World::Get().GetModSystem();

    for (auto& entry : acFactions.NpcFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }

    for (auto& entry : acFactions.ExtraFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }
}

void Actor::SetFactionRank(const TESFaction* apFaction, int8_t aRank) noexcept
{
    TP_THIS_FUNCTION(TSetFactionRankInternal, void, Actor, const TESFaction*, int8_t);

    POINTER_SKYRIMSE(TSetFactionRankInternal, s_setFactionRankInternal, 37677);

    TiltedPhoques::ThisCall(s_setFactionRankInternal, this, apFaction, aRank);
}

void Actor::SetPlayerTeammate(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetPlayerTeammate, void, Actor, bool aSet, bool abCanDoFavor);
    POINTER_SKYRIMSE(TSetPlayerTeammate, setPlayerTeammate, 37717);
    return TiltedPhoques::ThisCall(setPlayerTeammate, this, aSet, true);
}

void Actor::UnEquipAll() noexcept
{
    EquipManager::Get()->UnequipAll(this);

    // Taken from skyrim's code shouts can be two form types apparently
    if (equippedShout && ((int)equippedShout->formType - 41) <= 1)
    {
        EquipManager::Get()->UnEquipShout(this, equippedShout);
        equippedShout = nullptr;
    }
}

void Actor::RemoveFromAllFactions() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, RemoveFromAllFactions);

    s_pRemoveFromAllFactions(this);
}

TP_THIS_FUNCTION(TInitiateMountPackage, bool, Actor, Actor* apMount);
static TInitiateMountPackage* RealInitiateMountPackage = nullptr;

bool Actor::InitiateMountPackage(Actor* apMount) noexcept
{
    return TiltedPhoques::ThisCall(RealInitiateMountPackage, this, apMount);
}

void Actor::GenerateMagicCasters() noexcept
{
    using CS = MagicSystem::CastingSource;

    for (int i = 0; i < 4; i++)
    {
        if (casters[i] == nullptr)
            casters[i] = Cast<ActorMagicCaster>(GetMagicCaster(static_cast<CS>(i)));
    }
}

bool Actor::IsDead() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsDead);

    return s_pIsDead(this);
}

bool Actor::IsDragon() const noexcept
{
    const ActorExtension* pExtension = const_cast<Actor*>(this)->GetExtension();
    return BehaviorVar::IsDragon(pExtension->GraphDescriptorHash);
}

void Actor::Kill() noexcept
{
    // Never kill players
    ActorExtension* pExtension = GetExtension();
    if (pExtension->IsPlayer())
        return;

    // TODO: these args are kind of bogus of course
    KillImpl(nullptr, 100.f, true, true);

    // Papyrus kill will not go through if it is queued by a kill move
    /*
    PAPYRUS_FUNCTION(void, Actor, Kill, void*);
    s_pKill(this, NULL);
    */
}

void Actor::Reset() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Reset, int, TESObjectREFR*);

    s_pReset(this, 0, nullptr);
}

bool Actor::PlayIdle(TESIdleForm* apIdle) noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, PlayIdle, TESIdleForm*);
    return s_pPlayIdle(this, apIdle);
}

void Actor::Respawn() noexcept
{
    Resurrect(false);
    Reset();
}

bool Actor::IsVampireLord() const noexcept
{
    return race && race->formID == 0x200283A;
}

extern thread_local bool g_forceAnimation;

void Actor::FixVampireLordModel() noexcept
{
    TESBoundObject* pLordArmor = Cast<TESBoundObject>(TESForm::GetById(0x2011a84));
    if (!pLordArmor)
        return;

    {
        ScopedInventoryOverride _;
        AddObjectToContainer(pLordArmor, nullptr, 1, nullptr);
    }

    EquipManager::Get()->Equip(this, pLordArmor, nullptr, 1, nullptr, false, true, false, false);

    g_forceAnimation = true;

    BSFixedString str("isLevitating");
    uint32_t isLevitating = GetAnimationVariableInt(&str);
    spdlog::critical("isLevitating {}", isLevitating);

    // By default, a loaded vampire lord is not levitating.
    if (isLevitating)
    {
        BSFixedString levitation("LevitationToggle");
        SendAnimationEvent(&levitation);
    }

    // TODO: weapon draw code does not seem to take care of this
    //BSFixedString weapEquip("WeapEquip");
    //SendAnimationEvent(&weapEquip);

    g_forceAnimation = false;
}

char TP_MAKE_THISCALL(HookSetPosition, Actor, NiPoint3& aPosition)
{
    const auto pExtension = apThis ? apThis->GetExtension() : nullptr;
    const auto bIsRemote = pExtension && pExtension->IsRemote();

    if (bIsRemote && !ScopedReferencesOverride::IsOverriden())
        return 1;

    // Don't interfere with non actor references, or the player, or if we are calling our self
    if (apThis->formType != Actor::Type || apThis == PlayerCharacter::Get() || ScopedReferencesOverride::IsOverriden())
        return TiltedPhoques::ThisCall(RealSetPosition, apThis, aPosition);

    ScopedReferencesOverride recursionGuard;

    // It just works TM
    apThis->SetPosition(aPosition, false);

    return 1;
}

TP_THIS_FUNCTION(TForceState, void, Actor, const NiPoint3&, float, float, TESObjectCELL*, TESWorldSpace*, bool);
static TForceState* RealForceState = nullptr;

void TP_MAKE_THISCALL(HookForceState, Actor, const NiPoint3& acPosition, float aX, float aZ, TESObjectCELL* apCell, TESWorldSpace* apWorldSpace, bool aUnkBool)
{
    /*const auto pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("For TESNPC: {}, spawn at {} {} {}", pNpc->fullName.value, apPosition->m_x, apPosition->m_y,
                     apPosition->m_z);
    }*/

    // if (apThis != PlayerCharacter::Get())
    //     return;

    return TiltedPhoques::ThisCall(RealForceState, apThis, acPosition, aX, aZ, apCell, apWorldSpace, aUnkBool);
}

TP_THIS_FUNCTION(TSpawnActorInWorld, bool, Actor);
static TSpawnActorInWorld* RealSpawnActorInWorld = nullptr;

// TODO: this isn't SpawnActorInWorld, this is TESObjectREFR::UpdateReference3D()
bool TP_MAKE_THISCALL(HookSpawnActorInWorld, Actor)
{
    const auto* pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("Spawn Actor: {:X}, and NPC {}", apThis->formID, pNpc->fullName.value);
    }

    return TiltedPhoques::ThisCall(RealSpawnActorInWorld, apThis);
}

TP_THIS_FUNCTION(TDamageActor, bool, Actor, float aDamage, Actor* apHitter, bool aKillMove);
static TDamageActor* RealDamageActor = nullptr;

// TODO: this is flawed, since it does not account for invulnerable actors
bool TP_MAKE_THISCALL(HookDamageActor, Actor, float aDamage, Actor* apHitter, bool aKillMove)
{
    if (apHitter)
        World::Get().GetRunner().Trigger(HitEvent(apHitter->formID, apThis->formID));

    float realDamage = GameplayFormulas::CalculateRealDamage(apThis, aDamage, aKillMove);

    float currentHealth = apThis->GetActorValue(ActorValueInfo::kHealth);
    bool wouldKill = (currentHealth - realDamage) <= 0.f;

    const auto* pExHittee = apThis->GetExtension();
    if (pExHittee->IsLocalPlayer())
    {
        if (!World::Get().GetServerSettings().PvpEnabled)
        {
            if (apHitter && apHitter->GetExtension()->IsRemotePlayer())
                return false;
        }

        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
    }
    else if (pExHittee->IsRemotePlayer())
    {
        return wouldKill;
    }

    if (apHitter)
    {
        const auto* pExHitter = apHitter->GetExtension();
        if (pExHitter->IsLocalPlayer())
        {
            World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
            return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
        }
        if (pExHitter->IsRemotePlayer())
        {
            return wouldKill;
        }
    }

    if (pExHittee->IsLocal())
    {
        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -realDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
    }
    else
    {
        return wouldKill;
    }
}

TP_THIS_FUNCTION(TApplyActorEffect, void, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1);
static TApplyActorEffect* RealApplyActorEffect = nullptr;

void TP_MAKE_THISCALL(HookApplyActorEffect, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1)
{
    const auto* pValueModEffect = Cast<ValueModifierEffect>(apThis);

    if (pValueModEffect)
    {
        if (pValueModEffect->actorValueIndex == ActorValueInfo::kHealth && aEffectValue > 0.0f)
        {
            if (apTarget && apTarget->GetExtension())
            {
                const auto pExTarget = apTarget->GetExtension();
                if (pExTarget->IsLocal())
                {
                    World::Get().GetRunner().Trigger(HealthChangeEvent(apTarget->formID, aEffectValue));
                    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
                }
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
}

TP_THIS_FUNCTION(TRegenAttributes, void*, Actor, int aId, float regenValue);
static TRegenAttributes* RealRegenAttributes = nullptr;

void* TP_MAKE_THISCALL(HookRegenAttributes, Actor, int aId, float aRegenValue)
{
    if (aId != ActorValueInfo::kHealth)
    {
        return TiltedPhoques::ThisCall(RealRegenAttributes, apThis, aId, aRegenValue);
    }

    const auto* pExTarget = apThis->GetExtension();
    if (pExTarget->IsRemote())
    {
        return 0;
    }

    World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, aRegenValue));
    return TiltedPhoques::ThisCall(RealRegenAttributes, apThis, aId, aRegenValue);
}

void TP_MAKE_THISCALL(HookAddInventoryItem, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner)
{
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);
        item.Count = aCount;

        if (apExtraData)
            apThis->GetItemFromExtraData(item, apExtraData);

        QueueActorInventoryChange(apThis, InventoryChangeEvent(apThis->formID, std::move(item)), apOldOwner);
    }

    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apItem, apExtraData, aCount, apOldOwner);
}

void* TP_MAKE_THISCALL(HookPickUpObject, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2)
{
    if (!ScopedInventoryOverride::IsOverriden())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apObject->baseForm->formID, item.BaseId);
        item.Count = aCount;

        if (apObject->GetExtraDataList())
            apThis->GetItemFromExtraData(item, apObject->GetExtraDataList());

        // This is here so that objects that are picked up on both clients, aka non temps, are synced through activation sync.
        // The inventory change event should always be sent to the server, otherwise the server inventory won't be updated.
        bool shouldUpdateClients = apObject->IsTemporary() && !ScopedActivateOverride::IsOverriden();

        InventoryChangeEvent event(apThis->formID, std::move(item), false, shouldUpdateClients);

        // Original references can also be tracked drops. The service decides which network path owns this pickup.
        if (!ScopedActivateOverride::IsOverriden())
        {
            event.GroundRefHandle = apObject->GetHandle().handle.iBits;
            event.GroundRefFormId = apObject->formID;
            event.GroundRefBaseId = apObject->baseForm->formID;

#if TP_DROPSYNC_DIAG
            spdlog::info(
                "[DropSync] pickup.begin hook=Actor actor={:X} ref={:X} handle={:X} base={:X} aCount={} entryCount={} flags={:X} pos=({:.1f},{:.1f},{:.1f}) cell={:X}", apThis->formID,
                apObject->formID, event.GroundRefHandle, event.GroundRefBaseId, aCount, event.Item.Count, apObject->flags, apObject->position.x, apObject->position.y,
                apObject->position.z, apObject->parentCell ? apObject->parentCell->formID : 0);
#endif
        }

        QueueActorInventoryChange(apThis, std::move(event));
    }

    return TiltedPhoques::ThisCall(RealPickUpObject, apThis, apObject, aCount, aUnk1, aUnk2);
}

void Actor::PickUpObject(TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2) noexcept
{
    TiltedPhoques::ThisCall(RealPickUpObject, this, apObject, aCount, aUnk1, aUnk2);
}

void* TP_MAKE_THISCALL(HookDropObject, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation)
{
    auto& modSystem = World::Get().GetModSystem();

    Inventory::Entry item{};
    modSystem.GetServerModId(apObject->formID, item.BaseId);

    if (apExtraData)
        apThis->GetItemFromExtraData(item, apExtraData);

    // The extra data of a stack carries its own count, which must not replace the (negative) dropped amount
    if (apExtraData && item.Count != 0 && item.Count != aCount)
        spdlog::warn("[DropSync] drop count taken from the call, not from ExtraCount: base={:X} aCount={} extraCount={}", apObject->formID, aCount, item.Count);
    item.Count = -aCount;

    const uint32_t cDropSeq = ++s_dropSequence;

    InventoryChangeEvent event(apThis->formID, std::move(item), true);
    event.DropSeq = cDropSeq;

    const bool cQueued = QueueActorInventoryChange(apThis, std::move(event));

#if TP_DROPSYNC_DIAG
    if (cQueued)
        spdlog::info(
            "[DropSync] drop.begin seq={} actor={:X} base={:X} aCount={} hasExtra={} hasLocation={} actorPos=({:.1f},{:.1f},{:.1f})", cDropSeq, apThis->formID, apObject->formID, aCount,
            apExtraData != nullptr, apLocation != nullptr, apThis->position.x, apThis->position.y, apThis->position.z);
#endif

    // The game places a drop of the player without a location in front of its eye origin, never measured from the
    // actor. When the first person camera node loses its parent transform, that origin is reported at the cell origin
    // and the item is created hundreds of units away, out of sight (ls3, 17/09). Give such a drop the placement the
    // game uses for any other actor instead.
    const bool cIsPlayer = apThis == static_cast<Actor*>(PlayerCharacter::Get());
    NiPoint3 eyeOrigin{};
    NiPoint3 eyeDirection{};
    const bool cHaveEye = cIsPlayer && GetEyeVector(apThis, eyeOrigin, eyeDirection);

    NiPoint3 fallbackLocation{};
    NiPoint3* pLocation = apLocation;
    if (!apLocation && cHaveEye && !DropSync::IsEyeOriginUsable(eyeOrigin, apThis->position))
    {
        fallbackLocation = NiPoint3(DropSync::ActorDropLocation(apThis->position, apThis->rotation.z));
        pLocation = &fallbackLocation;

        spdlog::warn(
            "[DropSync] eye origin unusable, drop placed in front of the player instead: seq={} base={:X} eyeOrigin=({:.1f},{:.1f},{:.1f}) actorPos=({:.1f},{:.1f},{:.1f}) "
            "location=({:.1f},{:.1f},{:.1f})",
            cDropSeq, apObject->formID, eyeOrigin.x, eyeOrigin.y, eyeOrigin.z, apThis->position.x, apThis->position.y, apThis->position.z, fallbackLocation.x,
            fallbackLocation.y, fallbackLocation.z);
    }

#if TP_DROPSYNC_DIAG
    // Placement diagnostics. They do not depend on cQueued, because one of the controls is a single client that
    // never connects and queues nothing; drops of other actors stay out of the log unless they are replicated.
    const bool cTracePlacement = cQueued || cIsPlayer;
    const NiPoint3 cActorPosAtDrop = apThis->position;

    if (cTracePlacement)
    {
        LogDropEnvironment(apThis, cDropSeq, cQueued);
#if TP_DROPSYNC_DIAG_BASE
        LogDropBase(apThis, cDropSeq, eyeOrigin, eyeDirection, cHaveEye);
#endif
    }
#endif

    void* pReturned = nullptr;
    {
        ScopedInventoryOverride _;
        pReturned = TiltedPhoques::ThisCall(RealDropObject, apThis, apResult, apObject, apExtraData, aCount, pLocation, apRotation);
    }

    // Only drops that went to the server are tracked; everything else behaves as before
    if (cQueued)
    {
        DroppedItemEvent dropped{};
        dropped.DropSeq = cDropSeq;
        dropped.HandleBits = HandleFromDropResult(pReturned, apResult);
        dropped.BaseFormId = apObject->formID;
        dropped.ActorFormId = apThis->formID;
        dropped.Count = aCount;
        dropped.Queued = cQueued;

        if (TESObjectREFR* pDropped = dropped.HandleBits ? TESObjectREFR::GetByHandle(dropped.HandleBits) : nullptr)
        {
            dropped.RefFormId = pDropped->formID;
            dropped.PositionX = pDropped->position.x;
            dropped.PositionY = pDropped->position.y;
            dropped.PositionZ = pDropped->position.z;

#if TP_DROPSYNC_DIAG
            spdlog::info(
                "[DropSync] drop.created seq={} handle={:X} ref={:X} flags={:X} loaded={} pos=({:.1f},{:.1f},{:.1f}) cell={:X} returnedIsArg={}", cDropSeq, dropped.HandleBits,
                pDropped->formID, pDropped->flags, pDropped->loadedState != nullptr, pDropped->position.x, pDropped->position.y, pDropped->position.z,
                pDropped->parentCell ? pDropped->parentCell->formID : 0, pReturned == apResult);

            LogDropPlacement(cDropSeq, pDropped, cActorPosAtDrop, eyeOrigin, cHaveEye);
#endif
        }
        else
            spdlog::warn("[DropSync] drop.created NO REF: seq={} handle={:X} actor={:X} base={:X} aCount={}", cDropSeq, dropped.HandleBits, apThis->formID, apObject->formID, aCount);

        World::Get().GetRunner().Trigger(std::move(dropped));
    }

#if TP_DROPSYNC_DIAG
    // Same measurement for the single client control, where the drop never reaches a server
    if (!cQueued && cTracePlacement)
    {
        const uint32_t cHandleBits = HandleFromDropResult(pReturned, apResult);

        // GetByHandle releases the reference its lookup takes, so the handle count is left untouched
        if (TESObjectREFR* pDropped = cHandleBits ? TESObjectREFR::GetByHandle(cHandleBits) : nullptr)
        {
            spdlog::info(
                "[DropSync] drop.created seq={} handle={:X} ref={:X} flags={:X} loaded={} pos=({:.1f},{:.1f},{:.1f}) cell={:X} returnedIsArg={} queued=false", cDropSeq, cHandleBits,
                pDropped->formID, pDropped->flags, pDropped->loadedState != nullptr, pDropped->position.x, pDropped->position.y, pDropped->position.z,
                pDropped->parentCell ? pDropped->parentCell->formID : 0, pReturned == apResult);

            LogDropPlacement(cDropSeq, pDropped, cActorPosAtDrop, eyeOrigin, cHaveEye);
        }
    }
#endif

    return pReturned;
}

uint32_t Actor::DropOrPickUpObject(const Inventory::Entry& arEntry, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("Object to drop not found, {:X}:{:X}.", arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        return 0;
    }

    // Pickups of dropped items do not come through here: they arrive as a plain inventory change that names the
    // drop id, and InventoryService removes this client's copy of the ground reference.
    if (arEntry.Count >= 0)
    {
        spdlog::warn("[DropSync] DropOrPickUpObject ignored a non-drop entry: base={:X} count={}", objectId, arEntry.Count);
        return 0;
    }

    ExtraDataList* pExtraData = GetExtraDataFromItem(arEntry);
    return DropObject(pObject, pExtraData, -arEntry.Count, apLocation, apRotation);
}

uint32_t Actor::DropObject(TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    BSPointerHandle<TESObjectREFR> result{};

    // RealDropObject is the player's override of DropObject. Besides RemoveItem it keeps the player's own drops in
    // fields that only PlayerCharacter has, and writes them when the new reference has no 3D yet: on any other actor
    // that lands past the end of the object. RemoveItem with the dropping reason is the part both overrides share.
    if (this == static_cast<Actor*>(PlayerCharacter::Get()))
        TiltedPhoques::ThisCall(RealDropObject, this, &result, apObject, apExtraData, aCount, apLocation, apRotation);
    else
        result = RemoveItem(apObject, aCount, ITEM_REMOVE_REASON::kDropping, apExtraData, nullptr, apLocation, apRotation);

    spdlog::info("Dropping object, form id: {:X}, count: {}, actor: {:X}, result handle: {:X}", apObject->formID, aCount, formID, result.handle.iBits);
    return result.handle.iBits;
}

TP_THIS_FUNCTION(TUpdateDetectionState, void, ActorKnowledge, void*);
static TUpdateDetectionState* RealUpdateDetectionState = nullptr;

void TP_MAKE_THISCALL(HookUpdateDetectionState, ActorKnowledge, void* apState)
{
    auto pOwner = TESObjectREFR::GetByHandle(apThis->hOwner);
    auto pTarget = TESObjectREFR::GetByHandle(apThis->hTarget);

    if (pOwner && pTarget)
    {
        auto pOwnerActor = Cast<Actor>(pOwner);
        auto pTargetActor = Cast<Actor>(pTarget);
        if (pOwnerActor && pTargetActor)
        {
            if (pOwnerActor->GetExtension()->IsRemotePlayer() && pTargetActor->GetExtension()->IsLocalPlayer())
            {
                spdlog::debug("Cancelling detection from remote player to local player, owner: {:X}, target: {:X}", pOwner->formID, pTarget->formID);
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealUpdateDetectionState, apThis, apState);
}

struct DialogueItem;

// TODO: This is an AIProcess function
TP_THIS_FUNCTION(TProcessResponse, uint64_t, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor);
static TProcessResponse* RealProcessResponse = nullptr;

uint64_t TP_MAKE_THISCALL(HookProcessResponse, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor)
{
    if (apTalkingActor)
    {
        if (apTalkingActor->GetExtension()->IsRemotePlayer())
            return 0;  
    }
    return TiltedPhoques::ThisCall(RealProcessResponse, apThis, apVoice, apTalkingActor, apTalkedToActor);
}

bool TP_MAKE_THISCALL(HookInitiateMountPackage, Actor, Actor* apMount)
{
    if (apMount && apThis->GetExtension()->IsLocal())
        World::Get().GetRunner().Trigger(MountEvent(apThis->formID, apMount->formID));

    return TiltedPhoques::ThisCall(RealInitiateMountPackage, apThis, apMount);
}

TP_THIS_FUNCTION(TUnequipObject, void, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3);
static TUnequipObject* RealUnequipObject = nullptr;

void TP_MAKE_THISCALL(HookUnequipObject, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3)
{
    TiltedPhoques::ThisCall(RealUnequipObject, apThis, apUnk1, apObject, aUnk2, apUnk3);
}

TP_THIS_FUNCTION(TSpeakSoundFunction, float, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14);
static TSpeakSoundFunction* RealSpeakSoundFunction = nullptr;

float TP_MAKE_THISCALL(HookSpeakSoundFunction, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14)
{
    // Note most dialogues invoke SpeakSoundFunction twice, an initial call that queues it to an update thread, then a reinvocation.

    spdlog::debug("a3: {:X}, a4: {}, a5: {}, a6: {}, a7: {}, a8: {:X}, a9: {:X}, a10: {}, a11: {:X}, a12: {}, a13: {}, a14: {}", (uint64_t)a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);

    // The player having the conversation may not own this NPC. Ambient
    // speech still comes only from the actor's simulation owner.
    if (apThis->GetExtension()->IsLocal() || MenuTopicManager::IsPlayerDialogueSpeaker(apThis))
        World::Get().GetRunner().Trigger(DialogueEvent(apThis->formID, apName));

    return TiltedPhoques::ThisCall(RealSpeakSoundFunction, apThis, apName, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

float Actor::SpeakSound(const char* pFile)
{
    uint32_t handle[3]{};
    handle[0] = -1;

    return TiltedPhoques::ThisCall(RealSpeakSoundFunction, this, pFile, handle, 0, 0x32, 0, 0, 0, 0, 0, 0, 0, 1, 1);
}

bool Actor::IsTalking() noexcept
{
    TP_THIS_FUNCTION(TIsTalking, bool, Actor);
    POINTER_SKYRIMSE(TIsTalking, s_IsTalking, 37266);
    return TiltedPhoques::ThisCall(s_IsTalking, this);
}

bool Actor::IsInScene() noexcept
{
    // We don't have a solution for Condition Functions
    //PAPYRUS_FUNCTION(bool, Actor, IsInScene);
    //bool papyrusValue = s_pIsInScene(this);
    bool flagsValue= (flags1 & ActorBoolBits::kHasSceneExtra) != 0;

    return flagsValue;
}

bool Actor::IsInDialogueWithPlayer() noexcept
{
    using ObjectReference = TESObjectREFR;
    PAPYRUS_FUNCTION(bool, ObjectReference, IsInDialogueWithPlayer);
    return s_pIsInDialogueWithPlayer(this);
}

float Actor::GetVoiceRecoveryTime() noexcept
{
    // Doesn't work. And Wiki only describes w.r.t shouts
    // PAPYRUS_FUNCTION(float, Actor, GetVoiceRecoveryTime);
    // float fVRT = s_pGetVoiceRecoveryTime(this);
    // if (fVRT != fVoiceTimer)
    //    spdlog::warn(__FUNCTION__ ": fVRT {}, fVoiceTimer {}", fVRT, fVoiceTimer);

    return fVoiceTimer;
}

bool Actor::IsSpeakingInScene() 
{
    auto pScene = GetCurrentScene();
    bool isSpeakingInScene = IsInScene(); 
    isSpeakingInScene = isSpeakingInScene && GetVoiceRecoveryTime() > 0.0f;
    const bool isTalking = IsTalking(); 
    const bool isLeader = World::Get().GetPartyService().IsLeader(); // Helps distinguish logs in 2-party

    spdlog::debug(__FUNCTION__ ": isSpeakingInScene {}, isTalking {}, voiceRecoveryTime {}, isLeader {}, formId {:X}, name {}", isSpeakingInScene, isTalking, GetVoiceRecoveryTime(), isLeader, formID, baseForm->GetName());

    return isSpeakingInScene;
}

char TP_MAKE_THISCALL(HookActorProcess, Actor, float aDeltaTime)
{
    // Suppress local movement for remote actors, but preserve controller timing.
    if (apThis->GetExtension()->IsRemote())
    {
        if (apThis->currentProcess)
        {
            if (auto* pController = apThis->currentProcess->GetCharController())
                pController->UpdateStepTiming(aDeltaTime);
        }
        return 0;
    }

    return TiltedPhoques::ThisCall(RealActorProcess, apThis, aDeltaTime);
}

TP_THIS_FUNCTION(TAddDeathItems, void, Actor);
static TAddDeathItems* RealAddDeathItems = nullptr;

void TP_MAKE_THISCALL(HookAddDeathItems, Actor)
{
    if (apThis->GetExtension()->IsRemote())
        return;

    TiltedPhoques::ThisCall(RealAddDeathItems, apThis);
}

TP_THIS_FUNCTION(TIsFleeing, bool, Actor);
static TIsFleeing* RealIsFleeing = nullptr;

bool TP_MAKE_THISCALL(HookIsFleeing, Actor)
{
    // TODO: Player or RemotePlayer? Can players be in fleeing mode in skyrim?
    // TODO: investigate why the flee flag is set at all on remote players sometimes.
    if (apThis->GetExtension()->IsPlayer())
        return false;
    
    return TiltedPhoques::ThisCall(RealIsFleeing, apThis);
}

static TiltedPhoques::Initializer s_actorHooks(
    []()
    {
        POINTER_SKYRIMSE(TActorProcess, s_actorProcess, 37356);
        POINTER_SKYRIMSE(TSetPosition, s_setPosition, 19790);
        POINTER_SKYRIMSE(TRemoveSpell, s_removeSpell, 38717);
        POINTER_SKYRIMSE(TCharacterConstructor, s_characterCtor, 40245);
        POINTER_SKYRIMSE(TCharacterConstructor2, s_characterCtor2, 40246);
        POINTER_SKYRIMSE(TCharacterDestructor, s_characterDtor, 37175);
        POINTER_SKYRIMSE(TGetLocation, s_GetActorLocation, 19812);
        POINTER_SKYRIMSE(TForceState, s_ForceState, 37313);
        POINTER_SKYRIMSE(TSpawnActorInWorld, s_SpawnActorInWorld, 19742);
        POINTER_SKYRIMSE(TDamageActor, s_damageActor, 37335);
        POINTER_SKYRIMSE(TApplyActorEffect, s_applyActorEffect, 35086);
        POINTER_SKYRIMSE(TRegenAttributes, s_regenAttributes, 37448);
        POINTER_SKYRIMSE(TAddInventoryItem, s_addInventoryItem, 37525);
        POINTER_SKYRIMSE(TPickUpObject, s_pickUpObject, 37521);
        POINTER_SKYRIMSE(TDropObject, s_dropObject, 40454);
        POINTER_SKYRIMSE(TUpdateDetectionState, s_updateDetectionState, 42704);
        POINTER_SKYRIMSE(TProcessResponse, s_processResponse, 39643);
        POINTER_SKYRIMSE(TInitiateMountPackage, s_initiateMountPackage, 37905);
        POINTER_SKYRIMSE(TUnequipObject, s_unequipObject, 37975);
        POINTER_SKYRIMSE(TSpeakSoundFunction, s_speakSoundFunction, 37542);
        POINTER_SKYRIMSE(TAddDeathItems, addDeathItems, 37198);
        POINTER_SKYRIMSE(TIsFleeing, isFleeing, 37577);

        RealActorProcess = s_actorProcess.Get();
        RealSetPosition = s_setPosition.Get();
        RealRemoveSpell = s_removeSpell.Get();
        FUNC_GetActorLocation = s_GetActorLocation.Get();
        RealCharacterConstructor = s_characterCtor.Get();
        RealCharacterConstructor2 = s_characterCtor2.Get();
        RealForceState = s_ForceState.Get();
        RealSpawnActorInWorld = s_SpawnActorInWorld.Get();
        RealDamageActor = s_damageActor.Get();
        RealApplyActorEffect = s_applyActorEffect.Get();
        RealRegenAttributes = s_regenAttributes.Get();
        RealAddInventoryItem = s_addInventoryItem.Get();
        RealPickUpObject = s_pickUpObject.Get();
        RealDropObject = s_dropObject.Get();
        RealUpdateDetectionState = s_updateDetectionState.Get();
        RealProcessResponse = s_processResponse.Get();
        RealInitiateMountPackage = s_initiateMountPackage.Get();
        RealUnequipObject = s_unequipObject.Get();
        RealSpeakSoundFunction = s_speakSoundFunction.Get();
        RealAddDeathItems = addDeathItems.Get();
        RealIsFleeing = isFleeing.Get();

        TP_HOOK(&RealActorProcess, HookActorProcess);
        TP_HOOK(&RealSetPosition, HookSetPosition);
        TP_HOOK(&RealRemoveSpell, HookRemoveSpell);
        TP_HOOK(&RealCharacterConstructor, HookCharacterConstructor);
        TP_HOOK(&RealCharacterConstructor2, HookCharacterConstructor2);
        TP_HOOK(&RealForceState, HookForceState);
        TP_HOOK(&RealSpawnActorInWorld, HookSpawnActorInWorld);
        TP_HOOK(&RealDamageActor, HookDamageActor);
        TP_HOOK(&RealApplyActorEffect, HookApplyActorEffect);
        TP_HOOK(&RealRegenAttributes, HookRegenAttributes);
        TP_HOOK(&RealAddInventoryItem, HookAddInventoryItem);
        TP_HOOK(&RealPickUpObject, HookPickUpObject);
        TP_HOOK(&RealDropObject, HookDropObject);
        TP_HOOK(&RealUpdateDetectionState, HookUpdateDetectionState);
        TP_HOOK(&RealProcessResponse, HookProcessResponse);
        TP_HOOK(&RealInitiateMountPackage, HookInitiateMountPackage);
        TP_HOOK(&RealUnequipObject, HookUnequipObject);
        TP_HOOK(&RealSpeakSoundFunction, HookSpeakSoundFunction);
        TP_HOOK(&RealAddDeathItems, HookAddDeathItems);
        TP_HOOK(&RealIsFleeing, HookIsFleeing);
    });
