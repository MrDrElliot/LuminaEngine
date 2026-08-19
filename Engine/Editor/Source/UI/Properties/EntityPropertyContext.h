#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CWorld;
    
    EDITOR_API CWorld* GetEntityPropertyContextWorld();
    
    class FScopedEntityPropertyContext
    {
    public:
        explicit FScopedEntityPropertyContext(CWorld* InWorld);
        ~FScopedEntityPropertyContext();

        FScopedEntityPropertyContext(const FScopedEntityPropertyContext&) = delete;
        FScopedEntityPropertyContext& operator=(const FScopedEntityPropertyContext&) = delete;

    private:
        CWorld* Previous = nullptr;
    };

    // Begin a pick for the picker identified by Token (replaces any in-flight request).
    void RequestEntityPick(uint64 Token);

    // Abort the active pick, whoever owns it. Safe to call when none is active.
    void CancelEntityPick();

    // Viewport: is a picker waiting for a clicked entity this frame?
    bool IsEntityPickRequested();

    // Viewport: deliver the clicked entity's integral id to the waiting picker.
    void FulfillEntityPick(uint32 Entity);

    // Picker: true while Token owns the active request (drives the button highlight).
    bool IsEntityPickActiveFor(uint64 Token);

    // Picker: if a result is pending for Token, write it to OutEntity, clear it, return true.
    bool ConsumeEntityPickResult(uint64 Token, uint32& OutEntity);
}
