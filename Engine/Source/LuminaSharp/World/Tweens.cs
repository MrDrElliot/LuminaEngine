using System;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Curve family, chosen independently of the direction it is applied in.
public enum Transition
{
    Linear, Sine, Quad, Cubic, Quart, Quint, Expo, Circ,
    Back,
    Elastic,
    Bounce,
    Spring,
}

// Which end of the curve the shaping is applied to.
public enum Ease { In, Out, InOut, OutIn }

// Tweeners run one after another; Parallel puts the next one in the same step as the last.
public readonly unsafe partial struct Tween
{
    internal readonly ulong World;
    internal readonly uint Id;

    internal Tween(ulong World, uint Id)
    {
        this.World = World;
        this.Id = Id;
    }

    public bool IsRunning => World != 0 && IsRunningRaw(World, Id) != 0;

    public Tween MoveTo(Entity Target, FVector3 Position, float Duration)
    {
        MoveToRaw(World, Id, Target.Id, Position, Duration);
        return this;
    }

    // Takes the short way around, since it slerps.
    public Tween RotateTo(Entity Target, FQuat Rotation, float Duration)
    {
        RotateToRaw(World, Id, Target.Id, Rotation, Duration);
        return this;
    }

    public Tween ScaleTo(Entity Target, FVector3 Scale, float Duration)
    {
        ScaleToRaw(World, Id, Target.Id, Scale, Duration);
        return this;
    }

    public Tween Value(float From, float To, float Duration, Action<float> Setter)
    {
        ArgumentNullException.ThrowIfNull(Setter);

        GCHandle Handle = GCHandle.Alloc(Setter);
        ValueToRaw(World, Id, From, To, Duration,
            (delegate* unmanaged[Cdecl]<void*, float, void>)&ValueTrampoline,
            (delegate* unmanaged[Cdecl]<void*, void>)&FreeTrampoline,
            (void*)GCHandle.ToIntPtr(Handle));
        return this;
    }

    // Dead time, for spacing steps apart.
    public Tween Interval(float Duration)
    {
        IntervalRaw(World, Id, Duration);
        return this;
    }

    public Tween Call(Action Callback)
    {
        ArgumentNullException.ThrowIfNull(Callback);

        GCHandle Handle = GCHandle.Alloc(Callback);
        CallRaw(World, Id, (delegate* unmanaged[Cdecl]<void*, void>)&CallTrampoline,
            (delegate* unmanaged[Cdecl]<void*, void>)&FreeTrampoline,
            (void*)GCHandle.ToIntPtr(Handle));
        return this;
    }

    // Fires after the last step, including after the final loop.
    public Tween OnFinished(Action Callback)
    {
        ArgumentNullException.ThrowIfNull(Callback);

        GCHandle Handle = GCHandle.Alloc(Callback);
        OnFinishedRaw(World, Id, (delegate* unmanaged[Cdecl]<void*, void>)&CallTrampoline,
            (delegate* unmanaged[Cdecl]<void*, void>)&FreeTrampoline,
            (void*)GCHandle.ToIntPtr(Handle));
        return this;
    }

    // Transition, EaseWith and Delay all apply to the tweener that was added last.
    public Tween Trans(Transition Transition)
    {
        TransRaw(World, Id, (int)Transition);
        return this;
    }

    // Named EaseWith so it does not collide with the Ease enum.
    public Tween EaseWith(Ease Ease)
    {
        EaseRaw(World, Id, (int)Ease);
        return this;
    }

    public Tween Delay(float Seconds)
    {
        DelayRaw(World, Id, Seconds);
        return this;
    }

    public Tween Parallel()
    {
        ParallelRaw(World, Id);
        return this;
    }

    // 0 repeats forever, 1 is the default single pass.
    public Tween SetLoops(int Count)
    {
        SetLoopsRaw(World, Id, Count);
        return this;
    }

    public Tween SetSpeedScale(float Scale)
    {
        SetSpeedScaleRaw(World, Id, Scale);
        return this;
    }

    public Tween SetPaused(bool Paused)
    {
        SetPausedRaw(World, Id, Paused ? 1 : 0);
        return this;
    }

    // Stops where it is; whatever it was driving keeps its current value.
    public void Kill() => KillRaw(World, Id);

    // Native owns each GCHandle and frees it through FreeTrampoline.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static void CallTrampoline(void* Context)
    {
        if (GCHandle.FromIntPtr((IntPtr)Context).Target is Action Body)
        {
            Body();
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static void ValueTrampoline(void* Context, float Value)
    {
        if (GCHandle.FromIntPtr((IntPtr)Context).Target is Action<float> Setter)
        {
            Setter(Value);
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static void FreeTrampoline(void* Context)
    {
        GCHandle Handle = GCHandle.FromIntPtr((IntPtr)Context);
        if (Handle.IsAllocated)
        {
            Handle.Free();
        }
    }

    [NativeCall("LuminaSharp_Tween_MoveTo", SuppressGCTransition = true)]
    private static partial void MoveToRaw(ulong World, uint Id, uint Entity, FVector3 Target, float Duration);

    [NativeCall("LuminaSharp_Tween_RotateTo", SuppressGCTransition = true)]
    private static partial void RotateToRaw(ulong World, uint Id, uint Entity, FQuat Target, float Duration);

    [NativeCall("LuminaSharp_Tween_ScaleTo", SuppressGCTransition = true)]
    private static partial void ScaleToRaw(ulong World, uint Id, uint Entity, FVector3 Target, float Duration);

    [NativeCall("LuminaSharp_Tween_ValueTo")]
    private static partial void ValueToRaw(ulong World, uint Id, float From, float To, float Duration,
        delegate* unmanaged[Cdecl]<void*, float, void> Thunk,
        delegate* unmanaged[Cdecl]<void*, void> FreeThunk, void* Context);

    [NativeCall("LuminaSharp_Tween_Interval", SuppressGCTransition = true)]
    private static partial void IntervalRaw(ulong World, uint Id, float Duration);

    [NativeCall("LuminaSharp_Tween_Call")]
    private static partial void CallRaw(ulong World, uint Id,
        delegate* unmanaged[Cdecl]<void*, void> Thunk,
        delegate* unmanaged[Cdecl]<void*, void> FreeThunk, void* Context);

    [NativeCall("LuminaSharp_Tween_OnFinished")]
    private static partial void OnFinishedRaw(ulong World, uint Id,
        delegate* unmanaged[Cdecl]<void*, void> Thunk,
        delegate* unmanaged[Cdecl]<void*, void> FreeThunk, void* Context);

    [NativeCall("LuminaSharp_Tween_Trans", SuppressGCTransition = true)]
    private static partial void TransRaw(ulong World, uint Id, int Transition);

    [NativeCall("LuminaSharp_Tween_Ease", SuppressGCTransition = true)]
    private static partial void EaseRaw(ulong World, uint Id, int Ease);

    [NativeCall("LuminaSharp_Tween_Delay", SuppressGCTransition = true)]
    private static partial void DelayRaw(ulong World, uint Id, float Seconds);

    [NativeCall("LuminaSharp_Tween_Parallel", SuppressGCTransition = true)]
    private static partial void ParallelRaw(ulong World, uint Id);

    [NativeCall("LuminaSharp_Tween_SetLoops", SuppressGCTransition = true)]
    private static partial void SetLoopsRaw(ulong World, uint Id, int Count);

    [NativeCall("LuminaSharp_Tween_SetSpeedScale", SuppressGCTransition = true)]
    private static partial void SetSpeedScaleRaw(ulong World, uint Id, float Scale);

    [NativeCall("LuminaSharp_Tween_SetPaused", SuppressGCTransition = true)]
    private static partial void SetPausedRaw(ulong World, uint Id, int Paused);

    [NativeCall("LuminaSharp_Tween_Kill", SuppressGCTransition = true)]
    private static partial void KillRaw(ulong World, uint Id);

    [NativeCall("LuminaSharp_Tween_IsRunning", SuppressGCTransition = true)]
    private static partial int IsRunningRaw(ulong World, uint Id);
}

// A world's tween service, reached as World.Tweens. Game thread only.
public readonly unsafe partial struct Tweens
{
    internal readonly ulong Handle;

    internal Tweens(ulong Handle)
    {
        this.Handle = Handle;
    }

    public bool IsValid => Handle != 0;

    public Tween Create() => new Tween(Handle, CreateRaw(Handle, 0u, 0));

    // Killed automatically when Owner is destroyed, which is what a gameplay tween usually wants.
    public Tween CreateFor(Entity Owner) => new Tween(Handle, CreateRaw(Handle, Owner.Id, 1));

    [NativeCall("LuminaSharp_Tween_Create", SuppressGCTransition = true)]
    private static partial uint CreateRaw(ulong World, uint Owner, int HasOwner);
}
