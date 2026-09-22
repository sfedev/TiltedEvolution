#pragma once

#include "Message.h"

#include <Structs/Inventory.h>

// Sent only to the player whose pickup of a dropped item lost the race: somebody else claimed the same drop id
// first, so the item this player just added to its inventory has to be given back.
struct NotifyDropPickupRejected final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDropPickupRejected;

    NotifyDropPickupRejected()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDropPickupRejected& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && DropId == acRhs.DropId && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Item == acRhs.Item &&
               ClaimedByPlayerId == acRhs.ClaimedByPlayerId;
    }

    uint64_t DropId{};
    // The actor that picked the item up, as sent in the rejected request
    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    // The item as claimed, Count > 0
    Inventory::Entry Item{};
    uint32_t ClaimedByPlayerId{};
};
