#pragma once

#include "Format.h"
#include "Containers/Array.h"
#include "Containers/SegmentArray.h"
#include "Containers/String.h"
#include "Containers/Tuple.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::RHI
{
    constexpr auto kDefaultAlign                = 16;
    
    constexpr auto kSamplerBindingSlot          = 0;
    constexpr auto kImageBindingSlot            = 1;
    constexpr auto kRWImageBindingSlot          = 2;

    constexpr auto kFramesInFlight              = 2;
    constexpr auto kMaxTextureHeapSize          = INT16_MAX;
    constexpr auto kMaxNumSamplers              = 4000;
    constexpr auto kMaxNumTextureHeaps          = 1024;
    constexpr auto kDedicatedMemoryThreshold    = 32u * 1024 * 1024;
    constexpr auto kInvalidHeapSlot             = ~0u;
    constexpr auto kMaxInlineWrite              = 65536u;

    constexpr auto kMeshWorkGroupSize           = 32u;   // MESHLET_MESH_GROUP_SIZE (MeshletGeometry.slang)
    constexpr auto kMeshMaxOutputVertices       = 64u;   // out vertices Verts[MESHLET_MAX_VERTICES]
    constexpr auto kMeshMaxOutputPrimitives     = 64u;   // out indices Tris[MESHLET_MAX_TRIANGLES]

    constexpr auto kTaskWorkGroupSize           = 32u;   // MeshletCullTask.slang [numthreads(32, 1, 1)]

    constexpr auto kMaxTaskPayloadBytes         = 4096u;
    
    struct FTexture;
    struct FTextureHeap;
    struct FPipeline;
    struct FSemaphore;
    struct FCommandList;
    struct FDepthStencilState;
    struct FSwapchain;
    struct FSurface;

    using GPUPtr            = uint64;
    using FPipelineH        = THandle<FPipeline>;
    using FTextureH         = THandle<FTexture>;
    using FTextureHeapH     = THandle<FTextureHeap>;
    using FSemaphoreH       = THandle<FSemaphore>;
    using FDepthStencilH    = THandle<FDepthStencilState>;
    using FCmdListH         = THandle<FCommandList>;
    using FSwapchainH       = THandle<FSwapchain>;
    using FSurfaceH         = THandle<FSurface>;
    using FDevice           = struct FDeviceImpl*;
    
    // @TODO Setup all platform agnostic dispatches
    struct FDispatchTable
    {
        
    };
    
    enum class EBackend : uint8
    {
        Vulkan,
        Metal,  // @TODO
        DX12,   // @TODO
        
        Default = Vulkan
    };
    
    enum class EMemoryType : uint8
    {
        CPUWrite,
        CPURead,
        GPUOnly,
        
        Default = CPUWrite
    };
    
    enum class EQueueType : uint8
    {
        Graphics,
        Transfer,
        Compute,
        
        Default = Graphics
    };
    
    enum class EStageFlags : uint16
    {
        None              = 0,
        IndirectArguments = BIT(1),
        Transfer          = BIT(2),
        Compute           = BIT(3),
        RasterColorOut    = BIT(4),
        PixelShader       = BIT(5),
        FragmentTests     = BIT(6),
        VertexShader      = BIT(7),
        Host              = BIT(8),
        AllCommands       = BIT(9),
        MeshShader        = BIT(10),
        TaskShader        = BIT(11),
    };
    
    ENUM_CLASS_FLAGS(EStageFlags);
    
    enum class EFrontFace : uint8
    {
        CCW,
        CW,
    };
    
    enum class ECullMode : uint8
    {
        Front,
        Back,
        None,
    };
    
    enum class ELoadOp : uint8
    {
        Undefined,
        Load,
        Clear,
    };
    
    enum class EStoreOp : uint8
    {
        Undefined,
        Store,
        Discard,
    };
    
    enum class EDepthFlags : uint8
    {
        None,
        Read = BIT(1),
        Write = BIT(2),
    };
    
    ENUM_CLASS_FLAGS(EDepthFlags)
    
    enum class EOp : uint8
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };
    
    enum class ETopology : uint8
    {
        TriangleList,
        TriangleStrip,
        LineList,
    };
    
    enum class EBlend : uint8
    {
        Add,
        Subtract,
        RevSubtract,
        Min,
        Max
    };
    
    enum class EFactor : uint8
    {
        Zero,
        One,
        SrcColor,
        OneMinusSrcColor,
        DstColor,
        OneMinusDstColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha,
    };

    enum class EIndexType : uint8
    {
        Uint32,
        Uint16,
    };

    enum class EFilter : uint8
    {
        Linear,
        Nearest,
    };

    enum class EAddressMode : uint8
    {
        Repeat,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    enum class EReduction : uint8
    {
        None,
        Min,
        Max,
    };
    
    enum class ETextureType : uint8
    {
        Tex1D,
        Tex2D,
        Tex3D,
        TexCube,
        Tex2DArray,
        TexCubeArray,
    };
    
    enum class EImageUsageFlags : uint16
    {
        None                = 0,
        Sampled             = BIT(1),
        Storage             = BIT(2),
        ColorAttachment     = BIT(3),
        DepthAttachment     = BIT(4),
        TransferSrc         = BIT(5),
        TransferDst         = BIT(6),
    };
    
    ENUM_CLASS_FLAGS(EImageUsageFlags);
    
    enum class ESpecializationConstantType : uint8 
    {
        UInt8,
        UInt16,
        UInt32,
        Int8,
        Int16,
        Int32,
        Boolean,
        Float32,
    };
    
    enum class EStencilOp : uint8
    {
        Keep,
        Zero,
        Replace,
        IncrementAndClamp,
        DecrementAndClamp,
        Invert,
        IncrementAndWrap,
        DecrementAndWrap
    };
    
    struct FShaderSource
    {
        TSpan<const std::byte>  Source;
        FStringView             EntryPoint;
    };
    
    struct FSpecializationConstant
    {
        uint32 ConstantID;
        union
        {
            uint64  AsInt;
            float   AsFloat;
            bool    AsBool;
        };
        
        ESpecializationConstantType Type;
        
    };
    
    struct FRect
    {
        int MinX, MaxX;
        int MinY, MaxY;
    };
    
    struct FSemaphoreInfo
    {
        FSemaphoreH Semaphore;
        uint64      Value;
        EStageFlags Stage;
    };
    
    struct FDrawIndirectArguments
    {
        uint32 VertexCount;
        uint32 InstanceCount;
        uint32 FirstVertex;
        uint32 FirstInstance;
    };

    struct FDrawIndexedIndirectArguments
    {
        uint32 IndexCount;
        uint32 InstanceCount;
        uint32 FirstIndex;
        int32  VertexOffset;
        uint32 FirstInstance;
    };

    struct FDispatchIndirectArguments
    {
        uint32 GroupX;
        uint32 GroupY;
        uint32 GroupZ;
    };

    // VkDrawMeshTasksIndirectCommandEXT mirror: one mesh/task workgroup grid per indirect draw.
    struct FDrawMeshTasksIndirectArguments
    {
        uint32 GroupCountX;
        uint32 GroupCountY;
        uint32 GroupCountZ;
    };

    struct FTextureDesc
    {
        ETextureType Type = ETextureType::Tex2D;
        FUIntVector3 Dimension = {};
        uint32 MipCount = 1;
        uint32 LayerCount = 1;
        uint32 SampleCount = 1;
        EFormat Format = EFormat::UNKNOWN;
        EImageUsageFlags Usage = EImageUsageFlags::None;
    };
    
    struct FBlendDesc
    {
        bool    bBlendEnable    = false;
        EBlend  ColorOp         = EBlend::Add;
        EFactor SrcColorFactor  = EFactor::One;
        EFactor DstColorFactor  = EFactor::Zero;
        EBlend  AlphaOp         = EBlend::Add;
        EFactor SrcAlphaFactor  = EFactor::One;
        EFactor DstAlphaFactor  = EFactor::Zero;
        uint8   ColorWriteMask  = 0xF;
    };

    struct FSamplerDesc
    {
        EFilter         MinFilter       = EFilter::Linear;
        EFilter         MagFilter       = EFilter::Linear;
        EFilter         MipFilter       = EFilter::Linear;
        EAddressMode    AddressU        = EAddressMode::Repeat;
        EAddressMode    AddressV        = EAddressMode::Repeat;
        EAddressMode    AddressW        = EAddressMode::Repeat;
        float           MaxAnisotropy   = 1.0f;
        float           MipBias         = 0.0f;
        EOp             CompareOp       = EOp::Never; // Never = compare disabled
        EReduction      Reduction       = EReduction::None;
    };

    // Extent of 0 = full mip extent.
    struct FTextureSlice
    {
        uint32          Mip         = 0;
        uint32          Layer       = 0;
        uint32          LayerCount  = 1;
        FUIntVector3    Offset      = {};
        FUIntVector3    Extent      = {};
    };
    
    struct FStencil
    {
        EOp         Test = EOp::Always;
        EStencilOp  FailOp = EStencilOp::Keep;
        EStencilOp  PassOp = EStencilOp::Keep;
        EStencilOp  DepthFailOp = EStencilOp::Keep;
        uint8       Reference = 0;
    };
    
    struct FDepthStencilDesc
    {
        EDepthFlags DepthMode = EDepthFlags::None;
        EOp         DepthTest = EOp::Always;
        float       DepthBias = 0.0f;
        float       DepthBiasSlopeFactor = 0.0f;
        float       DepthBiasClamp = 0.0f;
        uint8       StencilReadMask = 0xFF;
        uint8       StencilWriteMask = 0xFF;
        FStencil    StencilFront;
        FStencil    StencilBack;
    };
    
    struct FColorTarget
    {
        EFormat     Format;
        FBlendDesc  Blend;
    };
    
    struct FRasterDesc
    {
        ETopology   Topology = ETopology::TriangleList;
        bool        bAlphaToCoverage = false;
        bool        bWireframe = false;
        uint8       SampleCount = 1;
        EFormat     DepthFormat = EFormat::UNKNOWN;
        EFormat     StencilFormat = EFormat::UNKNOWN;
        TSpan<const FColorTarget> ColorTargets;
    };
    
    struct FRenderAttachment
    {
        THandle<FTexture>   Texture;
        THandle<FTexture>   ResolveTexture; // MSAA resolve target, optional.
        ELoadOp             LoadOp  = ELoadOp::Undefined;
        EStoreOp            StoreOp = EStoreOp::Store;
        float               Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };
    
    struct FRenderPassDesc
    {
        TSpan<const FRenderAttachment>  ColorAttachments;
        FRenderAttachment               DepthAttachment;
        FRenderAttachment               StencilAttachment;
        FUIntVector2                    RenderArea;
    };
    
    template<typename T>
    constexpr bool IsValid(THandle<T> Handle)
    {
        return Handle.Handle != 0;
    }

    struct FDeviceDesc
    {
        bool bValidation    = false;
        bool bDebugUtils    = true;
        bool bGpuValidation = false;
    };

    // API-neutral GPU device summary, surfaced to tools.
    struct FGPUDeviceInfo
    {
        FString Name;       // Adapter name, e.g. "NVIDIA GeForce RTX 4080".
        FString APIName;    // Backend + version, e.g. "Vulkan 1.4.250".
        uint32  VendorID = 0;   // PCI vendor ID: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel.
        bool    bDiscrete = false;
    };

    struct FGPUMemoryHeapStats
    {
        uint32 HeapIndex       = 0;
        bool   bDeviceLocal    = false;
        bool   bHostVisible    = false;   // OR of all memory types pointing at this heap.
        bool   bReBAR          = false;   // CPU-writable VRAM (DeviceLocal+HostVisible) larger than the legacy 256MB BAR window.
        uint64 BudgetBytes     = 0;
        uint64 UsageBytes      = 0;
        uint64 AllocatedBytes  = 0;
        uint64 BlockBytes      = 0;
        uint32 BlockCount      = 0;
        uint32 AllocationCount = 0;
    };

    struct FGPUMemoryStats
    {
        uint64 TotalBudget      = 0;
        uint64 TotalUsage       = 0;
        uint64 TotalAllocated   = 0;
        uint64 TotalBlockBytes  = 0;
        uint32 TotalAllocations = 0;
        uint32 TotalBlocks      = 0;
        TFixedVector<FGPUMemoryHeapStats, 16> Heaps;   // VK_MAX_MEMORY_HEAPS == 16.
    };

    // Full per-heap GPU memory breakdown.
    RUNTIME_API void           GetGPUMemoryStats(FGPUMemoryStats& Out);
    RUNTIME_API FGPUDeviceInfo GetDeviceInfo();

    RUNTIME_API uint64         ClampCPUWriteSlice(const char* RingName, uint64 DesiredSliceSize, uint32 SliceCount);

    // maxTaskWorkGroupCount[0]. A meshlet draw's grid is one task workgroup per cull block, and a region
    // large enough to exceed this would otherwise be dispatched as-is.
    RUNTIME_API uint32         GetMaxTaskWorkGroupCount();

    RUNTIME_API bool           SupportsAsyncCompute();
    RUNTIME_API bool           SupportsAsyncTransfer();

    RUNTIME_API void        CreateDevice(const FDeviceDesc& Desc = {});
    RUNTIME_API void        FreeDevice();
    RUNTIME_API void        TickFrame();
    RUNTIME_API void        WaitDeviceIdle();
    RUNTIME_API uint64      GetSemaphoreValue(FSemaphoreH Semaphore);
    RUNTIME_API void        WaitSemaphore(FSemaphoreH Semaphore, uint64 Value);

    RUNTIME_API void        HandleDeviceLost();

    class ICrashTracker;
    RUNTIME_API ICrashTracker& GetCrashTracker();

    RUNTIME_API GPUPtr      Malloc(uint64 Size, uint64 Alignment = kDefaultAlign, EMemoryType Type = EMemoryType::Default);
    RUNTIME_API GPUPtr      Malloc(uint64 Size, EMemoryType Type);
    RUNTIME_API void*       ToHost(GPUPtr GPU);

    RUNTIME_API void        SetDebugName(GPUPtr GPU, const char* Name);
    RUNTIME_API void        SetDebugName(FTextureH Texture, const char* Name);
    RUNTIME_API void        Free(GPUPtr GPU);
    RUNTIME_API void        FreeH(FSemaphoreH Semaphore);
    RUNTIME_API void        FreeH(FPipelineH Pipeline);
    RUNTIME_API void        FreeH(FTextureH Texture);
    RUNTIME_API void        FreeH(FTextureHeapH Heap);
    RUNTIME_API void        FreeH(FDepthStencilH DepthStencil);
    RUNTIME_API void        FreeH(FSwapchainH Swapchain);
    RUNTIME_API void        FreeH(FSurfaceH Surface);

    RUNTIME_API FSurfaceH    CreateSurface(void* WindowHandle);
    RUNTIME_API FSwapchainH  CreateSwapchain(FSurfaceH Surface, const FUIntVector2& Extent);
    RUNTIME_API void         RecreateSwapchain(FSwapchainH Swapchain, const FUIntVector2& Extent);
    
    RUNTIME_API void         SetVSync(bool bEnabled);
    RUNTIME_API bool         GetVSync();
    RUNTIME_API FTextureH    AcquireNextImage(FSwapchainH Swapchain);   // invalid handle if out-of-date this call
    RUNTIME_API FUIntVector2 GetSwapchainExtent(FSwapchainH Swapchain);
    RUNTIME_API EFormat      GetSwapchainFormat(FSwapchainH Swapchain);
    RUNTIME_API void         CmdSwapchainBarrierToRender(FCmdListH CL, FSwapchainH Swapchain);
    RUNTIME_API bool         PresentSwapchain(FSwapchainH Swapchain, FCmdListH FinalCommandList, FSemaphoreH FrameSignal, uint64 FrameSignalValue);

    struct FPipelineStat
    {
        FString Stage;              // executable name, e.g. "Fragment Shader" -- one per compiled stage
        FString Name;               // driver's statistic name
        double  Value    = 0.0;     // union normalized to double; bools arrive as 0/1
        bool    bIsFloat = false;   // false -> render as an integer
    };

    RUNTIME_API bool            GetPipelineStatistics(FPipelineH Pipeline, TVector<FPipelineStat>& Out);

    RUNTIME_API FDepthStencilH  CreateDepthStencil(const FDepthStencilDesc& Desc);
    RUNTIME_API FPipelineH      CreateGraphicsPipeline(const FShaderSource& Vertex, const FShaderSource& Fragment, const FRasterDesc& Desc, TSpan<const FSpecializationConstant> Constants = {});
    RUNTIME_API FPipelineH      CreateComputePipeline(const FShaderSource& Compute, TSpan<const FSpecializationConstant> Constants = {});
    RUNTIME_API FPipelineH      CreateMeshShaderPipeline(const FShaderSource& Task, const FShaderSource& Mesh, const FShaderSource& Fragment, const FRasterDesc& Desc, TSpan<const FSpecializationConstant> Constants = {});
    RUNTIME_API FSemaphoreH     CreateTimelineSemaphore(uint64 Value);
    RUNTIME_API FTextureH       CreateTexture(const FTextureDesc& Desc, GPUPtr Location = 0);
    RUNTIME_API FTextureHeapH   CreateTextureHeap(uint32 TextureCount, uint32 RWTextureCount, uint32 SamplerCount);

    RUNTIME_API FTextureDesc    GetTextureDesc(FTextureH Texture);

    RUNTIME_API uint32      HeapWriteTexture(FTextureHeapH Heap, FTextureH Texture);
    RUNTIME_API void        HeapRepointTexture(FTextureHeapH Heap, uint32 Slot, FTextureH Texture);
    RUNTIME_API uint32      HeapWriteRWTexture(FTextureHeapH Heap, FTextureH Texture, uint32 Mip = 0);
    RUNTIME_API uint32      HeapWriteSampler(FTextureHeapH Heap, const FSamplerDesc& Desc);
    RUNTIME_API void        HeapSetFallbackTexture(FTextureHeapH Heap, FTextureH Texture);
    RUNTIME_API void        HeapFreeTexture(FTextureHeapH Heap, uint32 Slot);
    RUNTIME_API void        HeapFreeRWTexture(FTextureHeapH Heap, uint32 Slot);
    RUNTIME_API void        HeapFreeSampler(FTextureHeapH Heap, uint32 Slot);

    // Debug introspection: every occupied sampled slot in the heap.
    struct FHeapTextureInfo
    {
        uint32       Slot;
        FTextureDesc Desc;
    };
    RUNTIME_API void        GetTextureHeapTextures(FTextureHeapH Heap, TVector<FHeapTextureInfo>& OutTextures);

    RUNTIME_API FCmdListH   OpenCommandList(EQueueType Type = EQueueType::Default);
    RUNTIME_API void        ResetCommandList(FCmdListH CommandList);
    RUNTIME_API void        Submit(EQueueType Queue, TSpan<const FCmdListH> CommandLists, TSpan<const FSemaphoreInfo> Waits = {}, TSpan<const FSemaphoreInfo> Signals = {});
    RUNTIME_API void        Submit(FCmdListH CommandList, EQueueType Type = EQueueType::Default);
    
    RUNTIME_API void        SubmitAndWait(FCmdListH CommandList);

    RUNTIME_API void        CmdMemcpy(FCmdListH CL, GPUPtr Dest, GPUPtr Source, size_t Size);
    RUNTIME_API void        CmdMemset(FCmdListH CL, GPUPtr Dest, uint64 Size, uint32 Value);
    RUNTIME_API void        CmdMemzero(FCmdListH CL, GPUPtr Dest, uint64 Size);
    RUNTIME_API void        CmdWriteMemory(FCmdListH CL, GPUPtr Dest, const void* Data, uint64 Size);

    RUNTIME_API void        CmdCopyTexture(FCmdListH CL, FTextureH Source, const FTextureSlice& SourceSlice, FTextureH Dest, const FTextureSlice& DestSlice);
    RUNTIME_API void        CmdCopyMemoryToTexture(FCmdListH CL, GPUPtr Source, uint32 RowLength, FTextureH Dest, const FTextureSlice& Slice = {});
    RUNTIME_API void        CmdCopyTextureToMemory(FCmdListH CL, FTextureH Source, const FTextureSlice& Slice, GPUPtr Dest, uint32 RowLength = 0);
    RUNTIME_API void        CmdBlitTexture(FCmdListH CL, FTextureH Source, const FTextureSlice& SourceSlice, FTextureH Dest, const FTextureSlice& DestSlice, EFilter Filter = EFilter::Linear);
    RUNTIME_API void        CmdResolveTexture(FCmdListH CL, FTextureH Source, FTextureH Dest);
    RUNTIME_API void        CmdClearTexture(FCmdListH CL, FTextureH Texture, const float Value[4]);
    RUNTIME_API void        CmdClearTextureUInt(FCmdListH CL, FTextureH Texture, const uint32 Value[4]);

    RUNTIME_API void        CmdBarrier(FCmdListH CL, EStageFlags Before, EStageFlags After);

    RUNTIME_API void        CmdImageBarrier(FCmdListH CL, FTextureH Texture, EStageFlags Before, EStageFlags After);

    RUNTIME_API void        CmdReleaseImageToQueue(FCmdListH CL, FTextureH Texture, EQueueType Dest, EStageFlags Before);
    RUNTIME_API void        CmdAcquireImageFromQueue(FCmdListH CL, FTextureH Texture, EQueueType Source, EStageFlags After);

    // Canonical pipeline-stage barriers.
    namespace Barriers
    {
        inline void ComputeToAll(FCmdListH CL)
        {
            CmdBarrier(CL,
                EStageFlags::Compute,
                EStageFlags::Compute | EStageFlags::VertexShader | EStageFlags::PixelShader |
                EStageFlags::MeshShader | EStageFlags::TaskShader |
                EStageFlags::IndirectArguments | EStageFlags::FragmentTests | EStageFlags::Transfer);
        }

        // MeshShader/TaskShader belong in the SOURCE mask: a task shader that culls also writes.
        inline void RasterToRead(FCmdListH CL)
        {
            CmdBarrier(CL,
                EStageFlags::RasterColorOut | EStageFlags::FragmentTests |
                EStageFlags::MeshShader | EStageFlags::TaskShader,
                EStageFlags::PixelShader | EStageFlags::VertexShader | EStageFlags::Compute |
                EStageFlags::MeshShader | EStageFlags::TaskShader |
                EStageFlags::IndirectArguments |
                EStageFlags::RasterColorOut | EStageFlags::FragmentTests);
        }

        inline void RasterToRaster(FCmdListH CL)
        {
            CmdBarrier(CL,
                EStageFlags::RasterColorOut | EStageFlags::FragmentTests,
                EStageFlags::RasterColorOut | EStageFlags::FragmentTests);
        }

        inline void TransferToAll(FCmdListH CL)
        {
            CmdBarrier(CL, EStageFlags::Transfer, EStageFlags::AllCommands);
        }
        
        inline void TransferToTransfer(FCmdListH CL)
        {
            CmdBarrier(CL, EStageFlags::Transfer, EStageFlags::Transfer);
        }

        inline void AllToTransfer(FCmdListH CL)
        {
            CmdBarrier(CL, EStageFlags::AllCommands, EStageFlags::Transfer);
        }

        /** Uploads/clears feeding the GPU scene: consumed by the cull dispatches and the geometry front end. */
        // Narrow variants: use only where every reader of every buffer written is in the destination.
        inline void TransferToCompute(FCmdListH CL)
        {
            CmdBarrier(CL, EStageFlags::Transfer,
                EStageFlags::Compute | EStageFlags::TaskShader | EStageFlags::MeshShader);
        }

        /** A cull dispatch whose output feeds later dispatches, the task/mesh stages, and indirect fetch. */
        inline void ComputeToGeometry(FCmdListH CL)
        {
            CmdBarrier(CL, EStageFlags::Compute,
                EStageFlags::Compute | EStageFlags::TaskShader | EStageFlags::MeshShader |
                EStageFlags::IndirectArguments);
        }

        /** A dispatch that writes nothing but indirect arguments. */
        inline void ComputeToIndirect(FCmdListH CL)
        {
            CmdBarrier(CL, EStageFlags::Compute, EStageFlags::IndirectArguments);
        }
    }

    RUNTIME_API void        CmdBeginRenderPass(FCmdListH CL, const FRenderPassDesc& Desc);
    RUNTIME_API void        CmdEndRenderPass(FCmdListH CL);

    RUNTIME_API void        CmdSetTextureHeap(FCmdListH CL, FTextureHeapH Heap);

    RUNTIME_API void        CmdSetDepthStencilState(FCmdListH CL, FDepthStencilH DepthStencil);
    RUNTIME_API void        CmdSetFrontFace(FCmdListH CL, EFrontFace Front);
    RUNTIME_API void        CmdSetCullMode(FCmdListH CL, ECullMode Mode);
    RUNTIME_API void        CmdSetLineWidth(FCmdListH CL, float Width);

    RUNTIME_API void        CmdSetPipeline(FCmdListH CL, FPipelineH Pipeline);

    RUNTIME_API void        CmdSetScissor(FCmdListH CL, const FRect& Rect);
    RUNTIME_API void        CmdSetViewport(FCmdListH CL, const FRect& Rect);

    RUNTIME_API void        CmdSetIndexBuffer(FCmdListH CL, GPUPtr IndexBuffer, uint32 Offset = 0, EIndexType IndexType = EIndexType::Uint32);

    RUNTIME_API void        CmdDispatch(FCmdListH CL, GPUPtr DrawArgs, uint32 GroupX, uint32 GroupY, uint32 GroupZ);
    RUNTIME_API void        CmdDispatchIndirect(FCmdListH CL, GPUPtr DrawArgs, uint32 Offset = 0);

    RUNTIME_API void        CmdDraw(FCmdListH CL, GPUPtr DrawArgs, uint32 VertexCount, uint32 InstanceCount, uint32 FirstVertex, uint32 FirstInstance);
    RUNTIME_API void        CmdDrawIndexed(FCmdListH CL, GPUPtr IndexBuffer, uint32 IndexOffset, GPUPtr DrawArgs, uint32 IndexCount, uint32 InstanceCount, uint32 FirstIndex, int32 VertexOffset, uint32 FirstInstance, EIndexType IndexType = EIndexType::Uint32);
    RUNTIME_API void        CmdDrawIndirect(FCmdListH CL, GPUPtr DrawArgs, uint32 Offset, uint32 DrawCount, uint32 Stride);
    RUNTIME_API void        CmdDrawIndexedIndirect(FCmdListH CL, GPUPtr DrawArgs, uint32 Offset, uint32 DrawCount, uint32 Stride);
    
    RUNTIME_API void        CmdDrawIndirect(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint32 Offset, uint32 DrawCount, uint32 Stride);
    RUNTIME_API void        CmdDispatchIndirect(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint32 Offset);

    RUNTIME_API void        CmdDrawMeshTasks(FCmdListH CL, GPUPtr DrawArgs, uint32 GroupCountX, uint32 GroupCountY, uint32 GroupCountZ);
    RUNTIME_API void        CmdDrawMeshTasksIndirect(FCmdListH CL, GPUPtr DrawArgs, GPUPtr IndirectBuffer, uint32 Offset, uint32 DrawCount, uint32 Stride);
    RUNTIME_API void        CmdDrawMeshTasksIndirectCount(FCmdListH CL, GPUPtr DrawArgs, GPUPtr IndirectBuffer, uint32 Offset, GPUPtr CountBuffer, uint32 CountOffset, uint32 MaxDrawCount, uint32 Stride);

    RUNTIME_API void        CmdBeginMarker(FCmdListH CL, const char* Name);
    RUNTIME_API void        CmdEndMarker(FCmdListH CL);

    template<typename T>
    TTuple<T*, GPUPtr> New(uint64 Count = 1)
    {
        GPUPtr GPU = Malloc(Count * sizeof(T), alignof(T));
        T* Host = static_cast<T*>(ToHost(GPU));
        std::uninitialized_value_construct_n(Host, Count);
        return {Host, GPU};
    }

    template<typename T>
    class TUniqueH
    {
    public:

        TUniqueH() = default;
        TUniqueH(THandle<T> InHandle) : Handle(InHandle) {}
        TUniqueH(const TUniqueH&) = delete;
        TUniqueH& operator=(const TUniqueH&) = delete;
        TUniqueH(TUniqueH&& Other) noexcept : Handle(Other.Release()) {}

        TUniqueH& operator=(TUniqueH&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset(Other.Release());
            }
            return *this;
        }

        TUniqueH& operator=(THandle<T> InHandle)
        {
            Reset(InHandle);
            return *this;
        }

        ~TUniqueH()
        {
            Reset();
        }

        void Reset(THandle<T> InHandle = {})
        {
            if (IsValid(Handle))
            {
                FreeH(Handle);
            }
            Handle = InHandle;
        }

        THandle<T> Release()
        {
            THandle<T> Out = Handle;
            Handle = {};
            return Out;
        }

        THandle<T> Get() const { return Handle; }
        operator THandle<T>() const { return Handle; }
        explicit operator bool() const { return IsValid(Handle); }

    private:

        THandle<T> Handle = {};
    };

    using FPipelineUH       = TUniqueH<FPipeline>;
    using FTextureUH        = TUniqueH<FTexture>;
    using FTextureHeapUH    = TUniqueH<FTextureHeap>;
    using FSemaphoreUH      = TUniqueH<FSemaphore>;
    using FDepthStencilUH   = TUniqueH<FDepthStencilState>;

    class FUniqueGPUPtr
    {
    public:

        FUniqueGPUPtr() = default;
        explicit FUniqueGPUPtr(GPUPtr InPtr) : Ptr(InPtr) {}
        FUniqueGPUPtr(const FUniqueGPUPtr&) = delete;
        FUniqueGPUPtr& operator=(const FUniqueGPUPtr&) = delete;
        FUniqueGPUPtr(FUniqueGPUPtr&& Other) noexcept : Ptr(Other.Release()) {}

        FUniqueGPUPtr& operator=(FUniqueGPUPtr&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset(Other.Release());
            }
            return *this;
        }

        ~FUniqueGPUPtr()
        {
            Reset();
        }

        void Reset(GPUPtr InPtr = 0)
        {
            if (Ptr != 0)
            {
                Free(Ptr);
            }
            Ptr = InPtr;
        }

        GPUPtr Release()
        {
            GPUPtr Out = Ptr;
            Ptr = 0;
            return Out;
        }

        GPUPtr Get() const { return Ptr; }
        operator GPUPtr() const { return Ptr; }
        explicit operator bool() const { return Ptr != 0; }

    private:

        GPUPtr Ptr = 0;
    };

}
