#include "DotNetExport.h"
#include "ExportSignature.h"
#include "Platform/GenericPlatform.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/TaskTypes.h"

// The managed body is type-erased into a thunk and context the native lambda forwards to.

using namespace Lumina;

namespace
{
    // Mirrors the parallel thunk shape, marshalled as a plain Cdecl function pointer.
    using FThunkC = void (*)(void* Ctx, uint32 Start, uint32 End, uint32 Thread);
}

// BLOCKS until every chunk completes, so the managed context stays valid for the whole call.
LUMINA_DOTNET_EXPORT(void, Task_ParallelFor)(uint32 Num, uint32 MinRange, void* Thunk, void* Ctx, int32 Priority)
{
    FThunkC T = reinterpret_cast<FThunkC>(Thunk);
    if (T == nullptr)
    {
        return;
    }

    Task::ParallelFor(Num, [T, Ctx](const Task::FParallelRange& R)
    {
        T(Ctx, R.Start, R.End, R.Thread);
    }, MinRange, static_cast<ETaskPriority>(Priority));
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(Task_ParallelFor)
);
