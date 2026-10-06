#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "ModuleAPI.h"

namespace Lumina
{
    class CObject;
    class CWorld;

    // The UI calls into LuminaSharp, each a no-op while scripts are not loaded.
    namespace DotNetUI
    {
        // Pushes what changed in every C# ViewModel bound in World, just before its UI updates.
        RUNTIME_API void PollModels(CWorld* World);

        // A UIScript's document loaded, so its [Element] members resolve and its OnDocumentLoaded runs. A no-op for a C++ script.
        RUNTIME_API void ScriptDocumentLoaded(CObject* Script);
    }
}
