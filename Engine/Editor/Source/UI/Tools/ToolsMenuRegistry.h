#pragma once

#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    // A plugin-contributed Tools menu entry with type-erased callbacks, so the registry knows no concrete tool type.
    struct FToolsMenuEntry
    {
        FString           Label;      // menu text, may embed an LE_ICON_* glyph
        TFunction<bool()> IsActive;   // optional; drives the checkmark (tool currently open?)
        TFunction<void()> OnToggle;   // required; open the tool if closed, else close it
        uint32            Handle = 0; // assigned by Register; opaque to the caller
    };

    // Outlives every plugin, so a plugin must Unregister in ShutdownModule or its callbacks are destroyed against unmapped code on exit.
    class EDITOR_API FToolsMenuRegistry
    {
    public:

        static FToolsMenuRegistry& Get();

        // Returns a handle to pass to Unregister.
        uint32 Register(FToolsMenuEntry Entry);

        // Remove a previously registered entry. Safe to call with 0 or an already-removed handle.
        void Unregister(uint32 Handle);

        const TVector<FToolsMenuEntry>& GetEntries() const { return Entries; }

        bool IsEmpty() const { return Entries.IsEmpty(); }

    private:

        TVector<FToolsMenuEntry> Entries;
        uint32                   NextHandle = 1;
    };
}
