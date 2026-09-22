#include <Messages/NotifyDropPickupRejected.h>
#include <TiltedCore/Serialization.hpp>

void NotifyDropPickupRejected::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, DropId);
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Item.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, ClaimedByPlayerId);
}

void NotifyDropPickupRejected::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    DropId = Serialization::ReadVarInt(aReader);
    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
    ClaimedByPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
