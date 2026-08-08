#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    struct FSkeletonResource;

    // Context for FName properties tagged SocketPicker: entity details push the attach target's
    // available sockets (and, for skeletal parents, the skeleton so raw bones are pickable too).
    namespace SocketPickerContext
    {
        struct FSocketPickerData
        {
            TVector<FName> Sockets;
            const FSkeletonResource* Skeleton = nullptr;   // non-null: bones are attachable too
        };

        // Push/pop on a small stack so re-entrant draws restore the outer context correctly.
        void Push(const FSocketPickerData* Data);
        void Pop();

        const FSocketPickerData* GetActive();

        // RAII wrapper: pushes on construction, pops on destruction.
        struct FScope
        {
            explicit FScope(const FSocketPickerData* Data) { Push(Data); }
            ~FScope()                                      { Pop(); }
            FScope(const FScope&) = delete;
            FScope& operator=(const FScope&) = delete;
        };
    }
}
