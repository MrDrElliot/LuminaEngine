using System;
using LuminaSharp;

namespace Lumina;

// Typed access to an animation graph's parameter block, which the graph asset names as a reflected struct.
public unsafe partial class SAnimationGraphComponent
{
    // Null when the graph names a different struct, or none.
    public T? Parameters<T>() where T : NativeStruct
    {
        IntPtr Memory = NativeBindings.AnimGraphParameterMemory(Handle, typeof(T).Name);
        return Memory == IntPtr.Zero ? null : Wrapper<T>.Create(Memory);
    }

    public T RequireParameters<T>() where T : NativeStruct
    {
        return Parameters<T>() ?? throw new InvalidOperationException(
            $"Animation graph parameter block is not a {typeof(T).Name}.");
    }
}
