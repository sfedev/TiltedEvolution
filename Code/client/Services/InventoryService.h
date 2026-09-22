#pragma once

#include <Systems/DroppedItemTracker.h>
#include <Messages/RequestInventoryChanges.h>

#include <chrono>

struct World;
struct TransportService;
struct TESObjectREFR;

struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyObjectInventoryChanges;
struct NotifyInventoryChanges;
struct NotifyDropPickupRejected;
struct InventoryChangeEvent;
struct DroppedItemEvent;
struct EquipmentChangeEvent;
struct NotifyEquipmentChanges;

/**
 * @brief Manages inventories of actors and containers.
 *
 * The initial contents of actor inventories are synced through spawn messages through CharacterService
 * @see CharacterService
 * The initial contents of container inventories are synced through the ObjectService
 * @see ObjectService
 * The InventoryService manages any changes to both the current equipment of actors,
 * and the contents of both actor and object inventories.
 *
 * It also keeps track of the items dropped on the ground while connected (see DropSync.h), so that a dropped item
 * picked up by one player disappears for the others instead of being picked up again.
 */
struct InventoryService
{
    InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~InventoryService() noexcept = default;

    TP_NOCOPYMOVE(InventoryService);

    /**
     * @brief Check weapon state.
     * @see RunWeaponStateUpdates
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;
    /**
     * @brief Sends out inventory changes made by the local client.
     */
    void OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept;
    /**
     * @brief Binds the reference created by a local drop to the drop id that was sent to the server.
     */
    void OnDroppedItemEvent(const DroppedItemEvent& acEvent) noexcept;
    /**
     * @brief Sends out equipment changes made by the local client.
     */
    void OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept;

    /**
     * @brief Applies inventory changes sent by the server.
     */
    void OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept;
    /**
     * @brief Gives back an item picked up from the ground that another player had already picked up.
     */
    void OnDropPickupRejected(const NotifyDropPickupRejected& acMessage) noexcept;
    /**
     * @brief Applies equipment changes sent by the server.
     */
    void OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept;

    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;

    // Service-thread-only routing; hooks capture values and never inspect the tracker themselves.
    bool IsTrackedDrop(uint32_t aRefFormId, uint32_t aBaseFormId) const noexcept
    {
        return m_droppedItems.ContainsReference(aRefFormId, aBaseFormId);
    }

private:
    using TClock = std::chrono::steady_clock;

    // Diagnostics: where a freshly dropped reference is a moment later
    struct DropProbe
    {
        uint64_t DropId{};
        uint32_t DropSeq{};
        uint32_t Handle{};
        uint32_t RefFormId{};
        uint32_t BaseFormId{};
        bool Local{};
        float SpawnX{};
        float SpawnY{};
        float SpawnZ{};
        TClock::time_point CreatedAt{};
        uint8_t Stage{};
    };

    // A drop request waits for the reference the game creates, so it can tell the others where the item appeared
    struct PendingDrop
    {
        RequestInventoryChanges Request{};
        TClock::time_point QueuedAt{};
        bool Sent{};
    };

    // Checks that a reference removed by a remote pickup is really gone
    struct RemovalCheck
    {
        uint64_t DropId{};
        uint32_t Handle{};
        uint32_t RefFormId{};
        uint32_t BaseFormId{};
        TClock::time_point RemovedAt{};
        uint8_t Attempt{};
        bool NativePickup{};
    };

    /**
     * Checks whether local actors their weapon draw states have changed,
     * and if so, send the new states to the server.
     */
    void RunWeaponStateUpdates() noexcept;

    /**
     * Dropped item housekeeping: diagnostics probes, removal checks and parked removals.
     */
    void RunDroppedItemUpdates() noexcept;
    /**
     * Removes this client's copy of a dropped item that somebody else picked up.
     * @return false when the reference could not be resolved yet and the removal was parked
     */
    enum class PickupResult { Complete, Pending, InventoryHandled };
    PickupResult ApplyRemotePickup(uint64_t aDropId, uint32_t aPickerServerId, uint32_t aPickerOwnershipEpoch, bool aIsRetry) noexcept;
    void CancelRemovalChecks(uint32_t aRefFormId) noexcept;
    void SendHeldDrop(uint32_t aDropSeq, PendingDrop& aPending) noexcept;
    void BindReplicatedDrop(const NotifyInventoryChanges& acMessage, uint32_t aHandle, uint32_t aRemoteActorFormId, bool aAtDropperPosition) noexcept;
    TESObjectREFR* ResolveDroppedReference(uint32_t aHandle, uint32_t aRefFormId, uint32_t aBaseFormId, const char*& aprResolvedBy) const noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    DroppedItemTracker m_droppedItems;
    // Drop sequence number -> request of that drop, until the reference of that drop is known
    TiltedPhoques::Map<uint32_t, PendingDrop> m_announcedDrops;
    TiltedPhoques::Vector<DropProbe> m_dropProbes;
    TiltedPhoques::Vector<RemovalCheck> m_removalChecks;
    TClock::time_point m_nextParkedRetry{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_inventoryConnection;
    entt::scoped_connection m_droppedItemConnection;
    entt::scoped_connection m_equipmentConnection;
    entt::scoped_connection m_inventoryChangeConnection;
    entt::scoped_connection m_dropPickupRejectedConnection;
    entt::scoped_connection m_equipmentChangeConnection;
    entt::scoped_connection m_disconnectedConnection;
};
