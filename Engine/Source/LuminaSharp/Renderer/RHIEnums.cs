using System;

namespace LuminaSharp.Rendering;

// 1:1 mirrors of the engine RHI enums (Runtime/Renderer/RHI.h + Format.h). Underlying types match C++ exactly
// so these embed correctly inside the blittable descriptor structs; when passed as a bare argument the binding
// layer widens them to int (the thunk ABI). Keep values/order in lockstep with the native enums.

public enum EBackend : byte { Vulkan, Metal, DX12, Default = Vulkan }

public enum EMemoryType : byte { CPUWrite, CPURead, GPUOnly, Default = CPUWrite }

public enum EQueueType : byte { Graphics, Transfer, Compute, Default = Graphics }

[Flags]
public enum EStageFlags : ushort
{
    None              = 0,
    IndirectArguments = 1 << 1,
    Transfer          = 1 << 2,
    Compute           = 1 << 3,
    RasterColorOut    = 1 << 4,
    PixelShader       = 1 << 5,
    FragmentTests     = 1 << 6,
    VertexShader      = 1 << 7,
    Host              = 1 << 8,
    AllCommands       = 1 << 9,
}

public enum EFrontFace : byte { CCW, CW }

public enum ECullMode : byte { Front, Back, None }

public enum ELoadOp : byte { Undefined, Load, Clear }

public enum EStoreOp : byte { Undefined, Store, Discard }

[Flags]
public enum EDepthFlags : byte { None = 0, Read = 1 << 1, Write = 1 << 2 }

public enum EOp : byte { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always }

public enum ETopology : byte { TriangleList, TriangleStrip, LineList }

public enum EBlend : byte { Add, Subtract, RevSubtract, Min, Max }

public enum EFactor : byte
{
    Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
}

public enum EIndexType : byte { Uint32, Uint16 }

public enum EFilter : byte { Linear, Nearest }

public enum EAddressMode : byte { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder }

public enum EReduction : byte { None, Min, Max }

public enum ETextureType : byte { Tex1D, Tex2D, Tex3D, TexCube, Tex2DArray, TexCubeArray }

[Flags]
public enum EImageUsageFlags : ushort
{
    None            = 0,
    Sampled         = 1 << 1,
    Storage         = 1 << 2,
    ColorAttachment = 1 << 3,
    DepthAttachment = 1 << 4,
    TransferSrc     = 1 << 5,
    TransferDst     = 1 << 6,
}

public enum ESpecializationConstantType : byte
{
    UInt8, UInt16, UInt32, Int8, Int16, Int32, Boolean, Float32,
}

public enum EStencilOp : byte
{
    Keep, Zero, Replace, IncrementAndClamp, DecrementAndClamp, Invert, IncrementAndWrap, DecrementAndWrap,
}

// How a sampled view presents a texture with fewer than four channels. Mirrors Format.h ETextureSwizzle.
public enum ETextureSwizzle : byte { Identity, Grayscale, Alpha }

// Texture/render format. Mirrors Format.h EFormat (uint8) exactly.
public enum EFormat : byte
{
    UNKNOWN,

    R8_UINT, R8_SINT, R8_UNORM, R8_SNORM,
    RG8_UINT, RG8_SINT, RG8_UNORM, RG8_SNORM,
    R16_UINT, R16_SINT, R16_UNORM, R16_SNORM, R16_FLOAT,
    BGRA4_UNORM, B5G6R5_UNORM, B5G5R5A1_UNORM,
    RGBA8_UINT, RGBA8_SINT, RGBA8_UNORM, RGBA8_SNORM, BGRA8_UNORM, SRGBA8_UNORM, SBGRA8_UNORM,
    R10G10B10A2_UNORM, R11G11B10_FLOAT,
    RG16_UINT, RG16_SINT, RG16_UNORM, RG16_SNORM, RG16_FLOAT,
    R32_UINT, R32_SINT, R32_FLOAT,
    RGBA16_UINT, RGBA16_SINT, RGBA16_FLOAT, RGBA16_UNORM, RGBA16_SNORM,
    RG32_UINT, RG32_SINT, RG32_FLOAT,
    RGB32_UINT, RGB32_SINT, RGB32_FLOAT,
    RGBA32_UINT, RGBA32_SINT, RGBA32_FLOAT,

    D16, D24S8, X24G8_UINT, D32, D32S8, X32G8_UINT,

    BC1_UNORM, BC1_UNORM_SRGB, BC2_UNORM, BC2_UNORM_SRGB, BC3_UNORM, BC3_UNORM_SRGB,
    BC4_UNORM, BC4_SNORM, BC5_UNORM, BC5_SNORM, BC6H_UFLOAT, BC6H_SFLOAT, BC7_UNORM, BC7_UNORM_SRGB,

    COUNT,
}
