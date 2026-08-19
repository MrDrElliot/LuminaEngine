#pragma once

#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    // A plugin-contributed entry in the editor Tools menu. Core EditorUI hard-codes its built-in
    // tools; this registry is the extension point that lets an editor plugin surface a standalone
    // tool (one without an asset/file association) WITHOUT editing core. Register at module
    // StartupModule; FEditorUI::DrawToolsMenu renders all entries under a "Plugins" section.
    //
    // The callbacks are type-erased so the registry stays decoupled from any specific tool type:
    // the plugin captures the concrete tool type (and fetches the live FEditorUI itself).
    struct FToolsMenuEntry
    {
        FString           Label;      // menu text, may embed an LE_ICON_* glyph
        TFunction<bool()> IsActive;   // optional; drives the checkmark (tool currently open?)
        TFunction<void()> OnToggle;   // required; open the tool if closed, else close it
        uint32            Handle = 0; // assigned by Register; opaque to the caller
    };

    // Process-wide registry of plugin-contributed Tools-menu entries.
    //
    // LIFETIME: the callbacks capture code that lives in the REGISTERING module's DLL, but this
    // registry is a static singleton (in the Editor module) that outlives every plugin. A plugin
    // MUST Unregister in its ShutdownModule, before its DLL is unloaded -- otherwise the entry's
    // TFunctions are destroyed against unmapped code when the registry tears down, and the process
    // crashes on exit.
    class EDITOR_API FToolsMenuRegistry
    {
    public:

        static FToolsMenuRegistry& Get();

        // Returns a handle to pass to Unregister.
        uint32 Register(FToolsMenuEntry Entry);

        // Remove a previously registered entry. Safe to call with 0 or an already-removed handle.
        void Unregister(uint32 Handle);

        const TVector<FToolsMenuEntry>& GetEntries() const { return Entries; }

        bool IsEmpty() const { return Entries.empty(); }

    private:

        TVector<FToolsMenuEntry> Entries;
        uint32                   NextHandle = 1;
    };
}
