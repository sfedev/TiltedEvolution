#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <cmath>
#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <catch2/catch.hpp>

#include <DropSync.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>

#include "../server/Services/DropClaims.h"
#include "../client/Systems/DroppedItemTracker.h"

using namespace TiltedPhoques;

namespace
{
template <class T> T RoundTrip(const T& acMessage)
{
    Buffer buff(2000);

    Buffer::Writer writer(&buff);
    acMessage.Serialize(writer);

    Buffer::Reader reader(&buff);

    uint64_t trash;
    reader.ReadBits(trash, 8); // pop opcode

    T received;
    received.DeserializeRaw(reader);
    return received;
}
} // namespace

TEST_CASE("Drop ids", "[dropsync.id]")
{
    constexpr uint64_t cDropId = DropSync::MakeDropId(7, 3);

    REQUIRE(cDropId == 0x0000000700000003ull);
    REQUIRE(DropSync::GetPlayerId(cDropId) == 7);
    REQUIRE(DropSync::GetSeq(cDropId) == 3);
    REQUIRE(DropSync::MakeDropId(0xFFFFFFFF, 0xFFFFFFFF) == 0xFFFFFFFFFFFFFFFFull);
}

TEST_CASE("Drop sync packets", "[dropsync.packets]")
{
    SECTION("RequestInventoryChanges carries the drop id of a drop")
    {
        RequestInventoryChanges sendMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.OwnershipEpoch = 5;
        sendMessage.Item.BaseId = GameId(0, 0x139AE);
        sendMessage.Item.Count = -1;
        sendMessage.Drop = true;
        sendMessage.DropId = DropSync::MakeDropId(7, 3);

        const auto recvMessage = RoundTrip(sendMessage);
        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.DropId == 0x0000000700000003ull);
        REQUIRE(recvMessage.Item.Count == -1);
    }

    SECTION("RequestInventoryChanges without a drop id is unchanged")
    {
        RequestInventoryChanges sendMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.Item.BaseId = GameId(0, 0xF);
        sendMessage.Item.Count = 100;
        sendMessage.UpdateClients = false;

        const auto recvMessage = RoundTrip(sendMessage);
        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.DropId == 0);
        REQUIRE_FALSE(recvMessage.UpdateClients);
    }

    SECTION("NotifyInventoryChanges carries the drop id of a pickup")
    {
        NotifyInventoryChanges sendMessage;
        sendMessage.ServerId = 0x4321;
        sendMessage.OwnershipEpoch = 2;
        sendMessage.Item.BaseId = GameId(0, 0x139AE);
        sendMessage.Item.Count = 1;
        sendMessage.Drop = false;
        sendMessage.DropId = DropSync::MakeDropId(0x80000001, 0xFFFFFFFE);

        const auto recvMessage = RoundTrip(sendMessage);
        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.DropId == 0x80000001FFFFFFFEull);
    }

    SECTION("A drop carries where the dropping client created the reference")
    {
        // B's first drop of the 18/09 ls4 session
        RequestInventoryChanges sendMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.OwnershipEpoch = 1;
        sendMessage.Item.BaseId = GameId(0, 0x139AE);
        sendMessage.Item.Count = -1;
        sendMessage.Drop = true;
        sendMessage.DropId = DropSync::MakeDropId(1, 1);
        sendMessage.HasDropPosition = true;
        sendMessage.DropPosition = glm::vec3(634.2f, -429.9f, 121.5f);

        const auto recvMessage = RoundTrip(sendMessage);
        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.HasDropPosition);
        // The network vector keeps whole units, which is plenty for an item about to fall
        REQUIRE(recvMessage.DropPosition.x == 634.0f);
        REQUIRE(recvMessage.DropPosition.y == -429.0f);
        REQUIRE(recvMessage.DropPosition.z == 121.0f);
        REQUIRE(recvMessage.DropId == DropSync::MakeDropId(1, 1));
    }

    SECTION("A drop without a position stays without one")
    {
        RequestInventoryChanges sendMessage;
        sendMessage.ServerId = 0x1234;
        sendMessage.Item.BaseId = GameId(0, 0x139AE);
        sendMessage.Item.Count = -1;
        sendMessage.Drop = true;
        sendMessage.DropId = DropSync::MakeDropId(1, 2);

        const auto recvMessage = RoundTrip(sendMessage);
        REQUIRE(sendMessage == recvMessage);
        REQUIRE_FALSE(recvMessage.HasDropPosition);
        REQUIRE(recvMessage.DropPosition.x == 0.0f);
        REQUIRE(recvMessage.DropPosition.y == 0.0f);
        REQUIRE(recvMessage.DropPosition.z == 0.0f);
    }

    SECTION("The relayed drop carries the position, including negative coordinates")
    {
        NotifyInventoryChanges sendMessage;
        sendMessage.ServerId = 0x4321;
        sendMessage.OwnershipEpoch = 1;
        sendMessage.Item.BaseId = GameId(0, 0x3AEB9);
        sendMessage.Item.Count = -1;
        sendMessage.Drop = true;
        sendMessage.DropId = DropSync::MakeDropId(2, 3);
        sendMessage.HasDropPosition = true;
        sendMessage.DropPosition = glm::vec3(-112.8f, 94.7f, -3.0f);

        const auto recvMessage = RoundTrip(sendMessage);
        REQUIRE(sendMessage == recvMessage);
        REQUIRE(recvMessage.HasDropPosition);
        REQUIRE(recvMessage.DropPosition.x == -112.0f);
        REQUIRE(recvMessage.DropPosition.y == 94.0f);
        REQUIRE(recvMessage.DropPosition.z == -3.0f);
    }

    SECTION("Messages that differ only in the drop position are different")
    {
        NotifyInventoryChanges first;
        first.Drop = true;
        first.HasDropPosition = true;
        first.DropPosition = glm::vec3(10.0f, 20.0f, 30.0f);

        NotifyInventoryChanges second = first;
        second.DropPosition = glm::vec3(10.0f, 20.0f, 31.0f);
        REQUIRE_FALSE(first == second);

        second.HasDropPosition = false;
        REQUIRE_FALSE(first == second);
    }

    SECTION("NotifyDropPickupRejected through the factory")
    {
        NotifyDropPickupRejected sendMessage;
        sendMessage.DropId = DropSync::MakeDropId(9, 41);
        sendMessage.ServerId = 0x77;
        sendMessage.OwnershipEpoch = 3;
        sendMessage.Item.BaseId = GameId(0, 0x9F25C);
        sendMessage.Item.Count = 1;
        sendMessage.ClaimedByPlayerId = 4;

        Buffer buff(2000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());

        auto pReceived = CastUnique<NotifyDropPickupRejected>(std::move(pMessage));
        REQUIRE(*pReceived == sendMessage);
    }
}

TEST_CASE("Drop claims", "[dropsync.claims]")
{
    SECTION("First claim of an announced drop wins")
    {
        DropClaims claims;
        const uint64_t cDropId = DropSync::MakeDropId(1, 1);
        claims.Announce(cDropId, 1, 0, 0x139AE, -1);
        REQUIRE(claims.IsAnnounced(cDropId));

        const auto first = claims.TryClaim(cDropId, 2, 0, 0x139AE);
        REQUIRE(first.Type == DropClaims::Result::kClaimed);
        REQUIRE(first.ClaimedByPlayerId == 2);
        REQUIRE(first.DropperPlayerId == 1);
        REQUIRE_FALSE(claims.IsAnnounced(cDropId));

        const auto second = claims.TryClaim(cDropId, 1, 0, 0x139AE);
        REQUIRE(second.Type == DropClaims::Result::kAlreadyClaimed);
        REQUIRE(second.ClaimedByPlayerId == 2);
    }

    SECTION("An unknown id is still claimed only once")
    {
        DropClaims claims;
        const uint64_t cDropId = DropSync::MakeDropId(5, 9);

        REQUIRE(claims.TryClaim(cDropId, 5, 0, 0xF).Type == DropClaims::Result::kClaimedUnknown);
        REQUIRE(claims.TryClaim(cDropId, 6, 0, 0xF).Type == DropClaims::Result::kAlreadyClaimed);
    }

    SECTION("A claim for a different item consumes nothing")
    {
        DropClaims claims;
        const uint64_t cDropId = DropSync::MakeDropId(1, 2);
        claims.Announce(cDropId, 1, 0, 0x139AE, -1);

        REQUIRE(claims.TryClaim(cDropId, 2, 0, 0x9F25C).Type == DropClaims::Result::kBaseMismatch);
        REQUIRE(claims.IsAnnounced(cDropId));
        REQUIRE(claims.TryClaim(cDropId, 2, 0, 0x139AE).Type == DropClaims::Result::kClaimed);
    }

    SECTION("Both sets are bounded, oldest first")
    {
        DropClaims claims;
        for (uint32_t i = 1; i <= DropClaims::kMaxEntries + 1; ++i)
            claims.Announce(DropSync::MakeDropId(1, i), 1, 0, 0x139AE, -1);

        REQUIRE(claims.AnnouncedCount() == DropClaims::kMaxEntries);
        REQUIRE_FALSE(claims.IsAnnounced(DropSync::MakeDropId(1, 1)));
        REQUIRE(claims.IsAnnounced(DropSync::MakeDropId(1, 2)));

        for (uint32_t i = 1; i <= DropClaims::kMaxEntries + 1; ++i)
            (void)claims.TryClaim(DropSync::MakeDropId(2, i), 2, 0, 0xF);

        REQUIRE(claims.ClaimedCount() == DropClaims::kMaxEntries);
        // The oldest claim was forgotten, so that id could be claimed again: a dupe, never a lost item
        REQUIRE(claims.TryClaim(DropSync::MakeDropId(2, 1), 3, 0, 0xF).Type == DropClaims::Result::kClaimedUnknown);
    }
}

TEST_CASE("Dropped item tracker", "[dropsync.tracker]")
{
    auto makeBinding = [](uint64_t aDropId, uint32_t aHandle, uint32_t aRef, uint32_t aBase, bool aLocal)
    {
        DroppedItemTracker::Binding binding{};
        binding.DropId = aDropId;
        binding.Handle = aHandle;
        binding.RefFormId = aRef;
        binding.BaseFormId = aBase;
        binding.Count = 1;
        binding.Local = aLocal;
        return binding;
    };

    SECTION("Bind, find and remove")
    {
        DroppedItemTracker tracker;
        REQUIRE_FALSE(tracker.Bind(makeBinding(10, 0x100, 0xFF000001, 0x139AE, true)));
        REQUIRE(tracker.Size() == 1);

        const auto* pBinding = tracker.Find(10);
        REQUIRE(pBinding);
        REQUIRE(pBinding->RefFormId == 0xFF000001);
        REQUIRE_FALSE(tracker.Find(11));

        REQUIRE(tracker.Remove(10));
        REQUIRE_FALSE(tracker.Remove(10));
        REQUIRE(tracker.Size() == 0);
    }

    SECTION("A picked up reference is matched by handle, then by form id and base")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100, 0xFF000001, 0x139AE, true));
        tracker.Bind(makeBinding(11, 0x200, 0xFF000002, 0x9F25C, false));

        bool byHandle = false;
        auto taken = tracker.TakeByRef(0x200, 0xFF000002, 0x9F25C, &byHandle);
        REQUIRE(taken);
        REQUIRE(taken->DropId == 11);
        REQUIRE(byHandle);
        REQUIRE_FALSE(tracker.Find(11));

        // Handle changed (e.g. the cell was reloaded) but form id and base still agree
        taken = tracker.TakeByRef(0x999, 0xFF000001, 0x139AE, &byHandle);
        REQUIRE(taken);
        REQUIRE(taken->DropId == 10);
        REQUIRE_FALSE(byHandle);
        REQUIRE(tracker.Size() == 0);
    }

    SECTION("A recycled form id with another base is not a match")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100, 0xFF000001, 0x139AE, true));

        REQUIRE_FALSE(tracker.TakeByRef(0x300, 0xFF000001, 0x9F25C));
        REQUIRE_FALSE(tracker.TakeByRef(0x100, 0xFF000005, 0x139AE));
        REQUIRE(tracker.Size() == 1);
    }

    SECTION("Dropping the same original reference again retires its previous drop and pending removal")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100564, 0xA0D77, 0x3AEB9, false));
        REQUIRE(tracker.Park(10, 2));

        tracker.Bind(makeBinding(11, 0x100564, 0xA0D77, 0x3AEB9, true));
        REQUIRE(tracker.Size() == 1);
        REQUIRE_FALSE(tracker.Find(10));
        REQUIRE(tracker.Parked().empty());
        // A late pickup for drop 10 cannot resolve the reference now used by drop 11.
        REQUIRE_FALSE(tracker.Remove(10));
        REQUIRE(tracker.Find(11));

        tracker.Bind(makeBinding(12, 0x100564, 0xA0D77, 0x3AEB9, true));
        REQUIRE(tracker.Size() == 1);
        REQUIRE_FALSE(tracker.Find(11));
        const auto pickedUp = tracker.TakeByRef(0x100564, 0xA0D77, 0x3AEB9);
        REQUIRE(pickedUp);
        REQUIRE(pickedUp->DropId == 12);
        REQUIRE(tracker.Size() == 0);
    }

    SECTION("Rebinding a recycled form id also retires an old handle and base")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100, 0xFF000001, 0x139AE, true));
        tracker.Park(10, 2);
        tracker.Bind(makeBinding(11, 0x200, 0xFF000001, 0x9F25C, false));
        REQUIRE(tracker.Size() == 1);
        REQUIRE_FALSE(tracker.Find(10));
        REQUIRE(tracker.Parked().empty());
        REQUIRE_FALSE(tracker.TakeByRef(0x100, 0xFF000001, 0x139AE));
        REQUIRE(tracker.Find(11));
        const auto pickedUp = tracker.TakeByRef(0x200, 0xFF000001, 0x9F25C);
        REQUIRE(pickedUp);
        REQUIRE(pickedUp->DropId == 11);
    }

    SECTION("Distinct references of the same item keep independent drops")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100, 0xFF000001, 0x3AEB9, true));
        tracker.Bind(makeBinding(11, 0x200, 0xFF000002, 0x3AEB9, false));
        REQUIRE(tracker.Size() == 2);
        REQUIRE(tracker.Find(10));
        REQUIRE(tracker.Find(11));
        const auto pickedUp = tracker.TakeByRef(0x100, 0xFF000001, 0x3AEB9);
        REQUIRE(pickedUp);
        REQUIRE(pickedUp->DropId == 10);
        REQUIRE(tracker.Find(11));
    }

    SECTION("Matching a handle and reference still requires the expected base")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100, 0xFF000001, 0x139AE, true));
        REQUIRE_FALSE(tracker.TakeByRef(0x100, 0xFF000001, 0x9F25C));
        REQUIRE(tracker.Find(10));
    }

    SECTION("The oldest binding is evicted at the cap")
    {
        DroppedItemTracker tracker;
        for (uint32_t i = 1; i <= DroppedItemTracker::kMaxBindings; ++i)
            REQUIRE_FALSE(tracker.Bind(makeBinding(i, i, 0xFF000000 + i, 0xF, false)));

        const auto evicted = tracker.Bind(makeBinding(9999, 9999, 0xFF00FFFF, 0xF, false));
        REQUIRE(evicted);
        REQUIRE(evicted->DropId == 1);
        REQUIRE(tracker.Size() == DroppedItemTracker::kMaxBindings);
        REQUIRE(tracker.Find(9999));
    }

    SECTION("Activation routing recognizes original and temporary drops without consuming them")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100564, 0xA0D77, 0x3AEB9, false));
        tracker.Bind(makeBinding(11, 0x200, 0xFF000001, 0x139AE, true));
        REQUIRE(tracker.ContainsReference(0xA0D77, 0x3AEB9));
        REQUIRE(tracker.ContainsReference(0xFF000001, 0x139AE));
        REQUIRE_FALSE(tracker.ContainsReference(0xA0D77, 0x139AE));
        REQUIRE_FALSE(tracker.ContainsReference(0xA0D78, 0x3AEB9));
        REQUIRE_FALSE(tracker.ContainsReference(0, 0));
        REQUIRE(tracker.Size() == 2);
        REQUIRE(tracker.TakeByRef(0x100564, 0xA0D77, 0x3AEB9));
        REQUIRE_FALSE(tracker.ContainsReference(0xA0D77, 0x3AEB9));
        REQUIRE(tracker.ContainsReference(0xFF000001, 0x139AE));
    }

    SECTION("A deferred world removal preserves its notification identity until reference reuse")
    {
        DroppedItemTracker tracker;
        tracker.Bind(makeBinding(10, 0x100564, 0xA0D77, 0x3AEB9, false));
        REQUIRE(tracker.Park(10, 0x49, 7));
        REQUIRE(tracker.Parked().size() == 1);
        REQUIRE(tracker.Parked()[0].PickerServerId == 0x49);
        REQUIRE(tracker.Parked()[0].PickerOwnershipEpoch == 7);
        REQUIRE(tracker.Park(10, 0x49, 7));
        REQUIRE(tracker.Parked().size() == 1);
        tracker.Bind(makeBinding(11, 0x100564, 0xA0D77, 0x3AEB9, true));
        REQUIRE(tracker.Parked().empty());
        REQUIRE_FALSE(tracker.Find(10));
        REQUIRE(tracker.Find(11));
    }

    SECTION("Local claims and parked removals")
    {
        DroppedItemTracker tracker;
        tracker.MarkClaimedLocally(42);
        REQUIRE(tracker.WasClaimedLocally(42));
        REQUIRE_FALSE(tracker.WasClaimedLocally(43));

        for (uint64_t i = 0; i < DroppedItemTracker::kMaxLocalClaims; ++i)
            tracker.MarkClaimedLocally(1000 + i);
        REQUIRE_FALSE(tracker.WasClaimedLocally(42));

        REQUIRE_FALSE(tracker.Park(7, 0x55));
        tracker.Bind(makeBinding(7, 7, 0xFF000007, 0xF, false));
        REQUIRE(tracker.Park(7, 0x55));
        REQUIRE(tracker.Park(7, 0x55));
        REQUIRE(tracker.Parked().size() == 1);
        tracker.Unpark(7);
        REQUIRE(tracker.Parked().empty());

        for (uint32_t i = 0; i < DroppedItemTracker::kMaxParked; ++i)
        {
            tracker.Bind(makeBinding(100 + i, 100 + i, 0xFF000100 + i, 0xF, false));
            REQUIRE(tracker.Park(100 + i, 1));
        }
        REQUIRE(tracker.Parked().size() == DroppedItemTracker::kMaxParked);
        REQUIRE_FALSE(tracker.Park(5000, 1));

        // A new binding retires the oldest operation, guaranteeing room for its own removal.
        tracker.Bind(makeBinding(5000, 5000, 0xFF005000, 0xF, false));
        REQUIRE_FALSE(tracker.Find(100));
        REQUIRE(tracker.Park(5000, 1));
        REQUIRE(tracker.Parked().size() == DroppedItemTracker::kMaxParked);
        REQUIRE(tracker.TakeByRef(5000, 0xFF005000, 0xF));
        REQUIRE(tracker.Parked().size() == DroppedItemTracker::kMaxParked - 1);
        REQUIRE(tracker.Remove(101));
        REQUIRE(tracker.Parked().size() == DroppedItemTracker::kMaxParked - 2);

        tracker.Bind(makeBinding(10, 0x100, 0xFF000001, 0x139AE, true));
        tracker.Clear();
        REQUIRE(tracker.Size() == 0);
        REQUIRE(tracker.Parked().empty());
        REQUIRE_FALSE(tracker.WasClaimedLocally(1000));
    }
}

TEST_CASE("Drop placement", "[dropsync.placement]")
{
    SECTION("A healthy eye origin sits right above the player")
    {
        // B, 18/09 11:51:41: eye origin and position of the player in the same drop
        REQUIRE(DropSync::IsEyeOriginUsable(glm::vec3(732.0f, -501.4f, 121.5f), glm::vec3(732.0f, -501.4f, 1.0f)));
        // A mounted player or a larger race moves the eye a little, never hundreds of units
        REQUIRE(DropSync::IsEyeOriginUsable(glm::vec3(832.0f, -501.4f, 250.0f), glm::vec3(732.0f, -501.4f, 1.0f)));
    }

    SECTION("The eye origin of the broken camera node is rejected")
    {
        // B, 17/09 17:21:32: the first person camera node reported its local offset, at the cell origin
        REQUIRE_FALSE(DropSync::IsEyeOriginUsable(glm::vec3(0.0f, 0.0f, 120.5f), glm::vec3(537.6f, -497.0f, 1.0f)));
        REQUIRE_FALSE(DropSync::IsEyeOriginUsable(glm::vec3(std::nanf(""), 0.0f, 120.5f), glm::vec3(537.6f, -497.0f, 1.0f)));
        REQUIRE_FALSE(DropSync::IsEyeOriginUsable(glm::vec3(INFINITY, 0.0f, 120.5f), glm::vec3(537.6f, -497.0f, 1.0f)));
        // The same broken origin under a player standing high above or deep below the cell origin
        REQUIRE_FALSE(DropSync::IsEyeOriginUsable(glm::vec3(0.0f, 0.0f, 120.5f), glm::vec3(150.0f, -100.0f, 1800.0f)));
        REQUIRE_FALSE(DropSync::IsEyeOriginUsable(glm::vec3(0.0f, 0.0f, 120.5f), glm::vec3(150.0f, -100.0f, -1500.0f)));
    }

    SECTION("The fallback lands in front of the actor, the way the game drops for any other actor")
    {
        // B faced west (heading 4.7154, eye direction -0.9967, 0.0030): 50 units towards -X, 30 up
        const glm::vec3 cFacingWest = DropSync::ActorDropLocation(glm::vec3(732.0f, -501.4f, 1.0f), 4.7154f);
        REQUIRE(cFacingWest.x == Approx(682.0f).margin(0.1f));
        REQUIRE(cFacingWest.y == Approx(-501.4f).margin(0.5f));
        REQUIRE(cFacingWest.z == Approx(31.0f));

        // A faced east (heading 1.6980, eye direction 0.9741, -0.1246)
        const glm::vec3 cFacingEast = DropSync::ActorDropLocation(glm::vec3(384.8f, -487.1f, 0.3f), 1.6980f);
        REQUIRE(cFacingEast.x == Approx(384.8f + 49.6f).margin(0.2f));
        REQUIRE(cFacingEast.y == Approx(-487.1f - 6.4f).margin(0.2f));
        REQUIRE(cFacingEast.z == Approx(30.3f));
    }

    SECTION("A reported drop position is used only near the remote actor")
    {
        // B's first drop, as its copy is created in A next to B's character
        REQUIRE(DropSync::IsReplicatedDropPositionUsable(glm::vec3(634.0f, -429.0f, 121.0f), glm::vec3(732.0f, -499.0f, 1.0f)));
        // Right at the limit and just past it
        REQUIRE(DropSync::IsReplicatedDropPositionUsable(glm::vec3(1024.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f)));
        REQUIRE_FALSE(DropSync::IsReplicatedDropPositionUsable(glm::vec3(1025.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f)));
        // A position from another place of the world, or a corrupt one
        REQUIRE_FALSE(DropSync::IsReplicatedDropPositionUsable(glm::vec3(-102.0f, -64.0f, 120.0f), glm::vec3(5000.0f, 5000.0f, 0.0f)));
        REQUIRE_FALSE(DropSync::IsReplicatedDropPositionUsable(glm::vec3(std::nanf(""), 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f)));
    }
}
