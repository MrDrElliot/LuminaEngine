using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Handle to a scheduled timer, returned by <see cref="Timers.SetTimer"/>. Keep it to query or
/// <see cref="Timers.Clear"/> a looping timer. One-shot timers free themselves after firing, so the handle
/// may be discarded.
/// </summary>
public readonly struct TimerHandle
{
    internal readonly uint Id;
    internal readonly GCHandle Callback;

    internal TimerHandle(uint Id, GCHandle Callback)
    {
        this.Id = Id;
        this.Callback = Callback;
    }

    /// <summary>False for a timer that failed to schedule.</summary>
    public bool IsValid => Id != 0xFFFFFFFFu;
}

/// <summary>
/// A world's timer service (<c>World.Timers</c>). Schedules one-shot and looping callbacks against world
/// time (paused with the world, scaled by nothing else). Binds the native per-world FTimerManager. Game
/// thread only.
///
/// One-shot timers (<see cref="Delay"/>, or <see cref="SetTimer"/> with <c>Loop: false</c>) release their
/// callback automatically once they fire. Looping timers hold their callback until <see cref="Clear"/>; clear
/// them before the world is destroyed (e.g. in <c>EntityScript.OnDetach</c>) or use
/// <see cref="SetTimerForEntity"/> so the timer is tied to an entity's lifetime.
/// </summary>
public readonly unsafe partial struct Timers
{
    internal readonly ulong Handle;

    internal Timers(ulong Handle)
    {
        this.Handle = Handle;
    }

    public bool IsValid => Handle != 0;

    // The GCHandle target: the managed callback plus whether it loops (so the trampoline knows when to free).
    private sealed class Entry
    {
        public required Action Body;
        public bool Loop;
    }

    /// <summary>Schedule <paramref name="Callback"/> after <paramref name="Seconds"/>. With <paramref name="Loop"/>
    /// it repeats every <paramref name="Seconds"/>. <paramref name="FirstDelay"/> &gt;= 0 overrides the delay
    /// before the first fire (useful for staggering loopers).</summary>
    public TimerHandle SetTimer(float Seconds, Action Callback, bool Loop = false, float FirstDelay = -1.0f)
    {
        GCHandle Context = GCHandle.Alloc(new Entry { Body = Callback, Loop = Loop });
        uint Id = SetRaw(Seconds, Loop ? 1 : 0, FirstDelay, ThunkPtr, FreeThunkPtr, GCHandle.ToIntPtr(Context));
        if (Id == 0xFFFFFFFFu)
        {
            Context.Free();
            return Invalid;
        }
        return new TimerHandle(Id, Context);
    }

    /// <summary>Run <paramref name="Callback"/> once after <paramref name="Seconds"/>. Self-cleaning.</summary>
    public TimerHandle Delay(float Seconds, Action Callback) => SetTimer(Seconds, Callback, false, -1.0f);

    /// <summary>As <see cref="SetTimer"/>, but owned by <paramref name="Owner"/>: the timer is cleared
    /// automatically when that entity is destroyed.</summary>
    public TimerHandle SetTimerForEntity(Entity Owner, float Seconds, Action Callback, bool Loop = false, float FirstDelay = -1.0f)
    {
        GCHandle Context = GCHandle.Alloc(new Entry { Body = Callback, Loop = Loop });
        uint Id = SetForEntityRaw(Owner.Id, Seconds, Loop ? 1 : 0, FirstDelay, ThunkPtr, FreeThunkPtr, GCHandle.ToIntPtr(Context));
        if (Id == 0xFFFFFFFFu)
        {
            Context.Free();
            return Invalid;
        }
        return new TimerHandle(Id, Context);
    }

    /// <summary>Cancel a timer and release its callback. Safe to call on an already-fired one-shot.</summary>
    public void Clear(TimerHandle Timer)
    {
        if (!Timer.IsValid)
        {
            return;
        }

        // Destroying the native entry frees the context, so freeing here too would be a double free.
        ClearRaw(Timer.Id);
    }

    public bool IsActive(TimerHandle Timer) => Timer.IsValid && IsActiveRaw(Timer.Id) != 0;

    /// <summary>Seconds until the next fire.</summary>
    public float GetRemaining(TimerHandle Timer) => Timer.IsValid ? GetRemainingRaw(Timer.Id) : 0.0f;

    /// <summary>Seconds elapsed in the current interval.</summary>
    public float GetElapsed(TimerHandle Timer) => Timer.IsValid ? GetElapsedRaw(Timer.Id) : 0.0f;

    public void SetPaused(TimerHandle Timer, bool Paused)
    {
        if (Timer.IsValid)
        {
            SetPausedRaw(Timer.Id, Paused ? 1 : 0);
        }
    }

    private static readonly TimerHandle Invalid = new TimerHandle(0xFFFFFFFFu, default);

    // Native owns the handle and frees it through FreeContext when the timer entry dies.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void Fire(IntPtr Context)
    {
        try
        {
            GCHandle Handle = GCHandle.FromIntPtr(Context);
            if (Handle.Target is Entry E)
            {
                E.Body();
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Called by native when a timer entry is destroyed, whatever destroyed it.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void FreeContext(IntPtr Context)
    {
        try
        {
            GCHandle Handle = GCHandle.FromIntPtr(Context);
            if (Handle.IsAllocated)
            {
                Handle.Free();
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    private static readonly IntPtr ThunkPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, void>)&Fire;

    private static readonly IntPtr FreeThunkPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, void>)&FreeContext;

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Timer_Set")]
    private partial uint SetRaw(float Rate, int Loop, float FirstDelay, IntPtr Thunk, IntPtr FreeThunk, IntPtr Context);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Timer_SetForEntity")]
    private partial uint SetForEntityRaw(uint Owner, float Rate, int Loop, float FirstDelay, IntPtr Thunk, IntPtr FreeThunk, IntPtr Context);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Timer_Clear")]
    private partial void ClearRaw(uint Timer);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Timer_IsActive")]
    private partial int IsActiveRaw(uint Timer);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Timer_GetRemaining")]
    private partial float GetRemainingRaw(uint Timer);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Timer_GetElapsed")]
    private partial float GetElapsedRaw(uint Timer);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Timer_SetPaused")]
    private partial void SetPausedRaw(uint Timer, int Paused);
}
