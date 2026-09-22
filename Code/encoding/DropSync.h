#pragma once

#include <cmath>
#include <cstdint>

#include <glm/vec3.hpp>

// Dropped item sync.
//
// An item dropped on the ground becomes a temporary reference (form id 0xFF......) that every client creates on
// its own, so the form id means nothing to the other clients. The dropping client gives the drop an id instead,
// made of its server player id (never reused during a server run) and a per-process sequence number. Each client
// binds that id to its own local reference, and a pickup names the id so the others can remove their copy.
//
// TP_DROPSYNC_DIAG enables the verbose [DropSync] diagnostics (hook traces and the dropped reference tracker).
#ifndef TP_DROPSYNC_DIAG
#define TP_DROPSYNC_DIAG 1
#endif

// TP_DROPSYNC_DIAG_BASE adds the traces of the base the game places a drop on: the transform of the actor node,
// the first person camera node and the eye vector. They are the only ones that read offsets the repository does
// not map yet, so they can be turned off on their own without losing the rest of the diagnostics.
#ifndef TP_DROPSYNC_DIAG_BASE
#define TP_DROPSYNC_DIAG_BASE TP_DROPSYNC_DIAG
#endif

namespace DropSync
{
constexpr uint64_t MakeDropId(uint32_t aPlayerId, uint32_t aSeq) noexcept
{
    return (static_cast<uint64_t>(aPlayerId) << 32) | aSeq;
}

constexpr uint32_t GetPlayerId(uint64_t aDropId) noexcept
{
    return static_cast<uint32_t>(aDropId >> 32);
}

constexpr uint32_t GetSeq(uint64_t aDropId) noexcept
{
    return static_cast<uint32_t>(aDropId & 0xFFFFFFFF);
}

// Placement of a dropped item.
//
// Without an explicit location the game places a drop of the player in front of its eye vector, whose origin comes
// from the first person camera node, and a drop of any other actor 50 units in front of it and 30 up. The eye origin
// of a healthy player sits right above its feet; when that camera node loses its parent transform the origin is
// reported at the cell origin instead, and the item is created hundreds of units away, out of sight.

// Largest gaps accepted between the eye origin of the player and its position. A healthy eye is 120.5 units above
// the feet and right over them; the vertical margin leaves room for scaled races and mounts. Both bounds matter: the
// broken origin is always (0, 0, 120.5), which can be close horizontally to a player standing far above or below.
constexpr float kMaxEyeOffsetXY = 256.0f;
constexpr float kMaxEyeOffsetZ = 512.0f;

// Largest distance accepted between the drop position a remote client reports and its actor here. A drop lands
// within a couple hundred units of whoever dropped it; the margin covers the lag of the remote actor.
constexpr float kMaxReplicatedDropDistance = 1024.0f;

inline bool IsFinite(const glm::vec3& acPoint) noexcept
{
    return std::isfinite(acPoint.x) && std::isfinite(acPoint.y) && std::isfinite(acPoint.z);
}

inline bool IsEyeOriginUsable(const glm::vec3& acEyeOrigin, const glm::vec3& acActorPosition) noexcept
{
    if (!IsFinite(acEyeOrigin))
        return false;

    const float cDeltaX = acEyeOrigin.x - acActorPosition.x;
    const float cDeltaY = acEyeOrigin.y - acActorPosition.y;
    const float cDeltaZ = acEyeOrigin.z - acActorPosition.z;
    return cDeltaX * cDeltaX + cDeltaY * cDeltaY <= kMaxEyeOffsetXY * kMaxEyeOffsetXY && std::fabs(cDeltaZ) <= kMaxEyeOffsetZ;
}

// Where the game drops an item for an actor that is not the player, flattened to its heading
inline glm::vec3 ActorDropLocation(const glm::vec3& acActorPosition, float aHeading) noexcept
{
    return glm::vec3(acActorPosition.x + std::sin(aHeading) * 50.0f, acActorPosition.y + std::cos(aHeading) * 50.0f, acActorPosition.z + 30.0f);
}

inline bool IsReplicatedDropPositionUsable(const glm::vec3& acDropPosition, const glm::vec3& acActorPosition) noexcept
{
    if (!IsFinite(acDropPosition) || !IsFinite(acActorPosition))
        return false;

    const glm::vec3 cDelta = acDropPosition - acActorPosition;
    return cDelta.x * cDelta.x + cDelta.y * cDelta.y + cDelta.z * cDelta.z <= kMaxReplicatedDropDistance * kMaxReplicatedDropDistance;
}
} // namespace DropSync
