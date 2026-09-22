#pragma once

#include "Message.h"
#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

struct RequestInventoryChanges final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestInventoryChanges;

    RequestInventoryChanges()
        : ClientMessage(Opcode)
    {
    }

    virtual ~RequestInventoryChanges() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestInventoryChanges& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Item == acRhs.Item && Drop == acRhs.Drop && UpdateClients == acRhs.UpdateClients && DropId == acRhs.DropId &&
               HasDropPosition == acRhs.HasDropPosition && (!HasDropPosition || DropPosition == acRhs.DropPosition);
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    Inventory::Entry Item{};
    bool Drop = false;
    bool UpdateClients = true;
    // Identity of a ground reference (see DropSync.h): the one created by this drop (Drop, Count < 0)
    // or the one consumed by this pickup (!Drop, Count > 0). 0 when the change is not about a tracked drop.
    uint64_t DropId{};
    // Where the game created the reference of this drop on the dropping client, so the others create theirs there too
    bool HasDropPosition = false;
    Vector3_NetQuantize DropPosition{};
};
