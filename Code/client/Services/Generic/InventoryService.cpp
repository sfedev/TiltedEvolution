#include <Services/InventoryService.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/NotifyDropPickupRejected.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/DroppedItemEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <DropSync.h>

#include <cmath>

#include <World.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <DefaultObjectManager.h>

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
    m_droppedItemConnection = m_dispatcher.sink<DroppedItemEvent>().connect<&InventoryService::OnDroppedItemEvent>(this);
    m_dropPickupRejectedConnection = m_dispatcher.sink<NotifyDropPickupRejected>().connect<&InventoryService::OnDropPickupRejected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&InventoryService::OnDisconnected>(this);
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    RunWeaponStateUpdates();
    RunDroppedItemUpdates();
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // The game has consumed the ground reference by now, so its binding goes away whatever happens to the request
    std::optional<DroppedItemTracker::Binding> pickedUpDrop{};
    bool matchedByHandle = false;
    if (acEvent.GroundRefHandle != 0 || acEvent.GroundRefFormId != 0)
    {
        CancelRemovalChecks(acEvent.GroundRefFormId);
        pickedUpDrop = m_droppedItems.TakeByRef(acEvent.GroundRefHandle, acEvent.GroundRefFormId, acEvent.GroundRefBaseId, &matchedByHandle);
    }

    const bool cIsDropSyncEvent = acEvent.Drop || acEvent.DropSeq != 0 || acEvent.GroundRefHandle != 0;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.FormId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
    {
        if (cIsDropSyncEvent)
            spdlog::warn(
                "[DropSync] not sent: reason=actor entity not found actor={:X} base={:X} count={} seq={} groundHandle={:X} dropId={:X}", acEvent.FormId, acEvent.Item.BaseId.BaseId,
                acEvent.Item.Count, acEvent.DropSeq, acEvent.GroundRefHandle, pickedUpDrop ? pickedUpDrop->DropId : 0);
        return;
    }

    uint32_t serverId = 0;
    if (acEvent.OwnershipEpoch != 0)
    {
        const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
        const auto* pRemoteComponent = m_world.try_get<RemoteComponent>(*iter);
        const bool ownershipMatches = (pLocalComponent && pLocalComponent->Id == acEvent.ServerId && pLocalComponent->OwnershipEpoch == acEvent.OwnershipEpoch)
            || (pRemoteComponent && pRemoteComponent->Id == acEvent.ServerId && pRemoteComponent->OwnershipEpoch == acEvent.OwnershipEpoch);

        if (!ownershipMatches)
        {
            spdlog::debug("Discarded an inventory change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
            if (cIsDropSyncEvent)
                spdlog::warn(
                    "[DropSync] not sent: reason=ownership changed (epoch {}) actor={:X} base={:X} count={} seq={} groundHandle={:X} dropId={:X}", acEvent.OwnershipEpoch, acEvent.FormId,
                    acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.DropSeq, acEvent.GroundRefHandle, pickedUpDrop ? pickedUpDrop->DropId : 0);
            return;
        }

        serverId = acEvent.ServerId;
    }
    else
    {
        if (Cast<Actor>(TESForm::GetById(acEvent.FormId)))
            return;

        const std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
        if (!serverIdRes)
        {
            spdlog::warn(
                "Discarded inventory change for form {:X} because it has no server entity (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }
        serverId = *serverIdRes;
    }

    RequestInventoryChanges request;
    request.ServerId = serverId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;

    if (acEvent.Drop && acEvent.DropSeq != 0 && acEvent.Item.Count < 0 && m_transport.GetLocalPlayerId() != 0)
    {
        // The reference does not exist yet. The request waits for OnDroppedItemEvent, which comes right after it in the
        // same queue: it binds the reference to this id and adds where the game placed the item, so the other clients
        // create their copy on that spot instead of against this actor's body.
        request.DropId = DropSync::MakeDropId(m_transport.GetLocalPlayerId(), acEvent.DropSeq);

        PendingDrop pending{};
        pending.Request = request;
        pending.QueuedAt = TClock::now();
        m_announcedDrops[acEvent.DropSeq] = pending;

        spdlog::info(
            "[DropSync] drop request held until the game places the reference: seq={} dropId={:X} item={:X} count={}", acEvent.DropSeq, request.DropId,
            acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
        return;
    }
    else if (pickedUpDrop && !acEvent.Drop && acEvent.Item.Count > 0)
    {
        request.DropId = pickedUpDrop->DropId;
        request.UpdateClients = true;
        m_droppedItems.MarkClaimedLocally(pickedUpDrop->DropId);

        spdlog::info(
            "[DropSync] pickup of known drop: dropId={:X} ref={:X} handle={:X} base={:X} count={} local={} matchedBy={} -> claim sent", pickedUpDrop->DropId, acEvent.GroundRefFormId,
            acEvent.GroundRefHandle, acEvent.GroundRefBaseId, acEvent.Item.Count, pickedUpDrop->Local, matchedByHandle ? "handle" : "formid");
    }
    else if (acEvent.GroundRefHandle != 0 || acEvent.GroundRefFormId != 0)
    {
        spdlog::info(
            "[DropSync] pickup of untracked ref: ref={:X} handle={:X} base={:X} count={} bindings={} updateClients={} -> legacy path",
            acEvent.GroundRefFormId, acEvent.GroundRefHandle, acEvent.GroundRefBaseId, acEvent.Item.Count, m_droppedItems.Size(), request.UpdateClients);
    }

    m_transport.Send(request);

    spdlog::info(
        "Sending item request, item: {:X}, count: {}, target object: {:X}, drop: {}, updateClients: {}, dropId: {:X}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId,
        request.Drop, request.UpdateClients, request.DropId);
}

void InventoryService::SendHeldDrop(uint32_t aDropSeq, PendingDrop& aPending) noexcept
{
    const RequestInventoryChanges& request = aPending.Request;
    m_transport.Send(request);
    aPending.Sent = true;

    spdlog::info(
        "Sending item request, item: {:X}, count: {}, target server id: {:X}, drop: {}, updateClients: {}, dropId: {:X}, seq: {}, position: {}", request.Item.BaseId.BaseId,
        request.Item.Count, request.ServerId, request.Drop, request.UpdateClients, request.DropId, aDropSeq,
        request.HasDropPosition ? fmt::format("({:.1f},{:.1f},{:.1f})", request.DropPosition.x, request.DropPosition.y, request.DropPosition.z) : std::string("none"));
}

void InventoryService::OnDroppedItemEvent(const DroppedItemEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    CancelRemovalChecks(acEvent.RefFormId);

    uint64_t dropId = 0;
    if (m_announcedDrops.find(acEvent.DropSeq) != m_announcedDrops.end())
    {
        // The map hands out values as const through its iterators
        PendingDrop& pending = m_announcedDrops[acEvent.DropSeq];
        dropId = pending.Request.DropId;

        if (!pending.Sent)
        {
            if (acEvent.HandleBits != 0 && acEvent.RefFormId != 0)
            {
                pending.Request.HasDropPosition = true;
                pending.Request.DropPosition = glm::vec3(acEvent.PositionX, acEvent.PositionY, acEvent.PositionZ);
            }

            SendHeldDrop(acEvent.DropSeq, pending);
        }

        m_announcedDrops.erase(acEvent.DropSeq);
    }

    if (dropId == 0)
    {
        spdlog::info(
            "[DropSync] local drop not announced (request discarded or no player id yet): seq={} handle={:X} ref={:X} base={:X} -> untracked", acEvent.DropSeq, acEvent.HandleBits,
            acEvent.RefFormId, acEvent.BaseFormId);
    }
    else if (acEvent.HandleBits == 0 || acEvent.RefFormId == 0)
    {
        spdlog::warn("[DropSync] local drop has no ref to bind: dropId={:X} seq={} base={:X} count={}", dropId, acEvent.DropSeq, acEvent.BaseFormId, acEvent.Count);
    }
    else
    {
        DroppedItemTracker::Binding binding{};
        binding.DropId = dropId;
        binding.Handle = acEvent.HandleBits;
        binding.RefFormId = acEvent.RefFormId;
        binding.BaseFormId = acEvent.BaseFormId;
        binding.Count = acEvent.Count;
        binding.Local = true;

        if (const auto evicted = m_droppedItems.Bind(binding))
            spdlog::warn("[DropSync] binding evicted (cap {}): dropId={:X} local={}", DroppedItemTracker::kMaxBindings, evicted->DropId, evicted->Local);

        spdlog::info(
            "[DropSync] bound local drop: dropId={:X} seq={} handle={:X} ref={:X} base={:X} count={} bindings={}", dropId, acEvent.DropSeq, acEvent.HandleBits, acEvent.RefFormId,
            acEvent.BaseFormId, acEvent.Count, m_droppedItems.Size());
    }

#if TP_DROPSYNC_DIAG
    if (acEvent.HandleBits != 0)
    {
        DropProbe probe{};
        probe.DropId = dropId;
        probe.DropSeq = acEvent.DropSeq;
        probe.Handle = acEvent.HandleBits;
        probe.RefFormId = acEvent.RefFormId;
        probe.BaseFormId = acEvent.BaseFormId;
        probe.Local = true;
        probe.SpawnX = acEvent.PositionX;
        probe.SpawnY = acEvent.PositionY;
        probe.SpawnZ = acEvent.PositionZ;
        probe.CreatedAt = TClock::now();
        m_dropProbes.push_back(probe);
    }
#endif
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.ActorId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    const auto* pLocalComponent = m_world.try_get<LocalComponent>(*iter);
    if (acEvent.OwnershipEpoch == 0 || !pLocalComponent || pLocalComponent->Id != acEvent.ServerId || pLocalComponent->OwnershipEpoch != acEvent.OwnershipEpoch)
    {
        spdlog::debug("Discarded an equipment change for actor {:X} because ownership changed after it was queued (epoch {})", acEvent.ServerId, acEvent.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = acEvent.ServerId;
    request.OwnershipEpoch = acEvent.OwnershipEpoch;

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}", acEvent.ItemId, acEvent.Count, acEvent.ActorId);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.Drop || acMessage.DropId != 0)
        spdlog::info(
            "[DropSync] notify: actor={:X} epoch={} item={:X}:{:X} count={} drop={} dropId={:X}", acMessage.ServerId, acMessage.OwnershipEpoch, acMessage.Item.BaseId.ModId,
            acMessage.Item.BaseId.BaseId, acMessage.Item.Count, acMessage.Drop, acMessage.DropId);

    // Native pickup of an original reference also changes the remote actor's inventory. Never add it a second
    // time below. Deferred removals never replay inventory: a later actor snapshot already includes the pickup.
    if (acMessage.DropId != 0 && !acMessage.Drop && acMessage.Item.Count > 0)
    {
        const auto result = ApplyRemotePickup(acMessage.DropId, acMessage.ServerId, acMessage.OwnershipEpoch, false);
        if (result == PickupResult::InventoryHandled)
            return;
    }

    if (acMessage.OwnershipEpoch != 0)
    {
        Actor* pActor = nullptr;

        auto remoteView = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
        const auto remoteIt = std::find_if(remoteView.begin(), remoteView.end(), [remoteView, &acMessage](const entt::entity aEntity)
        {
            const auto& remoteComponent = remoteView.get<RemoteComponent>(aEntity);
            return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
        });

        if (remoteIt != remoteView.end())
            pActor = Cast<Actor>(TESForm::GetById(remoteView.get<FormIdComponent>(*remoteIt).Id));
        else
        {
            auto localView = m_world.view<LocalComponent, FormIdComponent>();
            const auto localIt = std::find_if(localView.begin(), localView.end(), [localView, &acMessage](const entt::entity aEntity)
            {
                const auto& localComponent = localView.get<LocalComponent>(aEntity);
                return localComponent.Id == acMessage.ServerId && localComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
            });

            if (localIt != localView.end())
                pActor = Cast<Actor>(TESForm::GetById(localView.get<FormIdComponent>(*localIt).Id));
        }

        if (!pActor)
        {
            spdlog::debug("Discarded an inventory update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
            if (acMessage.Drop || acMessage.DropId != 0)
                spdlog::info(
                    "[DropSync] notify: actor {:X} epoch {} not resolvable here; inventory part skipped (dropId={:X} count={})", acMessage.ServerId, acMessage.OwnershipEpoch,
                    acMessage.DropId, acMessage.Item.Count);
            return;
        }

        ScopedInventoryOverride _;

        if (acMessage.Drop)
        {
            // Create the copy where the dropping client saw the item appear. Without a position the game uses its
            // placement for any actor other than the player, right against the remote character's body.
            NiPoint3 dropLocation{};
            NiPoint3* pDropLocation = nullptr;
            if (acMessage.DropId != 0 && acMessage.Item.Count < 0 && acMessage.HasDropPosition)
            {
                if (DropSync::IsReplicatedDropPositionUsable(acMessage.DropPosition, pActor->position))
                {
                    dropLocation = NiPoint3(glm::vec3(acMessage.DropPosition));
                    pDropLocation = &dropLocation;
                }
                else
                    spdlog::warn(
                        "[DropSync] replicated drop position ignored, too far from the remote actor: dropId={:X} position=({:.1f},{:.1f},{:.1f}) actor={:X} actorPos=({:.1f},{:.1f},{:.1f})",
                        acMessage.DropId, acMessage.DropPosition.x, acMessage.DropPosition.y, acMessage.DropPosition.z, pActor->formID, pActor->position.x, pActor->position.y,
                        pActor->position.z);
            }

            const uint32_t cHandle = pActor->DropOrPickUpObject(acMessage.Item, pDropLocation, nullptr);
            if (acMessage.DropId != 0 && acMessage.Item.Count < 0)
                BindReplicatedDrop(acMessage, cHandle, pActor->formID, pDropLocation != nullptr);
        }
        else
            pActor->AddOrRemoveItem(acMessage.Item);

        return;
    }

    TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
    if (!pObject)
        return;

    ScopedInventoryOverride _;
    pObject->AddOrRemoveItem(acMessage.Item);
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
    const auto it = std::find_if(view.begin(), view.end(), [view, &acMessage](const entt::entity aEntity)
    {
        const auto& remoteComponent = view.get<RemoteComponent>(aEntity);
        return remoteComponent.Id == acMessage.ServerId && remoteComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
    });
    if (it == view.end())
    {
        spdlog::debug("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }

    // TODO: ExtraData necessary? probably
    if (acMessage.Unequip)
    {
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
    }
    else
    {
        // Unequip all armor first, since the game won't auto unequip armor
        Inventory wornArmor{};
        if (pItem->formType == FormType::Armor)
        {
            wornArmor = pActor->GetWornArmor();
            for (const auto& armor : wornArmor.Entries)
            {
                uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                TESForm* pArmor = TESForm::GetById(armorId);
                if (pArmor)
                    pEquipManager->UnEquip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
            }
        }

        pEquipManager->Equip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);

        for (const auto& armor : wornArmor.Entries)
        {
            uint32_t armorId = modSystem.GetGameId(armor.BaseId);
            TESForm* pArmor = TESForm::GetById(armorId);
            if (pArmor)
                pEquipManager->Equip(pActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false);
        }
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}

TESObjectREFR* InventoryService::ResolveDroppedReference(uint32_t aHandle, uint32_t aRefFormId, uint32_t aBaseFormId, const char*& aprResolvedBy) const noexcept
{
    // Handle slots and temporary form ids are both recycled by the game, so each is only trusted together with
    // something else: the handle with the form id it had, the form id with the base form. Original references can
    // be reused by drops as well. Mutations below separately distinguish native pickup from temporary deletion.
    if (aHandle != 0)
    {
        TESObjectREFR* pRef = TESObjectREFR::GetByHandle(aHandle);
        if (pRef && pRef->formID == aRefFormId && pRef->baseForm && pRef->baseForm->formID == aBaseFormId)
        {
            aprResolvedBy = "handle";
            return pRef;
        }
    }

    if (aRefFormId != 0)
    {
        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(aRefFormId));
        if (pRef && pRef->formType != Actor::Type && pRef->baseForm && pRef->baseForm->formID == aBaseFormId)
        {
            aprResolvedBy = "formid";
            return pRef;
        }
    }

    aprResolvedBy = "none";
    return nullptr;
}

void InventoryService::BindReplicatedDrop(const NotifyInventoryChanges& acMessage, uint32_t aHandle, uint32_t aRemoteActorFormId, bool aAtDropperPosition) noexcept
{
    TESObjectREFR* pRef = aHandle != 0 ? TESObjectREFR::GetByHandle(aHandle) : nullptr;
    if (!pRef)
    {
        spdlog::warn(
            "[DropSync] replicated drop produced NO REF: dropId={:X} remoteActor={:X} base={:X}:{:X} count={} handle={:X} (the remote actor may not hold the item here)", acMessage.DropId,
            aRemoteActorFormId, acMessage.Item.BaseId.ModId, acMessage.Item.BaseId.BaseId, acMessage.Item.Count, aHandle);
        return;
    }

    DroppedItemTracker::Binding binding{};
    binding.DropId = acMessage.DropId;
    binding.Handle = aHandle;
    binding.RefFormId = pRef->formID;
    binding.BaseFormId = pRef->baseForm ? pRef->baseForm->formID : 0;
    binding.Count = -acMessage.Item.Count;
    binding.Local = false;

    CancelRemovalChecks(binding.RefFormId);

    if (const auto evicted = m_droppedItems.Bind(binding))
        spdlog::warn("[DropSync] binding evicted (cap {}): dropId={:X} local={}", DroppedItemTracker::kMaxBindings, evicted->DropId, evicted->Local);

    spdlog::info(
        "[DropSync] replicated drop: dropId={:X} remoteActor={:X} base={:X} count={} -> handle={:X} ref={:X} loaded={} pos=({:.1f},{:.1f},{:.1f}) cell={:X} bindings={} placedAt={}",
        acMessage.DropId, aRemoteActorFormId, binding.BaseFormId, binding.Count, aHandle, pRef->formID, pRef->loadedState != nullptr, pRef->position.x, pRef->position.y,
        pRef->position.z, pRef->parentCell ? pRef->parentCell->formID : 0, m_droppedItems.Size(), aAtDropperPosition ? "dropper" : "remoteActor");

#if TP_DROPSYNC_DIAG
    DropProbe probe{};
    probe.DropId = acMessage.DropId;
    probe.DropSeq = DropSync::GetSeq(acMessage.DropId);
    probe.Handle = aHandle;
    probe.RefFormId = binding.RefFormId;
    probe.BaseFormId = binding.BaseFormId;
    probe.Local = false;
    probe.SpawnX = pRef->position.x;
    probe.SpawnY = pRef->position.y;
    probe.SpawnZ = pRef->position.z;
    probe.CreatedAt = TClock::now();
    m_dropProbes.push_back(probe);
#endif
}

void InventoryService::CancelRemovalChecks(uint32_t aRefFormId) noexcept
{
    if (aRefFormId == 0)
        return;
    m_removalChecks.erase(std::remove_if(m_removalChecks.begin(), m_removalChecks.end(), [aRefFormId](const RemovalCheck& acCheck)
    {
        if (acCheck.RefFormId != aRefFormId)
            return false;
        spdlog::info("[DropSync] removal verification cancelled: dropId={:X} ref={:X} has a new drop/pickup", acCheck.DropId, aRefFormId);
        return true;
    }), m_removalChecks.end());
}

InventoryService::PickupResult InventoryService::ApplyRemotePickup(uint64_t aDropId, uint32_t aPickerServerId, uint32_t aPickerOwnershipEpoch, bool aIsRetry) noexcept
{
    const DroppedItemTracker::Binding* pBinding = m_droppedItems.Find(aDropId);
    if (!pBinding)
    {
        const char* pReason = m_droppedItems.WasClaimedLocally(aDropId) ? "already picked up locally, the server arbitrates" : "unknown drop id (never replicated here, drops disabled or joined later)";
        spdlog::info("[DropSync] remote pickup not applied: dropId={:X} picker={:X} reason={}", aDropId, aPickerServerId, pReason);
        m_droppedItems.Unpark(aDropId);
        return PickupResult::Complete;
    }

    const DroppedItemTracker::Binding binding = *pBinding;

    const char* pResolvedBy = "none";
    TESObjectREFR* pRef = ResolveDroppedReference(binding.Handle, binding.RefFormId, binding.BaseFormId, pResolvedBy);
    if (!pRef)
    {
        // Typically the cell it lies in is not loaded here right now
        const bool cParked = m_droppedItems.Park(aDropId, aPickerServerId, aPickerOwnershipEpoch);
        if (!cParked)
            spdlog::warn("[DropSync] removal could not be parked: dropId={:X}; inventory notification still processed", aDropId);
        if (!aIsRetry)
            spdlog::info(
                "[DropSync] remote pickup not applied: dropId={:X} picker={:X} reason=ref not resolvable now (handle={:X} ref={:X}), removal parked={}", aDropId, aPickerServerId,
                binding.Handle, binding.RefFormId, cParked);
        return PickupResult::Pending;
    }

    m_droppedItems.Unpark(aDropId);

    const uint32_t cBaseFormId = pRef->baseForm ? pRef->baseForm->formID : 0;
    if (pRef->formType == Actor::Type || cBaseFormId != binding.BaseFormId)
    {
        spdlog::warn(
            "[DropSync] remote pickup not applied: dropId={:X} picker={:X} reason=bound reference changed (ref={:X} temporary={} base={:X} expected base={:X}); binding dropped", aDropId,
            aPickerServerId, pRef->formID, pRef->IsTemporary(), cBaseFormId, binding.BaseFormId);
        m_droppedItems.Remove(aDropId);
        return PickupResult::Complete;
    }

    if (pRef->IsDeleted())
    {
        spdlog::info("[DropSync] remote pickup not applied: dropId={:X} picker={:X} reason=ref {:X} already deleted", aDropId, aPickerServerId, pRef->formID);
        m_droppedItems.Remove(aDropId);
        return PickupResult::Complete;
    }

    if (!pRef->IsTemporary())
    {
        auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<LocalComponent>);
        const auto it = std::find_if(view.begin(), view.end(), [view, aPickerServerId, aPickerOwnershipEpoch](const entt::entity aEntity)
        {
            const auto& remote = view.get<RemoteComponent>(aEntity);
            return remote.Id == aPickerServerId && remote.OwnershipEpoch == aPickerOwnershipEpoch;
        });
        Actor* pPicker = it != view.end() ? Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*it).Id)) : nullptr;
        if (aIsRetry || !pPicker || binding.Count <= 0)
        {
            // A later actor snapshot may already contain this item. Never replay a native pickup after waiting,
            // nor award it to a different ownership epoch. Retire only the world copy, preserving the original
            // reference. The initial notification (or a later snapshot) handles inventory exactly once.
            ScopedInventoryOverride inventoryOverride;
            pRef->DisableImpl();
            spdlog::info("[DropSync] original world copy retired: dropId={:X} ref={:X} picker={:X} epoch={} retry={} actorAvailable={} disabled={}; no native inventory replay",
                aDropId, binding.RefFormId, aPickerServerId, aPickerOwnershipEpoch, aIsRetry, pPicker != nullptr, pRef->IsDisabled());
            m_droppedItems.Remove(aDropId);
            RemovalCheck check{};
            check.DropId = aDropId;
            check.Handle = binding.Handle;
            check.RefFormId = binding.RefFormId;
            check.BaseFormId = binding.BaseFormId;
            check.RemovedAt = TClock::now();
            check.NativePickup = true; // diagnostic only: never Delete an original reference
            m_removalChecks.push_back(check);
            return PickupResult::Complete;
        }

        const auto countItem = [pPicker, cBaseFormId]()
        {
            int64_t count = 0;
            const auto inventory = pPicker->GetInventory([cBaseFormId](TESForm& aForm) { return aForm.formID == cBaseFormId; });
            for (const auto& entry : inventory.Entries)
                count += entry.Count;
            return count;
        };
        const auto before = countItem();
        // The original reference's extra data belongs to the engine. Pick it up instead of permanently deleting
        // it or adding a second, unrelated inventory entry. The wrapper bypasses our Actor pickup hook.
        {
            ScopedInventoryOverride inventoryOverride;
            ScopedActivateOverride activateOverride;
            pPicker->PickUpObject(pRef, binding.Count, false, 0.0f);
        }
        const auto after = countItem();
        spdlog::info("[DropSync] native pickup applied: dropId={:X} picker={:X} epoch={} ref={:X} base={:X} count={} inventoryBefore={} inventoryAfter={} delta={} retry={}; plain inventory add skipped",
            aDropId, aPickerServerId, aPickerOwnershipEpoch, binding.RefFormId, binding.BaseFormId, binding.Count, before, after, after - before, aIsRetry);
        if (after - before != binding.Count)
            spdlog::warn("[DropSync] native pickup inventory mismatch: dropId={:X} expected={} actual={}; no blind retry or extra add", aDropId, binding.Count, after - before);

        m_droppedItems.Remove(aDropId);
        RemovalCheck check{};
        check.DropId = aDropId;
        check.Handle = binding.Handle;
        check.RefFormId = binding.RefFormId;
        check.BaseFormId = binding.BaseFormId;
        check.RemovedAt = TClock::now();
        check.NativePickup = true;
        m_removalChecks.push_back(check);
        return PickupResult::InventoryHandled;
    }

    spdlog::info(
        "[DropSync] remote pickup applied: dropId={:X} picker={:X} ref={:X} handle={:X} resolvedBy={} flagsBefore={:X} loaded={} pos=({:.1f},{:.1f},{:.1f}) retry={} -> Delete()", aDropId,
        aPickerServerId, pRef->formID, binding.Handle, pResolvedBy, pRef->flags, pRef->loadedState != nullptr, pRef->position.x, pRef->position.y, pRef->position.z, aIsRetry);

    {
        ScopedInventoryOverride _;
        pRef->Delete();
    }

    m_droppedItems.Remove(aDropId);

    RemovalCheck check{};
    check.DropId = aDropId;
    check.Handle = binding.Handle;
    check.RefFormId = binding.RefFormId;
    check.BaseFormId = binding.BaseFormId;
    check.RemovedAt = TClock::now();
    m_removalChecks.push_back(check);

    return PickupResult::Complete;
}

void InventoryService::OnDropPickupRejected(const NotifyDropPickupRejected& acMessage) noexcept
{
    // This client picked up its copy of a dropped item a moment after another player picked up theirs. The game
    // already added the item here, so it is given back, unless that could take away the wrong instance.
    auto localView = m_world.view<LocalComponent, FormIdComponent>();
    const auto localIt = std::find_if(localView.begin(), localView.end(), [localView, &acMessage](const entt::entity aEntity)
    {
        const auto& localComponent = localView.get<LocalComponent>(aEntity);
        return localComponent.Id == acMessage.ServerId && localComponent.OwnershipEpoch == acMessage.OwnershipEpoch;
    });

    Actor* pActor = localIt != localView.end() ? Cast<Actor>(TESForm::GetById(localView.get<FormIdComponent>(*localIt).Id)) : nullptr;

    const uint32_t cBaseFormId = m_world.GetModSystem().GetGameId(acMessage.Item.BaseId);

    if (!pActor || cBaseFormId == 0 || acMessage.Item.Count <= 0)
    {
        spdlog::warn(
            "[DropSync] pickup REJECTED by server: dropId={:X} claimedBy={:X} actor={:X} base={:X}:{:X} count={} -> actor or item not resolvable, nothing done", acMessage.DropId,
            acMessage.ClaimedByPlayerId, acMessage.ServerId, acMessage.Item.BaseId.ModId, acMessage.Item.BaseId.BaseId, acMessage.Item.Count);
        return;
    }

    const Inventory inventory = pActor->GetInventory([cBaseFormId](TESForm& aForm) { return aForm.formID == cBaseFormId; });

    int32_t have = 0;
    bool anyExtra = false;
    for (const auto& entry : inventory.Entries)
    {
        have += entry.Count;
        anyExtra = anyExtra || entry.ContainsExtraData();
    }

    // Removing by base form takes whichever instance the game chooses. That is only safe when every instance is the
    // one just picked up, or when none of them differs from the others (no tempering, enchantment or charge).
    const bool cSafeToRollBack = have >= acMessage.Item.Count && (have == acMessage.Item.Count || !anyExtra);

    if (!cSafeToRollBack)
    {
        spdlog::warn(
            "[DropSync] pickup REJECTED by server: dropId={:X} claimedBy={:X} actor={:X} base={:X} count={} have={} anyExtra={} -> kept (ambiguous instance), resync sent", acMessage.DropId,
            acMessage.ClaimedByPlayerId, pActor->formID, cBaseFormId, acMessage.Item.Count, have, anyExtra);

        // The item stays, so tell the server and the other players that this actor really has it
        RequestInventoryChanges request;
        request.ServerId = acMessage.ServerId;
        request.OwnershipEpoch = acMessage.OwnershipEpoch;
        request.Item = acMessage.Item;
        m_transport.Send(request);
        return;
    }

    spdlog::warn(
        "[DropSync] pickup REJECTED by server: dropId={:X} claimedBy={:X} actor={:X} base={:X} count={} have={} anyExtra={} -> rolled back", acMessage.DropId, acMessage.ClaimedByPlayerId,
        pActor->formID, cBaseFormId, acMessage.Item.Count, have, anyExtra);

    Inventory::Entry removal = acMessage.Item;
    removal.Count = -acMessage.Item.Count;

    {
        ScopedInventoryOverride _;
        pActor->AddOrRemoveItem(removal);
    }

    if (pActor == PlayerCharacter::Get())
        Utils::ShowHudMessage("Another player picked that item up first.");
}

void InventoryService::RunDroppedItemUpdates() noexcept
{
    if (m_dropProbes.empty() && m_removalChecks.empty() && m_droppedItems.Parked().empty() && m_announcedDrops.empty())
        return;

    const auto now = TClock::now();

    // A held drop request normally leaves with its reference in the same frame or the next one. The inventory change
    // must never stay behind: without a reference in time it goes without a position, and the others place their copy
    // as before. A sent entry is kept a while longer so a late reference can still be bound to its id.
    if (!m_announcedDrops.empty())
    {
        constexpr auto cHoldTimeout = std::chrono::seconds(1);
        constexpr auto cBindTimeout = std::chrono::seconds(30);

        TiltedPhoques::Vector<uint32_t> pendingSeqs;
        for (const auto& [dropSeq, pending] : m_announcedDrops)
            pendingSeqs.push_back(dropSeq);

        TiltedPhoques::Vector<uint32_t> expired;
        for (const uint32_t dropSeq : pendingSeqs)
        {
            // The map hands out values as const through its iterators
            PendingDrop& pending = m_announcedDrops[dropSeq];
            const auto cAge = now - pending.QueuedAt;
            if (!pending.Sent && cAge >= cHoldTimeout && m_transport.IsConnected())
            {
                spdlog::warn("[DropSync] drop request sent without a position: the game did not report the reference in time (seq={} dropId={:X})", dropSeq, pending.Request.DropId);
                SendHeldDrop(dropSeq, pending);
            }

            if (pending.Sent && cAge >= cBindTimeout)
                expired.push_back(dropSeq);
        }

        for (const uint32_t cDropSeq : expired)
            m_announcedDrops.erase(cDropSeq);
    }

#if TP_DROPSYNC_DIAG
    // Where is a freshly dropped reference half a second and three seconds later? Tells a reference that was never
    // created from one that lost its 3D, was deleted, or was pushed away by physics.
    for (auto& probe : m_dropProbes)
    {
        const auto cAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - probe.CreatedAt).count();
        const int64_t cDue = probe.Stage == 0 ? 500 : 3000;
        if (cAge < cDue)
            continue;

        probe.Stage++;

        const char* pResolvedBy = "none";
        TESObjectREFR* pRef = ResolveDroppedReference(probe.Handle, probe.RefFormId, probe.BaseFormId, pResolvedBy);
        const bool cStillBound = probe.DropId != 0 && m_droppedItems.Find(probe.DropId) != nullptr;

        if (!pRef)
        {
            if (cStillBound)
                spdlog::warn(
                    "[DropSync] track: dropId={:X} seq={} local={} t+{}ms exists=false ref={:X} handle={:X} -> reference vanished without a pickup", probe.DropId, probe.DropSeq, probe.Local,
                    cAge, probe.RefFormId, probe.Handle);
            else
                spdlog::info(
                    "[DropSync] track: dropId={:X} seq={} local={} t+{}ms exists=false ref={:X} handle={:X} (no longer bound: picked up)", probe.DropId, probe.DropSeq, probe.Local, cAge,
                    probe.RefFormId, probe.Handle);
            continue;
        }

        const float cMovedX = pRef->position.x - probe.SpawnX;
        const float cMovedY = pRef->position.y - probe.SpawnY;
        const float cMovedZ = pRef->position.z - probe.SpawnZ;
        const float cMoved = std::sqrt(cMovedX * cMovedX + cMovedY * cMovedY + cMovedZ * cMovedZ);

        float distanceToPlayer = -1.f;
        if (const auto* pPlayer = PlayerCharacter::Get())
        {
            const float cDx = pRef->position.x - pPlayer->position.x;
            const float cDy = pRef->position.y - pPlayer->position.y;
            const float cDz = pRef->position.z - pPlayer->position.z;
            distanceToPlayer = std::sqrt(cDx * cDx + cDy * cDy + cDz * cDz);
        }

        spdlog::info(
            "[DropSync] track: dropId={:X} seq={} local={} t+{}ms exists=true resolvedBy={} ref={:X} flags={:X} deleted={} disabled={} loaded={} has3D={} pos=({:.1f},{:.1f},{:.1f}) "
            "movedFromSpawn={:.1f} cell={:X} distToPlayer={:.1f} bound={}",
            probe.DropId, probe.DropSeq, probe.Local, cAge, pResolvedBy, pRef->formID, pRef->flags, pRef->IsDeleted(), pRef->IsDisabled(), pRef->loadedState != nullptr,
            pRef->GetNiNode() != nullptr, pRef->position.x, pRef->position.y, pRef->position.z, cMoved, pRef->parentCell ? pRef->parentCell->formID : 0, distanceToPlayer, cStillBound);
    }

    m_dropProbes.erase(std::remove_if(m_dropProbes.begin(), m_dropProbes.end(), [](const DropProbe& aProbe) { return aProbe.Stage >= 2; }), m_dropProbes.end());
#else
    m_dropProbes.clear();
#endif

    // Is a reference removed by a remote pickup really gone? Delete() had only been used on temporary actors so far.
    for (auto& check : m_removalChecks)
    {
        const auto cAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - check.RemovedAt).count();
        if (cAge < 500 * (check.Attempt + 1))
            continue;

        check.Attempt++;

        // A form-id fallback could find a later incarnation and disable somebody else's item. Verification
        // only trusts the captured handle; a new tracked drop/pickup cancels this check altogether.
        TESObjectREFR* pRef = check.Handle ? TESObjectREFR::GetByHandle(check.Handle) : nullptr;
        if (pRef && (pRef->formID != check.RefFormId || !pRef->baseForm || pRef->baseForm->formID != check.BaseFormId))
            pRef = nullptr;
        const bool cStillThere = pRef && !pRef->IsDeleted() && !pRef->IsDisabled() && (!check.NativePickup || pRef->parentCell != nullptr);

        if (!cStillThere)
        {
            spdlog::info(
                "[DropSync] remote pickup verify: dropId={:X} t+{}ms gone (exists={} deleted={} disabled={})", check.DropId, cAge, pRef != nullptr, pRef ? pRef->IsDeleted() : false,
                pRef ? pRef->IsDisabled() : false);
            check.Attempt = 0xFF;
            continue;
        }

        spdlog::warn(
            "[DropSync] remote pickup verify FAILED: dropId={:X} ref={:X} still present t+{}ms native={} (flags={:X} has3D={}), attempt {}", check.DropId, pRef->formID, cAge,
            check.NativePickup, pRef->flags, pRef->GetNiNode() != nullptr, check.Attempt);

        // Fallback, once: take it out of the world at the engine level
        if (!check.NativePickup && pRef->IsTemporary() && check.Attempt == 1)
        {
            ScopedInventoryOverride _;
            pRef->DisableImpl();
        }
        else
            check.Attempt = 0xFF;
    }

    m_removalChecks.erase(std::remove_if(m_removalChecks.begin(), m_removalChecks.end(), [](const RemovalCheck& aCheck) { return aCheck.Attempt == 0xFF; }), m_removalChecks.end());

    // Removals whose reference was not loaded here when the pickup arrived
    if (!m_droppedItems.Parked().empty() && now >= m_nextParkedRetry)
    {
        m_nextParkedRetry = now + 2s;

        const auto parked = m_droppedItems.Parked();
        for (const auto& removal : parked)
        {
            const auto result = ApplyRemotePickup(removal.DropId, removal.PickerServerId, removal.PickerOwnershipEpoch, true);
            if (result == PickupResult::Complete || result == PickupResult::InventoryHandled)
                spdlog::info("[DropSync] parked removal resolved: dropId={:X} parked={}", removal.DropId, m_droppedItems.Parked().size());
        }
    }
}

void InventoryService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    if (m_droppedItems.Size() != 0 || !m_announcedDrops.empty() || !m_dropProbes.empty() || !m_droppedItems.Parked().empty())
        spdlog::info(
            "[DropSync] cleared on disconnect: bindings={} parked={} announced={} probes={}", m_droppedItems.Size(), m_droppedItems.Parked().size(), m_announcedDrops.size(),
            m_dropProbes.size());

    m_droppedItems.Clear();
    m_announcedDrops.clear();
    m_dropProbes.clear();
    m_removalChecks.clear();
}
