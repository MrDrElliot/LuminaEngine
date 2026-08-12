namespace LuminaSharp;

// The type-erased op tables the native container property publishes, mirroring Lumina::FVectorOps and
// Lumina::FMapOps (Runtime/Source/Containers/ContainerOps.h) -- which is why they live together in a file
// named after that header rather than beside any one view that uses them.
//
// They stay in LuminaSharp, not Lumina, on purpose. The views themselves are mirrors of C++ container types
// and a script author writes them; these are marshalling plumbing nobody outside this assembly touches, so
// they belong with Native and NativeMarshal.
//
// Both are a PREFIX of the native struct, not the whole of it: the fields below must stay at the same
// offsets, and native appends anything new after them (it carries container construct/destruct entries C#
// has no use for). So a native change is safe here only while it is append-only.

#pragma warning disable CS0649 // fields are populated by overlaying native memory, not managed assignment

/// <summary>Mirror of native <c>Lumina::FVectorOps</c>. C# calls only PushBack/RemoveAt/Clear (reads decode
/// the header directly), and [Cdecl] matches the captureless lambdas on x64.</summary>
internal unsafe struct VectorOps
{
    public delegate* unmanaged[Cdecl]<void*, nuint> Size;
    public delegate* unmanaged[Cdecl]<void*, void*> Data;
    public delegate* unmanaged[Cdecl]<void*, void*, void> PushBack;
    public delegate* unmanaged[Cdecl]<void*, nuint, void> RemoveAt;
    public delegate* unmanaged[Cdecl]<void*, void> Clear;
    public delegate* unmanaged[Cdecl]<void*, nuint, void> Resize;
    public delegate* unmanaged[Cdecl]<void*, nuint, void> Reserve;
    public delegate* unmanaged[Cdecl]<void*, nuint, nuint, void> Swap;
    public uint ElementSize;
}

/// <summary>Mirror of native <c>Lumina::FMapOps</c>, the associative counterpart of <see cref="VectorOps"/>.
/// C++ <c>bool</c> is one byte, so RemoveByKey returns a byte here.</summary>
internal unsafe struct MapOps
{
    public delegate* unmanaged[Cdecl]<void*, nuint> Size;
    public delegate* unmanaged[Cdecl]<void*, void*, void*, void*> Insert;
    public delegate* unmanaged[Cdecl]<void*, void*, void*> Find;
    public delegate* unmanaged[Cdecl]<void*, void*, byte> RemoveByKey;
    public delegate* unmanaged[Cdecl]<void*, void> Clear;
    public delegate* unmanaged[Cdecl]<void*, nuint, void> Reserve;
    public delegate* unmanaged[Cdecl]<void*, void*, void*, void> ForEach;
    public delegate* unmanaged[Cdecl]<void*, void*, void> ConstructKey;
    public delegate* unmanaged[Cdecl]<void*, void*, void> DestructKey;
    public delegate* unmanaged[Cdecl]<void*, nuint, void**, void**, void> At;
    public uint KeySize;
    public uint ValueSize;
}

#pragma warning restore CS0649
