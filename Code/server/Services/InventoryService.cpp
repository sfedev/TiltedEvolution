#include "InventoryService.h"

#include <Components.h>
#include <World.h>
#include <GameServer.h>

#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyDropPickupRejected.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>

#include <DropSync.h>
#include <Setting.h>
namespace
{
Console::Setting bEnableItemDrops{"Gameplay:bEnableItemDrops", "(Experimental) Syncs dropped items by players", false};
}

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_inventoryChangeConnection = aDispatcher.sink<PacketEvent<RequestInventoryChanges>>().connect<&InventoryService::OnInventoryChanges>(this);
    m_equipmentChangeConnection = aDispatcher.sink<PacketEvent<RequestEquipmentChanges>>().connect<&InventoryService::OnEquipmentChanges>(this);
    m_drawWeaponConnection = aDispatcher.sink<PacketEvent<DrawWeaponRequest>>().connect<&InventoryService::OnWeaponDrawnRequest>(this);
}

void InventoryService::OnInventoryChanges(const PacketEvent<RequestInventoryChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it == view.end())
        return;

    bool isRemoteNpcInteraction = false;

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        const auto* pOwner = pOwnerComponent->GetOwner();
        if (pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
            spdlog::debug(
                "Rejected inventory change from player {:X} for actor {:X} at stale epoch {}; current owner is {:X} at epoch {}",
                acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch, ownerId, pOwnerComponent->OwnershipEpoch);
            // Debug lines never reach the log file, and a lost drop or pickup leaves the clients out of sync
            if (message.Drop || message.DropId)
                spdlog::info(
                    "[DropSync] request rejected: player={:X} actor={:X} reason=stale epoch msgEpoch={} curEpoch={} owner={:X} dropId={:X}", acMessage.pPlayer->GetId(),
                    message.ServerId, message.OwnershipEpoch, pOwnerComponent->OwnershipEpoch, ownerId, message.DropId);
            return;
        }

        if (pOwner != acMessage.pPlayer)
        {
            const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(*it);
            const auto* pCellComponent = m_world.try_get<CellIdComponent>(*it);
            // A non-owner may still change an NPC's inventory through normal gameplay interactions
            // such as pickpocketing or looting. The epoch and range checks keep the interaction tied
            // to the currently visible incarnation of that NPC.
            isRemoteNpcInteraction = pOwner && pCharacterComponent && pCellComponent && !pCharacterComponent->IsPlayer()
                && acMessage.pPlayer->GetCellComponent().IsInRange(*pCellComponent, pCharacterComponent->IsDragon());

            if (!isRemoteNpcInteraction)
            {
                const uint32_t ownerId = pOwner ? pOwner->GetId() : 0;
                spdlog::debug(
                    "Rejected inventory change from player {:X} for actor {:X} because it is owned by player {:X}", acMessage.pPlayer->GetId(), message.ServerId, ownerId);
                if (message.Drop || message.DropId)
                    spdlog::info(
                        "[DropSync] request rejected: player={:X} actor={:X} reason=not the owner msgEpoch={} curEpoch={} owner={:X} dropId={:X}", acMessage.pPlayer->GetId(),
                        message.ServerId, message.OwnershipEpoch, pOwnerComponent->OwnershipEpoch, ownerId, message.DropId);
                return;
            }
        }
    }
    else if (message.OwnershipEpoch != 0)
    {
        spdlog::warn(
            "Rejected inventory change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
            acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    // Dropped item sync (see DropSync.h). A drop id is only meaningful on a drop (Drop, Count < 0), where it names
    // the ground reference being created, and on a pickup (!Drop, Count > 0), where it names the one consumed.
    const uint32_t cPlayerId = acMessage.pPlayer->GetId();
    const bool isDropAnnounce = message.DropId != 0 && message.Drop && message.Item.Count < 0;
    const bool isPickupClaim = message.DropId != 0 && !message.Drop && message.Item.Count > 0;
    uint64_t dropId = message.DropId;

    if (dropId != 0 && (isRemoteNpcInteraction || (!isDropAnnounce && !isPickupClaim)))
    {
        spdlog::warn(
            "[DropSync] malformed drop info stripped: player={:X} actor={:X} drop={} count={} dropId={:X} remoteNpc={}", cPlayerId, message.ServerId, message.Drop,
            message.Item.Count, dropId, isRemoteNpcInteraction);
        dropId = 0;
    }

    if (dropId != 0 && isDropAnnounce && DropSync::GetPlayerId(dropId) != cPlayerId)
    {
        spdlog::warn("[DropSync] drop id {:X} does not carry sender player id {:X}; id stripped", dropId, cPlayerId);
        dropId = 0;
    }

    // With drop sync off nobody else has a copy of the item, so there is nothing to arbitrate. Drops announced
    // before the setting was turned off at runtime still are.
    if (dropId != 0 && isPickupClaim && !bEnableItemDrops && !m_dropClaims.IsAnnounced(dropId))
    {
#if TP_DROPSYNC_DIAG
        spdlog::info("[DropSync] pickup of unannounced id with drops disabled: player={:X} dropId={:X} -> id stripped (legacy path)", cPlayerId, dropId);
#endif
        dropId = 0;
    }

    if (dropId != 0 && isPickupClaim)
    {
        const auto outcome = m_dropClaims.TryClaim(dropId, cPlayerId, message.Item.BaseId.ModId, message.Item.BaseId.BaseId);

        if (outcome.Type == DropClaims::Result::kAlreadyClaimed)
        {
            // First claim wins. The loser already added the item locally: tell only that player to give it back.
            // The server inventory is not touched and nobody else hears about it.
            spdlog::warn(
                "[DropSync] pickup REJECTED, already claimed by player {:X}: player={:X} actor={:X} dropId={:X}; rolling back {} x {:X}:{:X}", outcome.ClaimedByPlayerId,
                cPlayerId, message.ServerId, dropId, message.Item.Count, message.Item.BaseId.ModId, message.Item.BaseId.BaseId);

            NotifyDropPickupRejected rejected;
            rejected.DropId = dropId;
            rejected.ServerId = message.ServerId;
            rejected.OwnershipEpoch = message.OwnershipEpoch;
            rejected.Item = message.Item;
            rejected.ClaimedByPlayerId = outcome.ClaimedByPlayerId;
            acMessage.pPlayer->Send(rejected);
            return;
        }

        if (outcome.Type == DropClaims::Result::kBaseMismatch)
        {
            spdlog::warn(
                "[DropSync] pickup base mismatch: player={:X} dropId={:X} claimed={:X}:{:X}; claim not consumed, relayed as untracked pickup", cPlayerId, dropId,
                message.Item.BaseId.ModId, message.Item.BaseId.BaseId);
            dropId = 0;
        }
        else
        {
            spdlog::info(
                "[DropSync] pickup claimed: player={:X} actor={:X} item={:X}:{:X} count={} dropId={:X} known={} dropper={:X} claims={}", cPlayerId, message.ServerId,
                message.Item.BaseId.ModId, message.Item.BaseId.BaseId, message.Item.Count, dropId, outcome.Type == DropClaims::Result::kClaimed, outcome.DropperPlayerId,
                m_dropClaims.ClaimedCount());
        }
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    inventoryComponent.Content.AddOrRemoveEntry(message.Item);

    if (!message.UpdateClients && !isRemoteNpcInteraction)
    {
        if (dropId != 0)
            spdlog::warn("[DropSync] tracked change not relayed, the client asked not to update clients: player={:X} dropId={:X}", cPlayerId, dropId);
        return;
    }

    NotifyInventoryChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Item = message.Item;

    notify.Drop = bEnableItemDrops && !isRemoteNpcInteraction ? message.Drop : false;

    if (dropId != 0 && isDropAnnounce)
    {
        // Receivers only create a ground reference when the drop is relayed as such
        notify.DropId = notify.Drop ? dropId : 0;
        if (notify.Drop)
            m_dropClaims.Announce(dropId, cPlayerId, message.Item.BaseId.ModId, message.Item.BaseId.BaseId, message.Item.Count);

        // Receivers create their copy where the dropping client saw the item appear
        notify.HasDropPosition = notify.Drop && message.HasDropPosition;
        if (notify.HasDropPosition)
            notify.DropPosition = message.DropPosition;

        spdlog::info(
            "[DropSync] drop: player={:X} actor={:X} item={:X}:{:X} count={} dropId={:X} relayedDrop={} dropsEnabled={} announced={} position={}", cPlayerId, message.ServerId,
            message.Item.BaseId.ModId, message.Item.BaseId.BaseId, message.Item.Count, dropId, notify.Drop, static_cast<bool>(bEnableItemDrops), m_dropClaims.AnnouncedCount(),
            notify.HasDropPosition ? fmt::format("({:.0f},{:.0f},{:.0f})", message.DropPosition.x, message.DropPosition.y, message.DropPosition.z) : std::string("none"));
    }
    else if (dropId != 0 && isPickupClaim)
        notify.DropId = dropId;

    // A tracked pickup goes to everybody: a player that saw the drop and then walked away still has a copy to remove
    if (notify.DropId != 0 && isPickupClaim)
    {
        GameServer::Get()->SendToPlayers(notify, acMessage.GetSender());
        return;
    }

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed (dropId={:X})", __FUNCTION__, notify.DropId);
}

void InventoryService::OnEquipmentChanges(const PacketEvent<RequestEquipmentChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(message.ServerId));

    if (it == view.end())
        return;

    const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*it);
    if (pOwnerComponent)
    {
        if (pOwnerComponent->GetOwner() != acMessage.pPlayer || pOwnerComponent->OwnershipEpoch != message.OwnershipEpoch)
        {
            const uint32_t ownerId = pOwnerComponent->GetOwner() ? pOwnerComponent->GetOwner()->GetId() : 0;
            spdlog::debug(
                "Rejected equipment change from player {:X} for actor {:X}; current owner is {:X} and requested epoch {} does not match {}",
                acMessage.pPlayer->GetId(), message.ServerId, ownerId, message.OwnershipEpoch, pOwnerComponent->OwnershipEpoch);
            return;
        }
    }
    else if (message.OwnershipEpoch != 0)
    {
        spdlog::warn(
            "Rejected equipment change from player {:X} because object {:X} unexpectedly carried ownership epoch {}",
            acMessage.pPlayer->GetId(), message.ServerId, message.OwnershipEpoch);
        return;
    }

    auto& inventoryComponent = view.get<InventoryComponent>(*it);
    inventoryComponent.Content.UpdateEquipment(message.CurrentInventory);

    NotifyEquipmentChanges notify;
    notify.ServerId = message.ServerId;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.ItemId = message.ItemId;
    notify.EquipSlotId = message.EquipSlotId;
    notify.Count = message.Count;
    notify.Unequip = message.Unequip;
    notify.IsSpell = message.IsSpell;
    notify.IsShout = message.IsShout;

    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnWeaponDrawnRequest(const PacketEvent<DrawWeaponRequest>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it != std::end(characterView) && characterView.get<OwnerComponent>(*it).GetOwner() == acMessage.pPlayer)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);
        characterComponent.SetWeaponDrawn(message.IsWeaponDrawn);
        spdlog::debug("Updating weapon drawn state {:x}:{}", message.Id, message.IsWeaponDrawn);
    }
}
