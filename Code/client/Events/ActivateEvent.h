#pragma once

struct TESObjectREFR;
struct Actor;
struct TESBoundObject;

/**
 * @brief In-game activation (i.e. interacting with an object).
 */
struct ActivateEvent
{
    ActivateEvent(TESObjectREFR* apObject, Actor* apActivator, TESBoundObject* apObjectToGet, int32_t aCount, bool aDefaultProcessing, uint8_t aUnk1, TESObjectREFR::OpenState aPreActivationOpenState, bool aActivateFlag = false)
        : pObject(apObject)
        , pActivator(apActivator)
        , pObjectToGet(apObjectToGet)
        , Count(aCount)
        , DefaultProcessing(aDefaultProcessing)
        , Unk1(aUnk1)
        , PreActivationOpenState(aPreActivationOpenState)
        , ActivateFlag(aActivateFlag)
        , ObjectFormId(apObject->formID)
        , ObjectBaseFormId(apObject->baseForm ? apObject->baseForm->formID : 0)
    {
    }

    TESObjectREFR* pObject;
    Actor* pActivator;
    TESBoundObject* pObjectToGet;
    int32_t Count;
    bool DefaultProcessing;
    uint8_t Unk1;
    TESObjectREFR::OpenState PreActivationOpenState;
    bool ActivateFlag;
    // Snapshot before activation can consume the object. Do not dereference pObject to route a dropped pickup.
    uint32_t ObjectFormId{};
    uint32_t ObjectBaseFormId{};
};
