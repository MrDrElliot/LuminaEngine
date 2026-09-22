using System;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace LuminaSharp;

public enum EScriptAsyncState
{
    Unknown = 0,
    Running = 1,
    Completed = 2,
    Faulted = 3,
    Canceled = 4,
}

// A frame cannot wait, so an async script function hands native a token it can poll instead of a result.
internal static class ScriptAsync
{
    private static readonly ConcurrentDictionary<ulong, System.Threading.Tasks.Task> Running = new();

    private static ulong NextToken;

    internal static int RunningCount => Running.Count;

    internal static bool IsFireAndForget(Type ReturnType)
    {
        return ReturnType == typeof(System.Threading.Tasks.Task);
    }

    // Named so the diagnostic can say which spelling to use instead of the one that cannot cross.
    internal static string? WhyUnsupported(Type ReturnType)
    {
        if (ReturnType == typeof(System.Threading.Tasks.ValueTask))
        {
            return "a ValueTask cannot be polled after it completes. Declare the function as async Task.";
        }
        if (ReturnType.IsGenericType)
        {
            Type Definition = ReturnType.GetGenericTypeDefinition();
            if (Definition == typeof(System.Threading.Tasks.Task<>) || Definition == typeof(System.Threading.Tasks.ValueTask<>))
            {
                return "an async result has no frame slot to land in. Declare the function as async Task and "
                    + "report the result through an out parameter or a callback.";
            }
        }
        return null;
    }

    internal static ulong Track(object? Result)
    {
        if (Result is not System.Threading.Tasks.Task Started)
        {
            return 0;
        }

        ulong Token = Interlocked.Increment(ref NextToken);
        Running[Token] = Started;
        return Token;
    }

    internal static EScriptAsyncState StateOf(ulong Token)
    {
        if (!Running.TryGetValue(Token, out System.Threading.Tasks.Task? Started))
        {
            return EScriptAsyncState.Unknown;
        }
        if (!Started.IsCompleted)
        {
            return EScriptAsyncState.Running;
        }
        if (Started.IsCanceled)
        {
            return EScriptAsyncState.Canceled;
        }
        return Started.IsFaulted ? EScriptAsyncState.Faulted : EScriptAsyncState.Completed;
    }

    internal static void Release(ulong Token)
    {
        if (Running.TryRemove(Token, out System.Threading.Tasks.Task? Started) && Started.IsFaulted)
        {
            Interop.LogException(Started.Exception!);
        }
    }

    // The bodies keep running, but their continuations post to a queue the unload clears, so none resume.
    internal static void Clear()
    {
        Running.Clear();
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int ScriptAsyncState(ulong Token)
    {
        try
        {
            return (int)StateOf(Token);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return (int)EScriptAsyncState.Unknown;
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void ScriptAsyncRelease(ulong Token)
    {
        try
        {
            Release(Token);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }
}
