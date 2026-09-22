#pragma once

/**
 * @brief Dispatched after the local game dropped an item on the ground, with the reference it created.
 *
 * The InventoryChangeEvent of the same drop is queued before the game creates the reference, so this event carries
 * the result and DropSeq ties the two together. Values only: the reference may already be gone when it is handled.
 */
struct DroppedItemEvent
{
    uint32_t DropSeq{};
    uint32_t HandleBits{};
    uint32_t RefFormId{};
    uint32_t BaseFormId{};
    uint32_t ActorFormId{};
    int32_t Count{};
    float PositionX{};
    float PositionY{};
    float PositionZ{};
    // Whether the inventory change of this drop was queued for the server at all
    bool Queued{};
};
