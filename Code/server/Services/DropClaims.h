#pragma once

#include <TiltedCore/Stl.hpp>

#include <cstdint>
#include <deque>

/**
 * @brief Arbitrates who picked up a dropped item first (see DropSync.h for the id).
 *
 * The server never sees the ground references themselves; it only remembers the drop ids it relayed and the ids that
 * were already claimed by a pickup. Both sets are bounded, oldest first: forgetting an id only means falling back to
 * the behaviour without arbitration, never taking an item away from a player.
 */
class DropClaims
{
public:
    static constexpr size_t kMaxEntries = 4096;

    enum class Result
    {
        kClaimed,        // Announced drop, first claim
        kClaimedUnknown, // Id never announced here (evicted, or dropped while drop sync was off), first claim
        kAlreadyClaimed, // Somebody else got there first
        kBaseMismatch    // The id was announced for a different item; nothing is consumed
    };

    struct Outcome
    {
        Result Type{Result::kClaimedUnknown};
        uint32_t ClaimedByPlayerId{};
        uint32_t DropperPlayerId{};
    };

    void Announce(uint64_t aDropId, uint32_t aDropperPlayerId, uint32_t aBaseModId, uint32_t aBaseId, int32_t aCount) noexcept
    {
        if (m_announced.find(aDropId) == m_announced.end())
        {
            m_announcedOrder.push_back(aDropId);
            if (m_announcedOrder.size() > kMaxEntries)
            {
                m_announced.erase(m_announcedOrder.front());
                m_announcedOrder.pop_front();
            }
        }

        m_announced[aDropId] = Announced{aDropperPlayerId, aBaseModId, aBaseId, aCount};
    }

    [[nodiscard]] bool IsAnnounced(uint64_t aDropId) const noexcept { return m_announced.find(aDropId) != m_announced.end(); }

    [[nodiscard]] Outcome TryClaim(uint64_t aDropId, uint32_t aClaimerPlayerId, uint32_t aBaseModId, uint32_t aBaseId) noexcept
    {
        Outcome outcome{};

        if (const auto claimed = m_claimed.find(aDropId); claimed != m_claimed.end())
        {
            outcome.Type = Result::kAlreadyClaimed;
            outcome.ClaimedByPlayerId = claimed->second;
            return outcome;
        }

        if (const auto announced = m_announced.find(aDropId); announced != m_announced.end())
        {
            outcome.DropperPlayerId = announced->second.DropperPlayerId;

            if (announced->second.BaseModId != aBaseModId || announced->second.BaseId != aBaseId)
            {
                outcome.Type = Result::kBaseMismatch;
                return outcome;
            }

            // The entry stays in the eviction queue; erasing an id that is already gone is harmless
            m_announced.erase(announced);
            outcome.Type = Result::kClaimed;
        }
        else
            outcome.Type = Result::kClaimedUnknown;

        m_claimed[aDropId] = aClaimerPlayerId;
        m_claimedOrder.push_back(aDropId);
        if (m_claimedOrder.size() > kMaxEntries)
        {
            m_claimed.erase(m_claimedOrder.front());
            m_claimedOrder.pop_front();
        }

        outcome.ClaimedByPlayerId = aClaimerPlayerId;
        return outcome;
    }

    [[nodiscard]] size_t AnnouncedCount() const noexcept { return m_announced.size(); }
    [[nodiscard]] size_t ClaimedCount() const noexcept { return m_claimed.size(); }

private:
    struct Announced
    {
        uint32_t DropperPlayerId{};
        uint32_t BaseModId{};
        uint32_t BaseId{};
        int32_t Count{};
    };

    TiltedPhoques::Map<uint64_t, Announced> m_announced{};
    TiltedPhoques::Map<uint64_t, uint32_t> m_claimed{};
    std::deque<uint64_t> m_announcedOrder{};
    std::deque<uint64_t> m_claimedOrder{};
};
