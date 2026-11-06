using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// What CTaskLibrary cannot express, so the parallel fan-out plus a disposable wrapper over its raw handle.
public static unsafe partial class Task
{
    // Lumina::Jobs::EJobPriority::Normal.
    private const int PriorityNormal = 1;

    // A body runs on a job worker, so blocking, awaiting or re-entering the task system from it corrupts the CLR.
    public static void ParallelFor(int Count, Action<int> Body)
    {
        if (Count <= 0)
        {
            return;
        }

        // Native releases the body once every chunk is done, and the call blocks until then.
        CTaskLibrary.ParallelFor((uint)Count, 0, ScriptCallback.Bind(new ParallelBody(Body)), PriorityNormal);
    }

    // Runs one chunk, Start in the low half of the payload and End in the high half.
    private sealed class ParallelBody : ScriptClosure
    {
        private readonly Action<int> Body;

        public ParallelBody(Action<int> Body)
        {
            this.Body = Body;
        }

        public override ulong Invoke(ulong Payload)
        {
            for (int Index = (int)(uint)Payload, End = (int)(Payload >> 32); Index < End; ++Index)
            {
                Body(Index);
            }
            return 0;
        }
    }

    // Wraps CTaskLibrary.Run so a using block releases the native handle rather than the caller remembering.
    public static TaskHandle Run(Action Body)
    {
        return new TaskHandle(CTaskLibrary.Run(ScriptCallback.Of(Body), PriorityNormal));
    }
}

// A value type cannot guard itself, so a second Dispose double-frees the native completion handle.
public readonly struct TaskHandle : IDisposable
{
    internal readonly ulong Handle;

    internal TaskHandle(ulong Handle)
    {
        this.Handle = Handle;
    }

    public bool IsValid => Handle != 0;

    // Blocks until the task has completed.
    public void Wait()
    {
        if (Handle != 0)
        {
            CTaskLibrary.Wait(Handle);
        }
    }

    public void Dispose()
    {
        if (Handle != 0)
        {
            CTaskLibrary.Release(Handle);
        }
    }
}
