#pragma once

#include "Message.h"

#include <Structs/Inventory.h>
#include <Structs/Vector3_NetQuantize.h>

struct NotifyInventoryChanges final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyInventoryChanges;

    NotifyInventoryChanges()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyInventoryChanges& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ServerId == acRhs.ServerId && OwnershipEpoch == acRhs.OwnershipEpoch && Item == acRhs.Item && Drop == acRhs.Drop && DropId == acRhs.DropId &&
               HasDropPosition == acRhs.HasDropPosition && (!HasDropPosition || DropPosition == acRhs.DropPosition);
    }

    uint32_t ServerId{};
    uint32_t OwnershipEpoch{};
    Inventory::Entry Item{};
    bool Drop = false;
    // Same meaning as RequestInventoryChanges::DropId
    uint64_t DropId{};
    // Same meaning as RequestInventoryChanges::DropPosition
    bool HasDropPosition = false;
    Vector3_NetQuantize DropPosition{};
};
