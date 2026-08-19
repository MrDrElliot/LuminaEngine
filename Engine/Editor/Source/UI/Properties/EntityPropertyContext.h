#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CWorld;

    // Draw-scoped world for PROPERTY(Entity) pickers, so they can enumerate/select
    // entities. Null outside such a scope (e.g. asset editors): picker falls back to a read-only id.
    RUNTIME_API CWorld* GetEntityPropertyContextWorld();

    // RAII: makes World the active entity-property context for the scope's lifetime and
    // restores the previous one on exit. Single-threaded editor UI use only.
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

    // Eyedropper: bridges an entity-ref property picker and the viewport. One pick
    // active at a time; picker polls ConsumeEntityPickResult and writes the chosen id.

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
