#pragma once

#include <TiltedCore/Stl.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>

/**
 * @brief Binds drop ids (see DropSync.h) to this client's own ground references.
 *
 * Engine free on purpose: it only stores handles and form ids, never reference pointers, and it is used from the
 * service thread only. Resolving and validating a reference is the caller's job.
 */
class DroppedItemTracker
{
public:
    static constexpr size_t kMaxBindings = 512;
    static constexpr size_t kMaxLocalClaims = 64;
    // At most one removal per live binding. Eviction/pickup removes its pending operation too, so a valid
    // tracked pickup always has room even when all currently tracked references are unloaded.
    static constexpr size_t kMaxParked = kMaxBindings;

    struct Binding
    {
        uint64_t DropId{};
        uint32_t Handle{};
        uint32_t RefFormId{};
        uint32_t BaseFormId{};
        int32_t Count{};
        // Created by this client's own drop (true) or replicated from another player's drop (false)
        bool Local{};
        uint64_t Order{};
    };

    struct ParkedRemoval
    {
        uint64_t DropId{};
        uint32_t PickerServerId{};
        uint32_t PickerOwnershipEpoch{};
    };

    // Returns the binding that had to be evicted to make room, if any
    std::optional<Binding> Bind(Binding aBinding) noexcept
    {
        // Skyrim can return an original reference again when an item is picked up and dropped. Temporary
        // form ids are recycled too. Once this form id represents a new drop, old ids must no longer resolve
        // it, even if its handle or base changed. Distinct references of the same base remain independent.
        if (aBinding.RefFormId != 0)
        {
            for (auto it = m_bindings.begin(); it != m_bindings.end();)
            {
                if (it->first != aBinding.DropId && it->second.RefFormId == aBinding.RefFormId)
                {
                    Unpark(it->first);
                    it = m_bindings.erase(it);
                }
                else
                    ++it;
            }
        }

        aBinding.Order = ++m_order;
        m_bindings[aBinding.DropId] = aBinding;

        if (m_bindings.size() <= kMaxBindings)
            return std::nullopt;

        auto oldest = m_bindings.begin();
        for (auto it = m_bindings.begin(); it != m_bindings.end(); ++it)
        {
            if (it->second.Order < oldest->second.Order)
                oldest = it;
        }

        Binding evicted = oldest->second;
        Unpark(evicted.DropId);
        m_bindings.erase(oldest);
        return evicted;
    }

    [[nodiscard]] const Binding* Find(uint64_t aDropId) const noexcept
    {
        const auto it = m_bindings.find(aDropId);
        return it != m_bindings.end() ? &it->second : nullptr;
    }

    bool Remove(uint64_t aDropId) noexcept
    {
        Unpark(aDropId);
        return m_bindings.erase(aDropId) > 0;
    }

    // Read-only lookup for activation routing, before the queued pickup consumes the binding.
    [[nodiscard]] bool ContainsReference(uint32_t aRefFormId, uint32_t aBaseFormId) const noexcept
    {
        if (aRefFormId == 0 || aBaseFormId == 0)
            return false;
        return std::any_of(m_bindings.begin(), m_bindings.end(), [=](const auto& acEntry)
        {
            return acEntry.second.RefFormId == aRefFormId && acEntry.second.BaseFormId == aBaseFormId;
        });
    }

    // Finds the binding of a reference that is being picked up and erases it. The handle together with the form id
    // identifies the reference; the form id together with the base form is the fallback when the handle changed.
    std::optional<Binding> TakeByRef(uint32_t aHandle, uint32_t aRefFormId, uint32_t aBaseFormId, bool* apMatchedByHandle = nullptr) noexcept
    {
        auto match = m_bindings.end();
        bool byHandle = false;

        for (auto it = m_bindings.begin(); it != m_bindings.end(); ++it)
        {
            const Binding& binding = it->second;
            if (aHandle != 0 && binding.Handle == aHandle && binding.RefFormId == aRefFormId && binding.BaseFormId == aBaseFormId)
            {
                match = it;
                byHandle = true;
                break;
            }

            if (match == m_bindings.end() && aRefFormId != 0 && binding.RefFormId == aRefFormId && binding.BaseFormId == aBaseFormId)
                match = it;
        }

        if (match == m_bindings.end())
            return std::nullopt;

        if (apMatchedByHandle)
            *apMatchedByHandle = byHandle;

        Binding taken = match->second;
        Unpark(taken.DropId);
        m_bindings.erase(match);
        return taken;
    }

    // Drop ids this client claimed itself, to tell "already picked up here" from "never seen here" later on
    void MarkClaimedLocally(uint64_t aDropId) noexcept
    {
        m_localClaims.push_back(aDropId);
        if (m_localClaims.size() > kMaxLocalClaims)
            m_localClaims.erase(m_localClaims.begin());
    }

    [[nodiscard]] bool WasClaimedLocally(uint64_t aDropId) const noexcept { return std::find(m_localClaims.begin(), m_localClaims.end(), aDropId) != m_localClaims.end(); }

    // A remote pickup whose local reference could not be resolved right now; retried later
    bool Park(uint64_t aDropId, uint32_t aPickerServerId, uint32_t aPickerOwnershipEpoch = 0) noexcept
    {
        if (!Find(aDropId))
            return false;
        for (const auto& parked : m_parked)
        {
            if (parked.DropId == aDropId)
                return true;
        }

        if (m_parked.size() >= kMaxParked)
            return false;

        m_parked.push_back(ParkedRemoval{aDropId, aPickerServerId, aPickerOwnershipEpoch});
        return true;
    }

    void Unpark(uint64_t aDropId) noexcept
    {
        m_parked.erase(std::remove_if(m_parked.begin(), m_parked.end(), [aDropId](const ParkedRemoval& aParked) { return aParked.DropId == aDropId; }), m_parked.end());
    }

    [[nodiscard]] const TiltedPhoques::Vector<ParkedRemoval>& Parked() const noexcept { return m_parked; }

    void Clear() noexcept
    {
        m_bindings.clear();
        m_localClaims.clear();
        m_parked.clear();
    }

    [[nodiscard]] size_t Size() const noexcept { return m_bindings.size(); }

private:
    TiltedPhoques::Map<uint64_t, Binding> m_bindings{};
    TiltedPhoques::Vector<uint64_t> m_localClaims{};
    TiltedPhoques::Vector<ParkedRemoval> m_parked{};
    uint64_t m_order{};
};
