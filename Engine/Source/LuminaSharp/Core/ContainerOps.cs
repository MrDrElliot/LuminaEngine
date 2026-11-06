namespace LuminaSharp;

// Prefixes of the native FVectorOps and FMapOps, kept at the same offsets since native only appends to them.

#pragma warning disable CS0649 // fields are populated by overlaying native memory, not managed assignment

// Mirror of native Lumina::FVectorOps. Num is a plain read on every table, so it skips the GC transition.
internal unsafe struct VectorOps
{
    public delegate* unmanaged[Cdecl, SuppressGCTransition]<void*, int> Num;
    public delegate* unmanaged[Cdecl]<void*, void*> GetData;
    public delegate* unmanaged[Cdecl]<void*, void*, void> Add;
    public delegate* unmanaged[Cdecl]<void*, int, void> RemoveAt;
    public delegate* unmanaged[Cdecl]<void*, void> Reset;
    public delegate* unmanaged[Cdecl]<void*, int, void> SetNum;
    public delegate* unmanaged[Cdecl]<void*, int, void> Reserve;
    public delegate* unmanaged[Cdecl]<void*, int, int, void> Swap;
    public uint ElementSize;
    public void* ConstructContainer;
    public void* DestructContainer;
    public void* ContainerContext;
    public uint ContainerSize;
    public uint DataOffset;
    public uint CountOffset;
    public uint CountDivisor;
}

// Mirror of native Lumina::FMapOps, where a C++ bool return is one byte.
internal unsafe struct MapOps
{
    public delegate* unmanaged[Cdecl, SuppressGCTransition]<void*, int> Num;
    public delegate* unmanaged[Cdecl]<void*, void*, void*, void*> Add;
    public delegate* unmanaged[Cdecl]<void*, void*, void*> Find;
    public delegate* unmanaged[Cdecl]<void*, void*, byte> Remove;
    public delegate* unmanaged[Cdecl]<void*, void> Reset;
    public delegate* unmanaged[Cdecl]<void*, int, void> Reserve;
    public delegate* unmanaged[Cdecl]<void*, void*, void*, void> ForEach;
    public delegate* unmanaged[Cdecl]<void*, void*, void> ConstructKey;
    public delegate* unmanaged[Cdecl]<void*, void*, void> DestructKey;
    public delegate* unmanaged[Cdecl]<void*, int, void**, void**, void> At;
    public uint KeySize;
    public uint ValueSize;
    public void* ConstructContainer;
    public void* DestructContainer;
    public void* ContainerContext;
    public uint ContainerSize;
    public delegate* unmanaged[Cdecl, SuppressGCTransition]<void*, int*, void**, void**, byte> Next;
}

#pragma warning restore CS0649
