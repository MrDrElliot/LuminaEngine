#include "EntityPropertyContext.h"

namespace Lumina
{
    namespace
    {
        // Nested DrawTree calls inherit it, and save and restore keep them balanced.
        CWorld* GActiveEntityPropertyWorld = nullptr;
    }

    CWorld* GetEntityPropertyContextWorld()
    {
        return GActiveEntityPropertyWorld;
    }

    FScopedEntityPropertyContext::FScopedEntityPropertyContext(CWorld* InWorld)
        : Previous(GActiveEntityPropertyWorld)
    {
        GActiveEntityPropertyWorld = InWorld;
    }

    FScopedEntityPropertyContext::~FScopedEntityPropertyContext()
    {
        GActiveEntityPropertyWorld = Previous;
    }

    namespace
    {
        bool   GPickRequested = false;  // a picker is waiting for a viewport click
        uint64 GPickToken     = 0;      // identifies the requesting picker
        bool   GPickHasResult = false;  // a clicked entity is pending consumption
        uint32 GPickResult    = 0;      // integral id of the clicked entity
    }

    void RequestEntityPick(uint64 Token)
    {
        GPickRequested = true;
        GPickToken     = Token;
        GPickHasResult = false;
    }

    void CancelEntityPick()
    {
        GPickRequested = false;
        GPickHasResult = false;
    }

    bool IsEntityPickRequested()
    {
        return GPickRequested;
    }

    void FulfillEntityPick(uint32 Entity)
    {
        if (GPickRequested)
        {
            GPickResult    = Entity;
            GPickHasResult = true;
            GPickRequested = false;
        }
    }

    bool IsEntityPickActiveFor(uint64 Token)
    {
        return GPickRequested && GPickToken == Token;
    }

    bool ConsumeEntityPickResult(uint64 Token, uint32& OutEntity)
    {
        if (GPickHasResult && GPickToken == Token)
        {
            OutEntity      = GPickResult;
            GPickHasResult = false;
            return true;
        }
        return false;
    }
}
