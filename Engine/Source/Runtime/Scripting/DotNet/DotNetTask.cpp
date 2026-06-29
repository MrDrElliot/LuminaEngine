#include "Platform/GenericPlatform.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/TaskTypes.h"

// Hand-written native -> C# bindings for the engine Task system: lets C# do Task.ParallelFor, schedule a
// one-shot async task, and wait. Each export is `extern "C" LUMINA_SCRIPT_API LuminaSharp_Task_<Op>` resolved on
// the C# side by the [NativeCall] generated delegate* (NativeBindings.Resolve("Runtime", ...)), mirroring
// the gameplay facades. The managed body is type-erased into a Thunk + Ctx the native lambda forwards to;
// Ctx is a GCHandle to the managed Action the C# side owns and frees.

using namespace Lumina;

namespace
{
    // The managed thunk the C# side passes: receives its GCHandle Ctx plus the [Start, End) range and the
    // executing worker slot. Mirrors the FParallelThunk shape, marshalled as a plain Cdecl function pointer.
    using FThunkC = void (*)(void* Ctx, uint32 Start, uint32 End, uint32 Thread);
}

// Splits [0, Num) across worker threads and invokes the managed thunk per chunk. BLOCKS until every chunk
// completes (ParallelFor is synchronous), so Ctx (the C# GCHandle) stays valid for the whole call.
extern "C" LUMINA_SCRIPT_API void LuminaSharp_Task_ParallelFor(uint32 Num, uint32 MinRange, void* Thunk, void* Ctx, int32 Priority)
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

// Schedules one async task that invokes the managed thunk once. Returns a heap-copied FTaskHandle (the
// completion shared_ptr) the C# side keeps alive and must release via LuminaSharp_Task_Release.
extern "C" LUMINA_SCRIPT_API void* LuminaSharp_Task_Run(void* Thunk, void* Ctx, int32 Priority)
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
extern "C" LUMINA_SCRIPT_API void LuminaSharp_Task_Wait(void* Handle)
{
    if (Handle != nullptr)
    {
        (*static_cast<FTaskHandle*>(Handle))->Wait();
    }
}

// Drops the heap-copied FTaskHandle (releases its refcount on the completion state).
extern "C" LUMINA_SCRIPT_API void LuminaSharp_Task_Release(void* Handle)
{
    delete static_cast<FTaskHandle*>(Handle);
}

// Blocks until every job submitted so far has completed.
extern "C" LUMINA_SCRIPT_API void LuminaSharp_Task_WaitForAll()
{
    GTaskSystem->WaitForAll();
}

// Number of background worker threads.
extern "C" LUMINA_SCRIPT_API int32 LuminaSharp_Task_NumWorkers()
{
    return static_cast<int32>(GTaskSystem->GetNumWorkers());
}
