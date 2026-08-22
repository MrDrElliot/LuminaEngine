#include "DotNetExport.h"
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

// Returns a heap-copied handle the C# side keeps alive and must release explicitly.
LUMINA_DOTNET_EXPORT(void*, Task_Run)(void* Thunk, void* Ctx, int32 Priority)
{
    FThunkC T = reinterpret_cast<FThunkC>(Thunk);
    if (T == nullptr)
    {
        return nullptr;
    }

    FTaskHandle H = Task::AsyncTask(1, 0, [T, Ctx](uint32 Start, uint32 End, uint32 Thread)
    {
        T(Ctx, Start, End, Thread);
    }, static_cast<ETaskPriority>(Priority));

    return new FTaskHandle(H);
}

// Blocks until the task behind the handle has completed.
LUMINA_DOTNET_EXPORT(void, Task_Wait)(void* Handle)
{
    if (Handle != nullptr)
    {
        (*static_cast<FTaskHandle*>(Handle))->Wait();
    }
}

// Drops the heap-copied FTaskHandle (releases its refcount on the completion state).
LUMINA_DOTNET_EXPORT(void, Task_Release)(void* Handle)
{
    delete static_cast<FTaskHandle*>(Handle);
}

// Blocks until every job submitted so far has completed.
LUMINA_DOTNET_EXPORT(void, Task_WaitForAll)()
{
    GTaskSystem->WaitForAll();
}

// Number of background worker threads.
LUMINA_DOTNET_EXPORT(int32, Task_NumWorkers)()
{
    return static_cast<int32>(GTaskSystem->GetNumWorkers());
}
