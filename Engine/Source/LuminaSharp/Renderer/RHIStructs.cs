using System;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp.Rendering;

// Blittable mirrors of the engine RHI descriptor structs (Runtime/Renderer/RHI.h). Field order and types
// match the native structs byte-for-byte so they pass by value with zero marshalling. C++ `bool`/`uint8`
// are mirrored as C# `byte` (a C# `bool` is not reliably 1 byte in a blittable struct). Descriptors that
// embed a native span (FRasterDesc, FRenderPassDesc, FShaderSource) are handled at the RHI call site, which
// takes the span as a separate ReadOnlySpan argument; the scalar parts live here.

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FRect")]
public struct FRect
{
    public int MinX, MaxX;
    public int MinY, MaxY;

    public FRect(int MinX, int MinY, int MaxX, int MaxY)
    {
        this.MinX = MinX; this.MinY = MinY; this.MaxX = MaxX; this.MaxY = MaxY;
    }

    /// <summary>A 0,0-origin rect of the given size.</summary>
    public static FRect Size(int Width, int Height) => new(0, 0, Width, Height);
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FTextureDesc")]
public struct FTextureDesc
{
    public ETextureType Type;
    public FUIntVector3 Dimension;
    public uint MipCount;
    public uint LayerCount;
    public uint SampleCount;
    public EFormat Format;
    public EImageUsageFlags Usage;

    public static FTextureDesc Texture2D(uint Width, uint Height, EFormat Format, EImageUsageFlags Usage)
        => new()
        {
            Type = ETextureType.Tex2D,
            Dimension = new FUIntVector3(Width, Height, 1u),
            MipCount = 1,
            LayerCount = 1,
            SampleCount = 1,
            Format = Format,
            Usage = Usage,
        };
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FBlendDesc")]
public struct FBlendDesc
{
    public byte    BlendEnable;       // C++ bool
    public EBlend  ColorOp;
    public EFactor SrcColorFactor;
    public EFactor DstColorFactor;
    public EBlend  AlphaOp;
    public EFactor SrcAlphaFactor;
    public EFactor DstAlphaFactor;
    public byte    ColorWriteMask;

    public bool Enabled { get => BlendEnable != 0; set => BlendEnable = value ? (byte)1 : (byte)0; }

    public static FBlendDesc Opaque => new()
    {
        BlendEnable = 0,
        ColorOp = EBlend.Add, SrcColorFactor = EFactor.One, DstColorFactor = EFactor.Zero,
        AlphaOp = EBlend.Add, SrcAlphaFactor = EFactor.One, DstAlphaFactor = EFactor.Zero,
        ColorWriteMask = 0xF,
    };

    public static FBlendDesc AlphaBlend => new()
    {
        BlendEnable = 1,
        ColorOp = EBlend.Add, SrcColorFactor = EFactor.SrcAlpha, DstColorFactor = EFactor.OneMinusSrcAlpha,
        AlphaOp = EBlend.Add, SrcAlphaFactor = EFactor.One, DstAlphaFactor = EFactor.OneMinusSrcAlpha,
        ColorWriteMask = 0xF,
    };
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FSamplerDesc")]
public struct FSamplerDesc
{
    public EFilter      MinFilter;
    public EFilter      MagFilter;
    public EFilter      MipFilter;
    public EAddressMode AddressU;
    public EAddressMode AddressV;
    public EAddressMode AddressW;
    public float        MaxAnisotropy;
    public float        MipBias;
    public EOp          CompareOp;   // Never = compare disabled
    public EReduction   Reduction;

    public static FSamplerDesc LinearWrap => new()
    {
        MinFilter = EFilter.Linear, MagFilter = EFilter.Linear, MipFilter = EFilter.Linear,
        AddressU = EAddressMode.Repeat, AddressV = EAddressMode.Repeat, AddressW = EAddressMode.Repeat,
        MaxAnisotropy = 1.0f, MipBias = 0.0f, CompareOp = EOp.Never, Reduction = EReduction.None,
    };
}

// Extent of 0 = full mip extent.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FTextureSlice")]
public struct FTextureSlice
{
    public uint         Mip;
    public uint         Layer;
    public uint         LayerCount;
    public FUIntVector3 Offset;
    public FUIntVector3 Extent;

    public static FTextureSlice Full => new() { LayerCount = 1 };
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FStencil")]
public struct FStencil
{
    public EOp        Test;
    public EStencilOp FailOp;
    public EStencilOp PassOp;
    public EStencilOp DepthFailOp;
    public byte       Reference;
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FDepthStencilDesc")]
public struct FDepthStencilDesc
{
    public EDepthFlags DepthMode;
    public EOp         DepthTest;
    public float       DepthBias;
    public float       DepthBiasSlopeFactor;
    public float       DepthBiasClamp;
    public byte        StencilReadMask;
    public byte        StencilWriteMask;
    public FStencil    StencilFront;
    public FStencil    StencilBack;
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FColorTarget")]
public struct FColorTarget
{
    public EFormat    Format;
    public FBlendDesc Blend;

    public FColorTarget(EFormat Format, FBlendDesc Blend)
    {
        this.Format = Format;
        this.Blend = Blend;
    }
}

// Scalar part of the engine FRasterDesc; ColorTargets is passed alongside as a ReadOnlySpan at the call site.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FRasterDesc")]
public struct FRasterDesc
{
    public ETopology Topology;
    public byte      AlphaToCoverage;   // C++ bool
    public byte      Wireframe;         // C++ bool
    public byte      SampleCount;
    public EFormat   DepthFormat;
    public EFormat   StencilFormat;
    private byte     _Reserved0;        // pad to 8 bytes so the wire struct passes by value unambiguously
    private byte     _Reserved1;

    public static FRasterDesc Default => new()
    {
        Topology = ETopology.TriangleList,
        AlphaToCoverage = 0,
        Wireframe = 0,
        SampleCount = 1,
        DepthFormat = EFormat.UNKNOWN,
        StencilFormat = EFormat.UNKNOWN,
    };
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FSemaphoreInfo")]
public struct FSemaphoreInfo
{
    public FSemaphoreH Semaphore;
    public ulong       Value;

    public FSemaphoreInfo(FSemaphoreH Semaphore, ulong Value)
    {
        this.Semaphore = Semaphore;
        this.Value = Value;
    }
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FDrawIndirectArguments")]
public struct FDrawIndirectArguments
{
    public uint VertexCount;
    public uint InstanceCount;
    public uint FirstVertex;
    public uint FirstInstance;
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FDrawIndexedIndirectArguments")]
public struct FDrawIndexedIndirectArguments
{
    public uint IndexCount;
    public uint InstanceCount;
    public uint FirstIndex;
    public int  VertexOffset;
    public uint FirstInstance;
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FDispatchIndirectArguments")]
public struct FDispatchIndirectArguments
{
    public uint GroupX;
    public uint GroupY;
    public uint GroupZ;
}

// Specialization constant. Union mirrored with explicit offsets (uint64 forces 8-byte slots; sizeof 24).
[StructLayout(LayoutKind.Explicit, Size = 24)]
[NativeLayout("RHI::FSpecializationConstant")]
public struct FSpecializationConstant
{
    [FieldOffset(0)]  public uint ConstantID;
    [FieldOffset(8)]  public ulong AsInt;
    [FieldOffset(8)]  public float AsFloat;
    [FieldOffset(16)] public ESpecializationConstantType Type;

    public static FSpecializationConstant Int(uint id, long value)
        => new() { ConstantID = id, AsInt = (ulong)value, Type = ESpecializationConstantType.Int32 };
    public static FSpecializationConstant UInt(uint id, ulong value)
        => new() { ConstantID = id, AsInt = value, Type = ESpecializationConstantType.UInt32 };
    public static FSpecializationConstant Float(uint id, float value)
        => new() { ConstantID = id, AsFloat = value, Type = ESpecializationConstantType.Float32 };
    public static FSpecializationConstant Bool(uint id, bool value)
        => new() { ConstantID = id, AsInt = value ? 1u : 0u, Type = ESpecializationConstantType.Boolean };
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FRenderAttachment")]
public struct FRenderAttachment
{
    public FTextureH Texture;
    public FTextureH ResolveTexture;   // MSAA resolve target, optional
    public ELoadOp   LoadOp;
    public EStoreOp  StoreOp;
    public float     ClearR;
    public float     ClearG;
    public float     ClearB;
    public float     ClearA;

    public static FRenderAttachment Clear(FTextureH Texture, float R = 0, float G = 0, float B = 0, float A = 1)
        => new()
        {
            Texture = Texture, LoadOp = ELoadOp.Clear, StoreOp = EStoreOp.Store,
            ClearR = R, ClearG = G, ClearB = B, ClearA = A,
        };

    public static FRenderAttachment Load(FTextureH Texture)
        => new() { Texture = Texture, LoadOp = ELoadOp.Load, StoreOp = EStoreOp.Store };
}

// One allocation from RHI::Malloc. Cpu is null for EMemoryType.GPUOnly; both pointers name the same first byte.
// An address and the bytes a command may touch there.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FGPURange")]
public struct FGPURange
{
    public GPUPtr Address;
    public ulong  Size;

    public FGPURange(GPUPtr Address, ulong Size)
    {
        this.Address = Address;
        this.Size = Size;
    }

    public FGPURange Sub(ulong Offset, ulong Bytes) => new FGPURange(Address + Offset, Bytes);
    public FGPURange Skip(ulong Offset) => new FGPURange(Address + Offset, Size - Offset);
}

[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FGPUAllocation")]
public struct FGPUAllocation
{
    public IntPtr Cpu;   // std::byte*
    public GPUPtr Gpu;
    public ulong  Size;

    public bool IsValid => Gpu.IsValid;
}

// Per-frame transient GPU allocation from RHI::AllocTransient, a CPU pointer with its device address.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FTransientAlloc")]
public struct FTransientAlloc
{
    public IntPtr Cpu;   // void*
    public GPUPtr Gpu;
    public ulong  Size;

    public bool IsValid => Gpu.IsValid;

    public static implicit operator FGPURange(FTransientAlloc Alloc) => new FGPURange(Alloc.Gpu, Alloc.Size);
}

// One GPU memory heap (RHI::FGPUMemoryHeapStats). bools mirrored as byte.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FGPUMemoryHeapStats")]
public struct FGPUMemoryHeapStats
{
    public uint   HeapIndex;
    public byte   DeviceLocal;
    public byte   HostVisible;
    public byte   ReBAR;
    public ulong  BudgetBytes;
    public ulong  UsageBytes;
    public ulong  AllocatedBytes;
    public ulong  BlockBytes;
    public uint   BlockCount;
    public uint   AllocationCount;
}

// One occupied sampled slot in a texture heap (RHI::FHeapTextureInfo). Returned by RHI.GetTextureHeapTextures.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FHeapTextureInfo")]
public struct FHeapTextureInfo
{
    public uint         Slot;
    public FTextureDesc Desc;
}

// Process-wide GPU memory totals (the scalar part of RHI::FGPUMemoryStats; per-heap detail via RHI.GetMemoryHeap).
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("RHI::FGPUMemoryTotals")]
public struct FGPUMemoryTotals
{
    public ulong TotalBudget;
    public ulong TotalUsage;
    public ulong TotalAllocated;
    public ulong TotalBlockBytes;
    public uint  TotalAllocations;
    public uint  TotalBlocks;
    public uint  HeapCount;
}
