#include "Containers/HandleAllocator.h"
#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "Core/Templates/LuminaTemplate.h"

#define VOLK_IMPLEMENTATION
#include <volk/volk.h>

#include "Core/CommandLine/CommandLine.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Threading/Atomic.h"
#include "Core/Windows/GLFWInclude.h"
#include "Memory/SmartPtr.h"
#include "Memory/Allocators/Allocator.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/API/Vulkan/VulkanMacros.h"
#include "Renderer/RHINative.h"
#include "Renderer/RenderDocImpl.h"
#include "Renderer/ErrorHandling/Vulkan/VulkanCrashTracker.h"
#include "Renderer/ErrorHandling/Vulkan/VulkanBreadcrumbs.h"
#include "Platform/CrashHandler.h"
#include "Platform/CrashReporter.h"
#include "Tools/Dialogs/Dialogs.h"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "tracy/TracyVulkan.hpp"
#include "Log/Log.h"
#include "Core/Profiler/Profile.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    struct FormatMapping
    {
        EFormat  Format;
        VkFormat vkFormat;
    };

    static const TArray<FormatMapping, static_cast<size_t>(EFormat::COUNT)> FormatMap = { {
        { EFormat::UNKNOWN,           VK_FORMAT_UNDEFINED                },
        { EFormat::R8_UINT,           VK_FORMAT_R8_UINT                  },
        { EFormat::R8_SINT,           VK_FORMAT_R8_SINT                  },
        { EFormat::R8_UNORM,          VK_FORMAT_R8_UNORM                 },
        { EFormat::R8_SNORM,          VK_FORMAT_R8_SNORM                 },
        { EFormat::RG8_UINT,          VK_FORMAT_R8G8_UINT                },
        { EFormat::RG8_SINT,          VK_FORMAT_R8G8_SINT                },
        { EFormat::RG8_UNORM,         VK_FORMAT_R8G8_UNORM               },
        { EFormat::RG8_SNORM,         VK_FORMAT_R8G8_SNORM               },
        { EFormat::R16_UINT,          VK_FORMAT_R16_UINT                 },
        { EFormat::R16_SINT,          VK_FORMAT_R16_SINT                 },
        { EFormat::R16_UNORM,         VK_FORMAT_R16_UNORM                },
        { EFormat::R16_SNORM,         VK_FORMAT_R16_SNORM                },
        { EFormat::R16_FLOAT,         VK_FORMAT_R16_SFLOAT               },
        { EFormat::BGRA4_UNORM,       VK_FORMAT_B4G4R4A4_UNORM_PACK16    },
        { EFormat::B5G6R5_UNORM,      VK_FORMAT_B5G6R5_UNORM_PACK16      },
        { EFormat::B5G5R5A1_UNORM,    VK_FORMAT_B5G5R5A1_UNORM_PACK16    },
        { EFormat::RGBA8_UINT,        VK_FORMAT_R8G8B8A8_UINT            },
        { EFormat::RGBA8_SINT,        VK_FORMAT_R8G8B8A8_SINT            },
        { EFormat::RGBA8_UNORM,       VK_FORMAT_R8G8B8A8_UNORM           },
        { EFormat::RGBA8_SNORM,       VK_FORMAT_R8G8B8A8_SNORM           },
        { EFormat::BGRA8_UNORM,       VK_FORMAT_B8G8R8A8_UNORM           },
        { EFormat::SRGBA8_UNORM,      VK_FORMAT_R8G8B8A8_SRGB            },
        { EFormat::SBGRA8_UNORM,      VK_FORMAT_B8G8R8A8_SRGB            },
        { EFormat::R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
        { EFormat::R11G11B10_FLOAT,   VK_FORMAT_B10G11R11_UFLOAT_PACK32  },
        { EFormat::RG16_UINT,         VK_FORMAT_R16G16_UINT              },
        { EFormat::RG16_SINT,         VK_FORMAT_R16G16_SINT              },
        { EFormat::RG16_UNORM,        VK_FORMAT_R16G16_UNORM             },
        { EFormat::RG16_SNORM,        VK_FORMAT_R16G16_SNORM             },
        { EFormat::RG16_FLOAT,        VK_FORMAT_R16G16_SFLOAT            },
        { EFormat::R32_UINT,          VK_FORMAT_R32_UINT                 },
        { EFormat::R32_SINT,          VK_FORMAT_R32_SINT                 },
        { EFormat::R32_FLOAT,         VK_FORMAT_R32_SFLOAT               },
        { EFormat::RGBA16_UINT,       VK_FORMAT_R16G16B16A16_UINT        },
        { EFormat::RGBA16_SINT,       VK_FORMAT_R16G16B16A16_SINT        },
        { EFormat::RGBA16_FLOAT,      VK_FORMAT_R16G16B16A16_SFLOAT      },
        { EFormat::RGBA16_UNORM,      VK_FORMAT_R16G16B16A16_UNORM       },
        { EFormat::RGBA16_SNORM,      VK_FORMAT_R16G16B16A16_SNORM       },
        { EFormat::RG32_UINT,         VK_FORMAT_R32G32_UINT              },
        { EFormat::RG32_SINT,         VK_FORMAT_R32G32_SINT              },
        { EFormat::RG32_FLOAT,        VK_FORMAT_R32G32_SFLOAT            },
        { EFormat::RGB32_UINT,        VK_FORMAT_R32G32B32_UINT           },
        { EFormat::RGB32_SINT,        VK_FORMAT_R32G32B32_SINT           },
        { EFormat::RGB32_FLOAT,       VK_FORMAT_R32G32B32_SFLOAT         },
        { EFormat::RGBA32_UINT,       VK_FORMAT_R32G32B32A32_UINT        },
        { EFormat::RGBA32_SINT,       VK_FORMAT_R32G32B32A32_SINT        },
        { EFormat::RGBA32_FLOAT,      VK_FORMAT_R32G32B32A32_SFLOAT      },
        { EFormat::D16,               VK_FORMAT_D16_UNORM                },
        { EFormat::D24S8,             VK_FORMAT_D24_UNORM_S8_UINT        },
        { EFormat::X24G8_UINT,        VK_FORMAT_D24_UNORM_S8_UINT        },
        { EFormat::D32,               VK_FORMAT_D32_SFLOAT               },
        { EFormat::D32S8,             VK_FORMAT_D32_SFLOAT_S8_UINT       },
        { EFormat::X32G8_UINT,        VK_FORMAT_D32_SFLOAT_S8_UINT       },
        { EFormat::BC1_UNORM,         VK_FORMAT_BC1_RGBA_UNORM_BLOCK     },
        { EFormat::BC1_UNORM_SRGB,    VK_FORMAT_BC1_RGBA_SRGB_BLOCK      },
        { EFormat::BC2_UNORM,         VK_FORMAT_BC2_UNORM_BLOCK          },
        { EFormat::BC2_UNORM_SRGB,    VK_FORMAT_BC2_SRGB_BLOCK           },
        { EFormat::BC3_UNORM,         VK_FORMAT_BC3_UNORM_BLOCK          },
        { EFormat::BC3_UNORM_SRGB,    VK_FORMAT_BC3_SRGB_BLOCK           },
        { EFormat::BC4_UNORM,         VK_FORMAT_BC4_UNORM_BLOCK          },
        { EFormat::BC4_SNORM,         VK_FORMAT_BC4_SNORM_BLOCK          },
        { EFormat::BC5_UNORM,         VK_FORMAT_BC5_UNORM_BLOCK          },
        { EFormat::BC5_SNORM,         VK_FORMAT_BC5_SNORM_BLOCK          },
        { EFormat::BC6H_UFLOAT,       VK_FORMAT_BC6H_UFLOAT_BLOCK        },
        { EFormat::BC6H_SFLOAT,       VK_FORMAT_BC6H_SFLOAT_BLOCK        },
        { EFormat::BC7_UNORM,         VK_FORMAT_BC7_UNORM_BLOCK          },
        { EFormat::BC7_UNORM_SRGB,    VK_FORMAT_BC7_SRGB_BLOCK           },
    } };

    static VkFormat ConvertFormat(EFormat Format)
    {
        DEBUG_ASSERT(Format < EFormat::COUNT);
        DEBUG_ASSERT(FormatMap[static_cast<uint32>(Format)].Format == Format);

        return FormatMap[static_cast<uint32>(Format)].vkFormat;
    }

    static VkImageAspectFlags GuessImageAspectFlags(VkFormat Format)
    {
        switch (Format)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;

        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;

        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }
}

namespace Lumina::Vulkan
{
    void ShowVulkanCheckFailureDialog(const FString& Expr, const char* File, int Line, const FString& ResultString)
    {
        FString Body = "Vulkan call failed:\n\n";
        Body += Expr;
        Body += "\n\nLocation: ";
        Body += File;
        Body += ":";
        Body += Lumina::Format("{}", Line).c_str();
        Body += "\n\n";
        Body += ResultString;
        Dialogs::ShowInternal(Dialogs::ESeverity::FatalError, Dialogs::EType::Ok, "Vulkan Error", Body);
    }
}

namespace Lumina::RHI
{
    static_assert(sizeof(GPUPtr) == sizeof(VkDeviceSize), "GPUPtr must be the same size as a vulkan device address");
        
    constexpr VkBufferUsageFlags kDefaultBufferUsages =
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT
        | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
        | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
        | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    
    static constexpr VkPipelineStageFlags2 ToVkPipelineState(EStageFlags Flags)
    {
        VkPipelineStageFlags2 Out = 0;
        
        Out |= EnumHasAnyFlags(Flags, EStageFlags::IndirectArguments)   ? VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT : 0;
        Out |= EnumHasAnyFlags(Flags, EStageFlags::Transfer)            ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : 0;
        Out |= EnumHasAnyFlags(Flags, EStageFlags::Compute)             ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : 0;
        Out |= EnumHasAnyFlags(Flags, EStageFlags::RasterColorOut)      ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : 0;
        Out |= EnumHasAnyFlags(Flags, EStageFlags::PixelShader)         ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT 
                                                                               | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT 
                                                                               | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT : 0;
        
        Out |= EnumHasAnyFlags(Flags, EStageFlags::FragmentTests)   ? VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT 
                                                                           | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT : 0;
        
        Out |= EnumHasAnyFlags(Flags, EStageFlags::VertexShader)    ? VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                                                           | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT : 0;

        Out |= EnumHasAnyFlags(Flags, EStageFlags::MeshShader)      ? VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT : 0;

        Out |= EnumHasAnyFlags(Flags, EStageFlags::Host)            ? VK_PIPELINE_STAGE_2_HOST_BIT : 0;
        Out |= EnumHasAnyFlags(Flags, EStageFlags::AllCommands)     ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : 0;

        return Out;
    }
    
    static VkImageAspectFlags AspectsForFormat(EFormat Format)
    {
        return GuessImageAspectFlags(ConvertFormat(Format));
    }

    static constexpr VkAttachmentLoadOp ToVkLoadOp(ELoadOp Op)
    {
        switch (Op)
        {
            case ELoadOp::Load:  return VK_ATTACHMENT_LOAD_OP_LOAD;
            case ELoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
            default:             return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }
    }

    static constexpr VkAttachmentStoreOp ToVkStoreOp(EStoreOp Op)
    {
        switch (Op)
        {
            case EStoreOp::Store:   return VK_ATTACHMENT_STORE_OP_STORE;
            case EStoreOp::Discard: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
            default:                return VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
    }
    
    static constexpr VkCullModeFlags ToVkCullModeFlags(ECullMode CullMode)
    {
        switch (CullMode)
        {
            case ECullMode::Front:  return VK_CULL_MODE_FRONT_BIT;
            case ECullMode::Back:   return VK_CULL_MODE_BACK_BIT;
            case ECullMode::None:   return VK_CULL_MODE_NONE;
        }
        
        return VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
    }
    
    static constexpr VkFrontFace ToVkFrontFace(EFrontFace FrontFace)
    {
        switch (FrontFace)
        {
            case EFrontFace::CCW: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
            case EFrontFace::CW: return VK_FRONT_FACE_CLOCKWISE;
        }
        return VK_FRONT_FACE_MAX_ENUM;
    }
    
    static constexpr VkStencilOp ToVkStencilOp(EStencilOp Op) 
    {
        switch (Op) 
        {
            case EStencilOp::Keep:              return VK_STENCIL_OP_KEEP;
            case EStencilOp::Zero:              return VK_STENCIL_OP_ZERO;
            case EStencilOp::Replace:           return VK_STENCIL_OP_REPLACE;
            case EStencilOp::IncrementAndClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            case EStencilOp::DecrementAndClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
            case EStencilOp::Invert:            return VK_STENCIL_OP_INVERT;
            case EStencilOp::IncrementAndWrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
            case EStencilOp::DecrementAndWrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        }
        return VK_STENCIL_OP_MAX_ENUM;
    }
    
    static constexpr VkCompareOp ToVkCompareOp(EOp Op)
    {
        switch (Op)
        {
            case EOp::Never:        return VK_COMPARE_OP_NEVER;
            case EOp::Less:         return VK_COMPARE_OP_LESS;
            case EOp::Equal:        return VK_COMPARE_OP_EQUAL;
            case EOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
            case EOp::Greater:      return VK_COMPARE_OP_GREATER;
            case EOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
            case EOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case EOp::Always:       return VK_COMPARE_OP_ALWAYS;
            default:                return VK_COMPARE_OP_MAX_ENUM;
        }
    }
    
    static constexpr VkBlendFactor ToVkBlendFactor(EFactor Factor)
    {
        switch (Factor)
        {
            case EFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
            case EFactor::One:              return VK_BLEND_FACTOR_ONE;
            case EFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
            case EFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case EFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
            case EFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case EFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
            case EFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case EFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
            case EFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        }
        return VK_BLEND_FACTOR_MAX_ENUM;
    }

    static constexpr VkBlendOp ToVkBlendOp(EBlend Blend)
    {
        switch (Blend)
        {
            case EBlend::Add:         return VK_BLEND_OP_ADD;
            case EBlend::Subtract:    return VK_BLEND_OP_SUBTRACT;
            case EBlend::RevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case EBlend::Min:         return VK_BLEND_OP_MIN;
            case EBlend::Max:         return VK_BLEND_OP_MAX;
        }
        return VK_BLEND_OP_MAX_ENUM;
    }

    static constexpr VkIndexType ToVkIndexType(EIndexType Type)
    {
        return Type == EIndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }

    static constexpr VkFilter ToVkFilter(EFilter Filter)
    {
        return Filter == EFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    static constexpr VkSamplerMipmapMode ToVkMipmapMode(EFilter Filter)
    {
        return Filter == EFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }

    static constexpr VkSamplerAddressMode ToVkAddressMode(EAddressMode Mode)
    {
        switch (Mode)
        {
            case EAddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case EAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case EAddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case EAddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
    }

    static constexpr VkPrimitiveTopology ToVkTopology(ETopology Topology)
    {
        switch (Topology)
        {
            case ETopology::TriangleList:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; 
            case ETopology::TriangleStrip:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case ETopology::LineList:       return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        }
        
        return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
    
    static constexpr uint32 SpecializationConstantSize(ESpecializationConstantType Type)
    {
        switch (Type)
        {
            case ESpecializationConstantType::UInt8:
            case ESpecializationConstantType::Int8:
            case ESpecializationConstantType::Boolean: return 1;
            case ESpecializationConstantType::UInt16:
            case ESpecializationConstantType::Int16:   return 2;
            case ESpecializationConstantType::UInt32:
            case ESpecializationConstantType::Int32:
            case ESpecializationConstantType::Float32: return 4;
        }
        return 4;
    }

    static VkSpecializationInfo ConstructSpecializationInfo(FMemMark& Mem, TSpan<const FSpecializationConstant> Constants)
    {
        if (Constants.empty())
        {
            return VkSpecializationInfo{ .mapEntryCount = 0, .pMapEntries = nullptr, .dataSize = 0, .pData = nullptr };
        }

        uint32 TotalSize = 0;
        for (const FSpecializationConstant& Constant : Constants)
        {
            TotalSize += SpecializationConstantSize(Constant.Type);
        }

        auto* Entries = Mem.AllocArray<VkSpecializationMapEntry>(Constants.size());
        auto* Data    = static_cast<std::byte*>(Mem.Allocate(TotalSize, 16));

        uint32 Offset = 0;
        for (size_t i = 0; i < Constants.size(); ++i)
        {
            const FSpecializationConstant& Constant = Constants[i];
            const uint32 Size = SpecializationConstantSize(Constant.Type);

            std::memcpy(Data + Offset, &Constant.AsInt, Size);

            Entries[i].constantID = Constant.ConstantID;
            Entries[i].offset     = Offset;
            Entries[i].size       = Size;

            Offset += Size;
        }

        return VkSpecializationInfo
        {
            .mapEntryCount = static_cast<uint32>(Constants.size()),
            .pMapEntries   = Entries,
            .dataSize      = TotalSize,
            .pData         = Data
        };
    }
    
    static constexpr uint32 kMaxBlockNameLength = 64;

    struct FMemoryBlock
    {
        VkBuffer        Buffer;
        VmaAllocation   Allocation;
        void*           Host;
        GPUPtr          Device;
        uint64          Size;
        #if USING(WITH_EDITOR)
        char            Name[kMaxBlockNameLength];
        EMemoryType     MemType;
        #endif
    };

#if USING(WITH_EDITOR)
    struct FFreedBlock
    {
        GPUPtr          Device;
        uint64          Size;
        uint64          SubmitOrdinal;
        char            Name[kMaxBlockNameLength];
    };

    static constexpr uint32 kFreedBlockHistory = 4096;
    
    struct FTextureRecord
    {
        uint64          Size;
        FTextureDesc    Desc;
        char            Name[kMaxBlockNameLength];
    };
#endif
    
    struct FTexture
    {
        VkImage Image;
        VkImageView DefaultImageView = VK_NULL_HANDLE;
        VmaAllocation Allocation;
        VkImageViewType Type = VK_IMAGE_VIEW_TYPE_2D;
        EFormat Format;
        FTextureDesc Desc;
        bool bSwapchainImage = false;
        
        uint32        BoundSampledSlot = kInvalidHeapSlot;
        FTextureHeapH BoundHeap        = {};

        operator VkImage() const { return Image; }
        operator VkImageView() const { return DefaultImageView; }
    };
    
    struct FTextureHeap
    {
        VkDescriptorSet     DescriptorSet;
        VkDescriptorPool    DescriptorPool;
        FHandleAllocator    SamplerSlots;
        FHandleAllocator    SampledImageSlots;
        FHandleAllocator    RWImageSlots;
        
        TVector<VkSampler>   Samplers;
        TVector<VkImageView> ImageViews;
        TVector<VkImageView> RWImageViews;
        
        // Debug introspection only.
        TVector<FTextureH>   SampledOwners;
        VkImageView          FallbackView = VK_NULL_HANDLE;
    };
    
    struct FSemaphore
    {
        VkSemaphore Semaphore;
        
        operator VkSemaphore() const { return Semaphore; }
    };
    
    struct FPipeline
    {
        VkPipeline Pipeline;
        VkPipelineBindPoint BindPoint;
        
        operator VkPipeline() const { return Pipeline; }
    };
    
    struct FDepthStencilState : FDepthStencilDesc {};
    
#if defined(TRACY_ENABLE)
    static tracy::VkCtx* GTracyGPUContexts[3] = {};
    static tracy::VkCtx* GTracyOwnedContexts[3] = {};

    // The graphics context, for the paths that are graphics-only by construction (present/collect).
    static tracy::VkCtx*& GTracyGPUContext = GTracyGPUContexts[0];

    static constexpr uint32 kMaxGPUZoneDepth = 16;
#endif

    struct FCommandList
    {
        VkCommandBuffer CommandBuffer;
        VkCommandPool   Pool;
        GPUPtr          CurrentIndexBuffer;
        VkIndexType     CurrentIndexType;
        EQueueType      Queue;

#if defined(TRACY_ENABLE)
        alignas(tracy::VkCtxScope) uint8 GPUZoneStack[kMaxGPUZoneDepth * sizeof(tracy::VkCtxScope)];
        uint32 GPUZoneDepth = 0;
#endif

        uint32 BreadcrumbStack[FGpuBreadcrumbs::MaxDepth] = {};
        uint32 BreadcrumbDepth = 0;

        /// Begun and not yet submitted or reset. Tracked per list rather than by counting Open/Reset pairs
        /// because a SUBMITTED list is not reset until its frame slot comes back around.
        bool bOpen = false;
    };

    struct FSurface
    {
        VkSurfaceKHR Surface = VK_NULL_HANDLE;
        void*        Window  = nullptr;   // GLFWwindow*
    };

    struct FSwapchain
    {
        VkSurfaceKHR            Surface;
        VkSwapchainKHR          Swapchain;
        VkFormat               Format;
        FUIntVector2           Extent;
        void*                  Window;             // GLFWwindow*
        TVector<FTextureH>     Images;             // external FTextures (one per swapchain image)
        TVector<VkSemaphore>   AcquireSemaphores;  // binary, ring
        TVector<VkSemaphore>   PresentSemaphores;  // binary, one per image
        uint32                 AcquireIndex;
        uint32                 CurrentImageIndex;
        VkSemaphore            CurrentAcquire;
    };

    struct FDeviceImpl
    {
        VkInstance                      Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT        DebugMessenger = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties      Properties = {};
        TVector<const char*>            EnabledDeviceExtensions;
        VkDeviceSize                    RobustUniformBufferAccessSizeAlignment = 0;
        VkDeviceSize                    RobustStorageBufferAccessSizeAlignment = 0;
        bool                            bUnifiedImageLayouts = false;
        bool                            bMemoryPriority = false;
        bool                            bMeshShaderSupported = false;
        uint32                          MaxMeshWorkGroupCountX = 0;
        // Non-zero when the mesh stage must be pinned to a fixed subgroup size at pipeline creation;
        // see the ShuffleMeshletClip note where this is decided.
        uint32                          MeshRequiredSubgroupSize = 0;
        bool                            bPipelineStats = false;
        TUniquePtr<FVulkanCrashTracker> CrashTracker;
        FGpuBreadcrumbs                 Breadcrumbs;

        VkDevice                        Device;
        VkPhysicalDevice                PhysicsDevice;
        VmaAllocator                    Allocator;
        TArray<VkQueue, 3>              Queues;
        TArray<uint32, 3>               QueueFamilies;

        TArray<uint32, 3>               SharedQueueFamilies;
        uint32                          NumSharedQueueFamilies = 0;
        bool                            bHasAsyncComputeQueue = false;
        bool                            bHasAsyncTransferQueue = false;
        // No surface extensions and no VK_KHR_swapchain were requested. Checked by the presentation
        // entry points so a headless device fails with a sentence rather than a null dispatch call.
        bool                            bHeadless = false;

        VkDescriptorPool                DescriptorPool;
        VkDescriptorSetLayout           DescriptorLayout;
        VkPipelineLayout                PipelineLayout;

        TArray<VkCommandPool, 3>        TransientPools;
        struct FPendingTransition
        {
            VkCommandBuffer Buffer;
            VkCommandPool   Pool;
            uint32          Slot;
        };
        TVector<FPendingTransition>     PendingTransient;
        // Frame slot currently being recorded. Published by RetireSlot AFTER it drains, so a concurrent
        // retire can never land in the list being drained.
        TAtomic<uint32>                 CurrentRetireSlot{0};

        struct FPendingHeapDestroy
        {
            VkImageView View;
            VkSampler   Sampler;
            uint32      Slot;
        };
        TVector<FPendingHeapDestroy>    PendingHeapDestroys;

        // All live allocations, sorted by device address for interior-pointer resolution.
        TVector<FMemoryBlock>           MemoryBlocks;

#if USING(WITH_EDITOR)
        // Ring of destroyed allocations, guarded by MemoryMutex alongside MemoryBlocks. Unsorted: it is
        // read once, by the device-lost report, and a linear scan of 4096 entries is free there.
        TVector<FFreedBlock>            FreedBlocks;
        uint32                          FreedBlockCursor = 0;
        // Bumped per submit, stamped into FreedBlocks. Relaxed: it is a coarse age, not a synchronizer.
        TAtomic<uint64>                 SubmitOrdinal{0};

        // Live textures, for the memory tool's per-purpose breakdown. Its own lock rather than
        // MemoryMutex: texture creation and buffer allocation both run on the streaming jobs.
        THashMap<uint64, FTextureRecord> TextureLedger;
        FMutex                           TextureLedgerMutex;
#endif

        TSegmentMap<FSemaphore>         Semaphores;
        TSegmentMap<FPipeline>          Pipelines;
        TSegmentMap<FTexture>           Textures;
        TSegmentMap<FCommandList>       CommandLists;
        TSegmentMap<FTextureHeap>       TextureHeaps;
        TSegmentMap<FDepthStencilState> DepthStates;
        TSegmentMap<FSwapchain>         Swapchains;
        TSegmentMap<FSurface>           Surfaces;

        TArray<TVector<FCmdListH>, 3>   FreeCommandLists;
        
        TAtomic<uint32>                 OpenCommandLists[3] = {};

        TVector<FTextureH>              UninitializedTextures;
        // Published under InitMutex, so Submit can skip the lock when no image is waiting.
        TAtomic<uint32>                 PendingImageInits{0};

        VkMemoryRequirements            MemoryRequirements;

        FSharedMutex                    MemoryMutex;
        TArray<FMutex, 3>               QueueMutexes;
        TArray<uint32, 3>               QueueLockIndex;
        FMutex                          TransientMutex;
        FMutex                          CommandPoolMutex;
        FMutex                          InitMutex;
        FMutex                          HeapMutex;

        operator VkDevice() const           { return Device; }
        operator VkPhysicalDevice() const   { return PhysicsDevice; }
    };

    static FDeviceImpl* GDevice;

    static bool GLogCopies = false;

    // The mutex owning the VkQueue that Type resolves to. Aliased types share the owner's lock.
    static FMutex& QueueLockFor(EQueueType Type)
    {
        return GDevice->QueueMutexes[GDevice->QueueLockIndex[(uint32)Type]];
    }

    class FAllQueuesLock
    {
    public:
        FAllQueuesLock()
        {
            for (uint32 i = 0; i < 3; ++i)
            {
                // Only the owner of a lock index takes it; aliased types would double-lock.
                if (GDevice->QueueLockIndex[i] == i)
                {
                    GDevice->QueueMutexes[i].lock();
                    Held[Count++] = i;
                }
            }
        }

        ~FAllQueuesLock()
        {
            while (Count > 0)
            {
                GDevice->QueueMutexes[Held[--Count]].unlock();
            }
        }

        FAllQueuesLock(const FAllQueuesLock&) = delete;
        FAllQueuesLock& operator=(const FAllQueuesLock&) = delete;

    private:
        uint32 Held[3] = {};
        uint32 Count = 0;
    };

    static void FlushPendingHeapDestroysLocked(uint32 Slot, bool bForce);

    static TVector<Native::FDeviceCreationRequest> GPendingDeviceRequests;

    // Resolve a GPUPtr (possibly interior) to its owning allocation. Caller holds MemoryMutex (shared).
    static const FMemoryBlock* FindMemory(GPUPtr Ptr)
    {
        const TVector<FMemoryBlock>& Blocks = GDevice->MemoryBlocks;
        auto It = std::ranges::upper_bound(Blocks, Ptr, {}, &FMemoryBlock::Device);
        if (It == Blocks.begin())
        {
            return nullptr;
        }
        --It;
        if (Ptr - It->Device >= It->Size)
        {
            return nullptr;
        }
        return It;
    }

#if USING(WITH_EDITOR)
    // Same search, for the one caller that WRITES to the block. Caller holds MemoryMutex.
    static FMemoryBlock* FindMemoryMutable(GPUPtr Ptr)
    {
        return const_cast<FMemoryBlock*>(FindMemory(Ptr));
    }

    static void CopyBlockName(char (&Dest)[kMaxBlockNameLength], const char* Name)
    {
        if (Name == nullptr)
        {
            Dest[0] = '\0';
            return;
        }

        std::snprintf(Dest, kMaxBlockNameLength, "%s", Name);
    }

    // Caller holds MemoryMutex. Allocated once and overwritten in place forever after: this runs on the
    // free path, which a growing scene renderer hits every time a ring resizes.
    static void RecordFreedBlockLocked(const FMemoryBlock& Block)
    {
        TVector<FFreedBlock>& Ring = GDevice->FreedBlocks;
        if (Ring.size() < kFreedBlockHistory)
        {
            Ring.resize(kFreedBlockHistory);
            for (FFreedBlock& Entry : Ring)
            {
                Entry.Device = 0;
                Entry.Size   = 0;
            }
        }

        FFreedBlock& Entry  = Ring[GDevice->FreedBlockCursor];
        Entry.Device        = Block.Device;
        Entry.Size          = Block.Size;
        Entry.SubmitOrdinal = GDevice->SubmitOrdinal.load(std::memory_order_relaxed);
        CopyBlockName(Entry.Name, Block.Name);

        GDevice->FreedBlockCursor = (GDevice->FreedBlockCursor + 1u) % kFreedBlockHistory;
    }
#endif

    static TAtomic<FValidationHandlerFn> GValidationHandler{nullptr};
    static TAtomic<void*>                GValidationHandlerUserData{nullptr};
    
    void SetValidationHandler(FValidationHandlerFn Handler, void* UserData)
    {
        GValidationHandlerUserData.store(UserData, std::memory_order_relaxed);
        GValidationHandler.store(Handler, std::memory_order_release);
    }

    static VkBool32 VKAPI_PTR VkDebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT MessageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT MessageTypes,
        const VkDebugUtilsMessengerCallbackDataEXT* CallbackData,
        void* UserData)
    {
        if (MessageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
        {
            return VK_FALSE;
        }

        auto GetMessageTypeString = [](VkDebugUtilsMessageTypeFlagsEXT Types) -> FFixedString
        {
            FFixedString Result;
            if (Types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)
            {
                Result += "[General] ";
            }
            if (Types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
            {
                Result += "[Validation] ";
            }
            return Result.empty() ? "[Unknown] " : Result;
        };

        const FFixedString TypeString = GetMessageTypeString(MessageTypes);
        const FStringView StringView(TypeString.c_str(), TypeString.size());

        EValidationSeverity Severity;
        switch (MessageSeverity)
        {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            LOG_TRACE("Vulkan {}{}", StringView, CallbackData->pMessage);
            Severity = EValidationSeverity::Verbose;
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            LOG_DEBUG("Vulkan {}{}", StringView, CallbackData->pMessage);
            Severity = EValidationSeverity::Info;
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            LOG_WARN("Vulkan {}{}", StringView, CallbackData->pMessage);
            Severity = EValidationSeverity::Warning;
            break;
        default:
            LOG_ERROR("Vulkan {}{}", StringView, CallbackData->pMessage);
            Severity = EValidationSeverity::Error;
            break;
        }

        if (const FValidationHandlerFn Handler = GValidationHandler.load(std::memory_order_acquire))
        {
            Handler(Severity, CallbackData->pMessage, GValidationHandlerUserData.load(std::memory_order_relaxed));
        }

        return VK_FALSE;
    }

    static void ShowVulkanInitFailure(const char* Title, const FString& Message)
    {
        LOG_CRITICAL("{}: {}", Title, Message);
        Dialogs::ShowInternal(Dialogs::ESeverity::FatalError, Dialogs::EType::Ok, Title, Message);
    }

    struct FDeviceSuitability
    {
        bool    bSuitable = false;

        /// Why the device was turned down, phrased to be read in a dialog. Empty when suitable.
        FString Reason;

        /// Width the mesh stage must be pinned to, or zero when the device needs no pinning.
        uint32  MeshRequiredSubgroupSize = 0;

        /// maxMeshWorkGroupCount[0] as reported, before the overflow clamp the caller applies.
        uint32  MaxMeshWorkGroupCount = 0;
    };
    
    static FDeviceSuitability EvaluateDeviceSuitability(VkPhysicalDevice Gpu, bool bHeadless, bool bLog)
    {
        FDeviceSuitability Result;

        uint32 ExtCount = 0;
        vkEnumerateDeviceExtensionProperties(Gpu, nullptr, &ExtCount, nullptr);
        TVector<VkExtensionProperties> Available(ExtCount);
        vkEnumerateDeviceExtensionProperties(Gpu, nullptr, &ExtCount, Available.data());

        auto HasExtension = [&](const char* Name)
        {
            for (const VkExtensionProperties& Ext : Available)
            {
                if (strcmp(Ext.extensionName, Name) == 0)
                {
                    return true;
                }
            }
            return false;
        };

        if (!bHeadless && !HasExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            Result.Reason = "no VK_KHR_swapchain support";
            return Result;
        }

        {
            uint32 FamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(Gpu, &FamilyCount, nullptr);
            TVector<VkQueueFamilyProperties> Families(FamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(Gpu, &FamilyCount, Families.data());

            bool bHasGraphics = false;
            for (const VkQueueFamilyProperties& Family : Families)
            {
                if (Family.queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    bHasGraphics = true;
                    break;
                }
            }

            if (!bHasGraphics)
            {
                Result.Reason = "exposes no graphics queue family";
                return Result;
            }
        }

        if (!HasExtension(VK_EXT_MESH_SHADER_EXTENSION_NAME))
        {
            Result.Reason = "no VK_EXT_mesh_shader extension";
            return Result;
        }

        VkPhysicalDeviceMeshShaderFeaturesEXT SupportedMesh
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 MeshQuery
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &SupportedMesh };
        vkGetPhysicalDeviceFeatures2(Gpu, &MeshQuery);

        if (!SupportedMesh.meshShader)
        {
            Result.Reason = "the meshShader feature is not supported";
            return Result;
        }

        // Core since Vulkan 1.3, and callers only offer devices that already reported 1.4.
        VkPhysicalDeviceSubgroupSizeControlProperties SubgroupProps
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES };
        VkPhysicalDeviceMeshShaderPropertiesEXT MeshProps
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT, .pNext = &SubgroupProps };
        VkPhysicalDeviceProperties2 MeshPropQuery
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &MeshProps };
        vkGetPhysicalDeviceProperties2(Gpu, &MeshPropQuery);

        VkPhysicalDeviceVulkan13Features Supported13
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 FeatureQuery
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &Supported13 };
        vkGetPhysicalDeviceFeatures2(Gpu, &FeatureQuery);

        const struct { const char* Name; uint32 Required; uint32 Actual; } Limits[] =
        {
            { "maxMeshWorkGroupSize[0]",     kMeshWorkGroupSize,       MeshProps.maxMeshWorkGroupSize[0]     },
            { "maxMeshWorkGroupInvocations", kMeshWorkGroupSize,       MeshProps.maxMeshWorkGroupInvocations },
            { "maxMeshOutputVertices",       kMeshMaxOutputVertices,   MeshProps.maxMeshOutputVertices       },
            { "maxMeshOutputPrimitives",     kMeshMaxOutputPrimitives, MeshProps.maxMeshOutputPrimitives     },
        };

        for (const auto& Limit : Limits)
        {
            if (Limit.Actual < Limit.Required)
            {
                Result.Reason = FString("reports ") + Limit.Name + " = "
                    + Lumina::Format("{}", Limit.Actual).c_str() + ", the geometry path needs at least "
                    + Lumina::Format("{}", Limit.Required).c_str();
                return Result;
            }
        }

        if (SubgroupProps.minSubgroupSize < kMeshWorkGroupSize)
        {
            const bool bMeshStagePinnable =
                (SubgroupProps.requiredSubgroupSizeStages & VK_SHADER_STAGE_MESH_BIT_EXT) != 0;

            if (Supported13.subgroupSizeControl && bMeshStagePinnable
                && SubgroupProps.maxSubgroupSize >= kMeshWorkGroupSize)
            {
                Result.MeshRequiredSubgroupSize = kMeshWorkGroupSize;
            }
            else
            {
                Result.Reason = FString("may run a mesh subgroup as narrow as ")
                    + Lumina::Format("{}", SubgroupProps.minSubgroupSize).c_str() + " and cannot be pinned to "
                    + Lumina::Format("{}", kMeshWorkGroupSize).c_str() + " (subgroupSizeControl: "
                    + (Supported13.subgroupSizeControl ? "yes" : "no") + ", mesh stage pinnable: "
                    + (bMeshStagePinnable ? "yes" : "no") + ", maxSubgroupSize: "
                    + Lumina::Format("{}", SubgroupProps.maxSubgroupSize).c_str() + ")";
                return Result;
            }
        }

        if (bLog)
        {
            // maxMeshWorkGroupCount[0] is printed RAW, before the caller's clamp: a device reporting
            // 4294967295 there is the signature of the overflow that made an entire machine render
            // nothing, so it is worth being able to read it straight out of a user's log.
            LOG_DISPLAY("Mesh shader limits: workgroup {} (max invocations {}), out verts {}, out prims {}, "
                        "out components {}, out memory {} B, max workgroup count {}. "
                        "Subgroup {}-{}, mesh workgroup is {} threads{}.",
                        MeshProps.maxMeshWorkGroupSize[0], MeshProps.maxMeshWorkGroupInvocations,
                        MeshProps.maxMeshOutputVertices, MeshProps.maxMeshOutputPrimitives,
                        MeshProps.maxMeshOutputComponents, MeshProps.maxMeshOutputMemorySize,
                        MeshProps.maxMeshWorkGroupCount[0],
                        SubgroupProps.minSubgroupSize, SubgroupProps.maxSubgroupSize, kMeshWorkGroupSize,
                        Result.MeshRequiredSubgroupSize != 0 ? " (subgroup size pinned)" : "");
        }

        Result.MaxMeshWorkGroupCount = MeshProps.maxMeshWorkGroupCount[0];
        Result.bSuitable             = true;
        return Result;
    }

    ICrashTracker& GetCrashTracker()
    {
        return *GDevice->CrashTracker;
    }

    void GetGPUMemoryStats(FGPUMemoryStats& Out)
    {
        Out = FGPUMemoryStats{};
        if (GDevice == nullptr)
        {
            return;
        }

        VkPhysicalDeviceMemoryProperties MemProps{};
        vkGetPhysicalDeviceMemoryProperties(GDevice->PhysicsDevice, &MemProps);

        VmaBudget Budgets[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(GDevice->Allocator, Budgets);

        for (uint32 HeapIndex = 0; HeapIndex < MemProps.memoryHeapCount; ++HeapIndex)
        {
            const VmaBudget& Budget = Budgets[HeapIndex];

            FGPUMemoryHeapStats Heap;
            Heap.HeapIndex       = HeapIndex;
            Heap.bDeviceLocal    = (MemProps.memoryHeaps[HeapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            Heap.BudgetBytes     = Budget.budget;
            Heap.UsageBytes      = Budget.usage;
            Heap.AllocatedBytes  = Budget.statistics.allocationBytes;
            Heap.BlockBytes      = Budget.statistics.blockBytes;
            Heap.BlockCount      = Budget.statistics.blockCount;
            Heap.AllocationCount = Budget.statistics.allocationCount;

            for (uint32 TypeIndex = 0; TypeIndex < MemProps.memoryTypeCount; ++TypeIndex)
            {
                if (MemProps.memoryTypes[TypeIndex].heapIndex != HeapIndex)
                {
                    continue;
                }
                if (MemProps.memoryTypes[TypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                {
                    Heap.bHostVisible = true;
                }
            }
            Heap.bReBAR = Heap.bDeviceLocal && Heap.bHostVisible && MemProps.memoryHeaps[HeapIndex].size > (256ull << 20);

            Out.TotalBudget      += Heap.BudgetBytes;
            Out.TotalUsage       += Heap.UsageBytes;
            Out.TotalAllocated   += Heap.AllocatedBytes;
            Out.TotalBlockBytes  += Heap.BlockBytes;
            Out.TotalAllocations += Heap.AllocationCount;
            Out.TotalBlocks      += Heap.BlockCount;
            Out.Heaps.push_back(Heap);
        }
    }

    static const char* MemoryTypeToString(EMemoryType Type)
    {
        switch (Type)
        {
        case EMemoryType::CPUWrite: return "CPUWrite";
        case EMemoryType::CPURead:  return "CPURead";
        case EMemoryType::GPUOnly:  return "GPUOnly";
        }
        return "Unknown";
    }

    [[noreturn]] static void PanicOutOfGPUMemory(const char* What, VkResult Result)
    {
        FGPUMemoryStats Stats;
        GetGPUMemoryStats(Stats);

        LOG_CRITICAL("GPU OUT OF MEMORY allocating {}: {}", What, Vulkan::VkResultToString(Result));
        for (const FGPUMemoryHeapStats& Heap : Stats.Heaps)
        {
            const char* Kind = Heap.bReBAR      ? "Device (ReBAR)"
                             : Heap.bDeviceLocal ? (Heap.bHostVisible ? "Device (BAR)" : "Device")
                             : "Host";

            LOG_CRITICAL("  Heap {} {}: {} / {} MiB used, {} allocations across {} blocks.",
                Heap.HeapIndex, Kind, Heap.UsageBytes >> 20, Heap.BudgetBytes >> 20, Heap.AllocationCount, Heap.BlockCount);
        }

        PANIC("GPU out of memory allocating {}.", What);
        std::unreachable();
    }

    // Share of a small (non-ReBAR) CPU-visible VRAM aperture that any one CPU-write ring may reserve.
    static constexpr uint64 kCPUWriteApertureDivisor = 4;
    static constexpr uint64 kMinCPUWriteSlice        = 8ull * 1024 * 1024;

    uint64 ClampCPUWriteSlice(const char* RingName, uint64 DesiredSliceSize, uint32 SliceCount)
    {
        if (GDevice == nullptr || SliceCount == 0)
        {
            return DesiredSliceSize;
        }

        FGPUMemoryStats Stats;
        GetGPUMemoryStats(Stats);

        uint64 Aperture = 0;
        for (const FGPUMemoryHeapStats& Heap : Stats.Heaps)
        {
            if (!Heap.bDeviceLocal || !Heap.bHostVisible)
            {
                continue;
            }
            if (Heap.bReBAR)
            {
                return DesiredSliceSize;
            }
            Aperture = Math::Max(Aperture, Heap.BudgetBytes);
        }

        // No host-visible VRAM heap: CPU-write already lands in system memory, nothing to ration.
        if (Aperture == 0)
        {
            return DesiredSliceSize;
        }

        // Typed rather than a ull literal: ull is unsigned long long, which is a DIFFERENT type from
        // uint64 wherever uint64_t is unsigned long, so the two arguments stopped deducing to one T.
        constexpr uint64 kMegabyte = 1024 * 1024;

        const uint64 PerSlice = (Aperture / kCPUWriteApertureDivisor) / SliceCount;
        const uint64 Cap      = Math::Max(kMinCPUWriteSlice, (PerSlice / kMegabyte) * kMegabyte);
        if (Cap >= DesiredSliceSize)
        {
            return DesiredSliceSize;
        }

        LOG_DISPLAY("RHI: {} ring clamped {} MiB -> {} MiB/slice x{} (ReBAR off, {} MiB CPU-visible aperture).",
            RingName, DesiredSliceSize >> 20, Cap >> 20, SliceCount, Aperture >> 20);
        return Cap;
    }

#if USING(WITH_EDITOR)
    void GetGPUAllocations(TVector<FGPUAllocation>& Out)
    {
        Out.clear();
        if (GDevice == nullptr)
        {
            return;
        }

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            Out.reserve(GDevice->MemoryBlocks.size());

            for (const FMemoryBlock& Block : GDevice->MemoryBlocks)
            {
                FGPUAllocation& Alloc = Out.push_back();
                Alloc.Kind   = EGPUAllocationKind::Buffer;
                Alloc.Size   = Block.Size;
                Alloc.Memory = Block.MemType;
                std::snprintf(Alloc.Name, kMaxGPUAllocationName, "%s", Block.Name);
            }
        }

        {
            FScopeLock Lock(GDevice->TextureLedgerMutex);
            Out.reserve(Out.size() + GDevice->TextureLedger.size());

            for (const auto& [Handle, Record] : GDevice->TextureLedger)
            {
                FGPUAllocation& Alloc = Out.push_back();
                Alloc.Kind = EGPUAllocationKind::Texture;
                Alloc.Size = Record.Size;
                Alloc.Desc = Record.Desc;
                std::snprintf(Alloc.Name, kMaxGPUAllocationName, "%s", Record.Name);
            }
        }
    }
#endif

    FGPUDeviceInfo GetDeviceInfo()
    {
        FGPUDeviceInfo Info;
        if (GDevice == nullptr)
        {
            return Info;
        }

        const VkPhysicalDeviceProperties& Props = GDevice->Properties;
        Info.Name      = Props.deviceName;
        Info.VendorID  = Props.vendorID;
        Info.bDiscrete = Props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        Info.APIName   = "Vulkan ";
        Info.APIName += Lumina::Format("{}", VK_API_VERSION_MAJOR(Props.apiVersion)).c_str();
        Info.APIName += ".";
        Info.APIName += Lumina::Format("{}", VK_API_VERSION_MINOR(Props.apiVersion)).c_str();
        Info.APIName += ".";
        Info.APIName += Lumina::Format("{}", VK_API_VERSION_PATCH(Props.apiVersion)).c_str();
        return Info;
    }

    namespace Native
    {
        FNativeDeviceHandles GetNativeDeviceHandles()
        {
            FNativeDeviceHandles Handles;
            Handles.Backend = EBackend::Vulkan;
            if (GDevice == nullptr)
            {
                return Handles;
            }

            // Dispatchable Vk handles are pointers -> implicit to void*; PFNs need a reinterpret.
            Handles.Instance            = GDevice->Instance;
            Handles.PhysicalDevice      = GDevice->PhysicsDevice;
            Handles.Device              = GDevice->Device;
            Handles.GraphicsQueue       = GDevice->Queues[(uint32)EQueueType::Graphics];
            Handles.GraphicsQueueFamily = GDevice->QueueFamilies[(uint32)EQueueType::Graphics];
            Handles.GetInstanceProcAddr = reinterpret_cast<void*>(vkGetInstanceProcAddr);   // volk globals
            Handles.GetDeviceProcAddr   = reinterpret_cast<void*>(vkGetDeviceProcAddr);
            Handles.ApiVersion          = GDevice->Properties.apiVersion;
            return Handles;
        }

        void* GetNativeCommandBuffer(FCmdListH CommandList)
        {
            if (GDevice == nullptr || !IsValid(CommandList))
            {
                return nullptr;
            }
            return GDevice->CommandLists[CommandList].CommandBuffer;
        }

        void RegisterDeviceCreationRequest(const FDeviceCreationRequest& Request)
        {
            GPendingDeviceRequests.push_back(Request);
        }

        void AcquireSubmitLock()
        {
            if (GDevice)
            {
                for (uint32 i = 0; i < 3; ++i)
                {
                    if (GDevice->QueueLockIndex[i] == i)
                    {
                        GDevice->QueueMutexes[i].lock();
                    }
                }
            }
        }

        void ReleaseSubmitLock()
        {
            if (GDevice)
            {
                for (uint32 i = 3; i-- > 0; )
                {
                    if (GDevice->QueueLockIndex[i] == i)
                    {
                        GDevice->QueueMutexes[i].unlock();
                    }
                }
            }
        }
    }

    void HandleDeviceLost()
    {
        LOG_ERROR("[DeviceLost] Vulkan device lost.");

        if (GDevice != nullptr)
        {
            const FString Innermost = GDevice->Breadcrumbs.ReportOutstanding();
            if (!Innermost.empty())
            {
                CrashReporting::SetAttribute("GPUPass", Innermost);
            }
        }

        if (GDevice != nullptr && GDevice->CrashTracker)
        {
            GDevice->CrashTracker->OnDeviceLost();
        }

        CrashHandler::ReportFatal("Vulkan device lost", CrashHandler::EFatalKind::Gpu);

        std::abort();
    }

    void CreateDevice(const FDeviceDesc& DeviceDesc)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        GDevice = new FDeviceImpl{};
        GDevice->CrashTracker = MakeUnique<FVulkanCrashTracker>();
        GDevice->bHeadless    = DeviceDesc.bHeadless;

        GLogCopies = GCommandLine != nullptr && GCommandLine->Has("logcopies");

        // GLFW is only consulted for the window-system bits. A headless caller has no GLFW instance at
        // all, so asking it anything here would report failure on a perfectly good driver.
        if (!DeviceDesc.bHeadless && !glfwVulkanSupported())
        {
            ShowVulkanInitFailure("Vulkan Not Supported",
                "GLFW reports that this system does not support Vulkan. The Vulkan loader ("
            #if defined(LE_PLATFORM_WINDOWS)
                "vulkan-1.dll"
            #else
                "libvulkan.so.1"
            #endif
                ") was not found, or no installed GPU driver provides a Vulkan ICD.");
            std::abort();
        }

        if (volkInitialize() != VK_SUCCESS)
        {
            ShowVulkanInitFailure("Vulkan Loader Failure",
                "Failed to initialize the Vulkan loader (volkInitialize). The Vulkan runtime appears to be missing or corrupted.");
            std::abort();
        }

        uint32 ValidationLayerVersion = 0;
        {
            const char* EnabledLayers[1] = {};
            uint32 LayerCount = 0;

            if (DeviceDesc.bValidation)
            {
                uint32 AvailableCount = 0;
                vkEnumerateInstanceLayerProperties(&AvailableCount, nullptr);
                TVector<VkLayerProperties> Available(AvailableCount);
                vkEnumerateInstanceLayerProperties(&AvailableCount, Available.data());

                for (const VkLayerProperties& Layer : Available)
                {
                    if (strcmp(Layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                    {
                        EnabledLayers[LayerCount++] = "VK_LAYER_KHRONOS_validation";
                        ValidationLayerVersion = Layer.specVersion;
                        break;
                    }
                }
                if (LayerCount == 0)
                {
                    LOG_WARN("Vulkan validation requested but VK_LAYER_KHRONOS_validation is not installed.");
                }
            }

            // Surface extensions for the swapchain plus debug utils. Headless takes neither: with no
            // GLFW instance the query returns null, and there is nothing to present to anyway.
            TVector<const char*> InstanceExtensions;
            if (!DeviceDesc.bHeadless)
            {
                uint32 GlfwExtCount = 0;
                const char** GlfwExts = glfwGetRequiredInstanceExtensions(&GlfwExtCount);
                InstanceExtensions.assign(GlfwExts, GlfwExts + GlfwExtCount);
            }
            if (DeviceDesc.bValidation || DeviceDesc.bDebugUtils)
            {
                InstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }

            if (!GPendingDeviceRequests.empty())
            {
                uint32 AvailCount = 0;
                vkEnumerateInstanceExtensionProperties(nullptr, &AvailCount, nullptr);
                TVector<VkExtensionProperties> Available(AvailCount);
                vkEnumerateInstanceExtensionProperties(nullptr, &AvailCount, Available.data());

                auto IsAvailable = [&](const char* Name)
                {
                    for (const VkExtensionProperties& Ext : Available)
                    {
                        if (strcmp(Ext.extensionName, Name) == 0)
                        {
                            return true;
                        }
                    }
                    return false;
                };
                auto AppendUnique = [](TVector<const char*>& List, const char* Name)
                {
                    for (const char* Existing : List)
                    {
                        if (strcmp(Existing, Name) == 0)
                        {
                            return;
                        }
                    }
                    List.push_back(Name);
                };

                for (const Native::FDeviceCreationRequest& Request : GPendingDeviceRequests)
                {
                    for (const char* Name : Request.InstanceExtensions)
                    {
                        if (IsAvailable(Name))
                        {
                            AppendUnique(InstanceExtensions, Name);
                        }
                        else
                        {
                            LOG_WARN("Skipping unsupported requested instance extension '{}'.", Name);
                        }
                    }
                }
            }

            VkApplicationInfo AppInfo
            {
                .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                .pNext              = nullptr, 
                .pApplicationName   = "Lumina Engine",
                .applicationVersion = 1,
                .pEngineName        = "Lumina",
                .engineVersion      = 1,
                .apiVersion         = VK_API_VERSION_1_4,
            };

            // Messenger info doubles as instance-creation pNext so create/destroy are covered too.
            VkDebugUtilsMessengerCreateInfoEXT MessengerInfo
            {
                .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .pNext           = nullptr, 
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = VkDebugCallback,
            };

            static constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

            static constexpr VkBool32 kEnable  = VK_TRUE;
            static constexpr VkBool32 kDisable = VK_FALSE;

            VkLayerSettingEXT LayerSettings[24] = {};
            uint32 LayerSettingCount = 0;

            auto AddSetting = [&](const char* Name, const VkBool32& Value)
            {
                ASSERT(LayerSettingCount < std::size(LayerSettings));

                LayerSettings[LayerSettingCount++] = VkLayerSettingEXT
                {
                    .pLayerName   = kValidationLayer,
                    .pSettingName = Name,
                    .type         = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                    .valueCount   = 1,
                    .pValues      = &Value,
                };
            };

            auto AddUIntSetting = [&](const char* Name, const uint32& Value)
            {
                ASSERT(LayerSettingCount < std::size(LayerSettings));

                LayerSettings[LayerSettingCount++] = VkLayerSettingEXT
                {
                    .pLayerName   = kValidationLayer,
                    .pSettingName = Name,
                    .type         = VK_LAYER_SETTING_TYPE_UINT32_EXT,
                    .valueCount   = 1,
                    .pValues      = &Value,
                };
            };

            auto IsListed = [](const char* Argument, const char* Check)
            {
                if (GCommandLine == nullptr)
                {
                    return false;
                }

                const TOptional<FFixedString> List = GCommandLine->Get(Argument);
                if (!List.has_value())
                {
                    return false;
                }

                const FString Haystack = FString(",") + List.value().c_str() + ",";
                const FString Needle   = FString(",") + Check + ",";
                return Haystack.find(Needle) != FString::npos;
            };

            auto ResolveCheck = [&](const char* Check, bool bDefault)
            {
                if (IsListed("novalidate", Check))
                {
                    return false;
                }
                if (IsListed("validate", Check))
                {
                    return true;
                }
                return bDefault;
            };

            AddSetting("validate_sync", ResolveCheck("sync", true) ? kEnable : kDisable);

            AddSetting("syncval_message_extra_properties", ResolveCheck("syncdetail", true) ? kEnable : kDisable);

            if (DeviceDesc.bValidation)
            {
                AddSetting("gpuav_enable", kEnable);

                struct FCheck { const char* Setting; const char* Token; bool bDefault; };
                static constexpr FCheck kChecks[] =
                {
                    { "gpuav_shader_instrumentation",          "instrument",       false },

                    { "gpuav_descriptor_checks",               "descriptor",       false },
                    { "gpuav_buffer_address_oob",              "bda",              true  },
                    { "gpuav_post_process_descriptor_indexing","postprocess",      false },
                    { "gpuav_buffers_validation",              "buffers",          true  },
                    { "gpuav_indirect_draws_buffers",          "indirectdraw",     true  },
                    { "gpuav_indirect_dispatches_buffers",     "indirectdispatch", true  },
                    { "gpuav_index_buffers",                   "indexbuffer",      true  },
                    { "gpuav_buffer_copies",                   "buffercopy",       true  },
                    { "gpuav_image_layout",                    "imagelayout",      true  },
                    { "gpuav_reserve_binding_slot",            "slot",             true  },
                };

                for (const FCheck& Check : kChecks)
                {
                    AddSetting(Check.Setting, ResolveCheck(Check.Token, Check.bDefault) ? kEnable : kDisable);
                }
                
                static constexpr uint32 kMaxTrackedAddresses = 65536;
                AddUIntSetting("gpuav_max_buffer_device_addresses", kMaxTrackedAddresses);
            }
            else
            {
                AddSetting("gpuav_enable", kDisable);
            }

            if (GCommandLine != nullptr)
            {
                if (const TOptional<FFixedString> Disabled = GCommandLine->Get("novalidate"))
                {
                    LOG_WARN("Vulkan RHI - validation checks disabled by --novalidate: {}", Disabled.value().c_str());
                }
                if (const TOptional<FFixedString> Enabled = GCommandLine->Get("validate"))
                {
                    LOG_WARN("Vulkan RHI - validation checks force-enabled by --validate: {}", Enabled.value().c_str());
                }
            }

            VkLayerSettingsCreateInfoEXT LayerSettingsInfo
            {
                .sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                .pNext        = &MessengerInfo,
                .settingCount = LayerSettingCount,
                .pSettings    = LayerSettings,
            };

            VkInstanceCreateInfo InstanceInfo
            {
                .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                .pNext                   = DeviceDesc.bValidation ? (const void*)&LayerSettingsInfo : nullptr,
                .pApplicationInfo        = &AppInfo,
                .enabledLayerCount       = LayerCount,
                .ppEnabledLayerNames     = EnabledLayers,
                .enabledExtensionCount   = (uint32)InstanceExtensions.size(),
                .ppEnabledExtensionNames = InstanceExtensions.data(),
            };

            const VkResult InstanceResult = vkCreateInstance(&InstanceInfo, nullptr, &GDevice->Instance);
            if (InstanceResult != VK_SUCCESS)
            {
                ShowVulkanInitFailure("Vulkan Instance Creation Failed",
                    FString("Failed to create a Vulkan 1.4 instance: ") + Vulkan::VkResultToString(InstanceResult));
                std::abort();
            }

            volkLoadInstance(GDevice->Instance);

            if (DeviceDesc.bValidation && vkCreateDebugUtilsMessengerEXT != nullptr)
            {
                VK_CHECK(vkCreateDebugUtilsMessengerEXT(GDevice->Instance, &MessengerInfo, nullptr, &GDevice->DebugMessenger));
            }
        }

        {
            uint32 GpuCount = 0;
            vkEnumeratePhysicalDevices(GDevice->Instance, &GpuCount, nullptr);
            TVector<VkPhysicalDevice> Gpus(GpuCount);
            vkEnumeratePhysicalDevices(GDevice->Instance, &GpuCount, Gpus.data());

            VkPhysicalDevice Best = VK_NULL_HANDLE;
            int32 BestScore = -1;

            // Kept so a machine where every modern device is unusable can name each one and its
            // reason, rather than reporting only whichever happened to score highest.
            FString Rejections;
            bool bAnyModernDevice = false;

            for (VkPhysicalDevice Gpu : Gpus)
            {
                VkPhysicalDeviceProperties Props;
                vkGetPhysicalDeviceProperties(Gpu, &Props);

                if (Props.apiVersion < VK_API_VERSION_1_4)
                {
                    continue;
                }

                bAnyModernDevice = true;

                // Asked before scoring, not after choosing. A device that cannot run the renderer
                // must not be allowed to outrank one that can purely on the strength of its type.
                const FDeviceSuitability Suitability = EvaluateDeviceSuitability(Gpu, DeviceDesc.bHeadless, false);

                if (!Suitability.bSuitable)
                {
                    LOG_WARN("Skipping GPU '{}': {}.", Props.deviceName, Suitability.Reason);
                    Rejections += FString("  ") + Props.deviceName + ": " + Suitability.Reason + "\n";
                    continue;
                }

                const int32 Score = (Props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000
                                  : (Props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 100 : 1;
                if (Score > BestScore)
                {
                    Best = Gpu;
                    BestScore = Score;
                }
            }

            if (Best == VK_NULL_HANDLE && bAnyModernDevice)
            {
                FString Message = "No GPU meeting the renderer's requirements was found.\n\n";
                Message += Rejections;
                Message += "\nMesh shaders need Turing (GTX 16-series / RTX 20-series) or newer on NVIDIA, "
                    "RDNA2 (RX 6000) or newer on AMD, or Arc on Intel.";
                ShowVulkanInitFailure("Vulkan Device Unsuitable", Message);
                std::abort();
            }

            if (Best == VK_NULL_HANDLE)
            {
                uint32 InstanceVersion = VK_API_VERSION_1_0;
                if (vkEnumerateInstanceVersion != nullptr)
                {
                    vkEnumerateInstanceVersion(&InstanceVersion);
                }

                FString Message = "No GPU supporting Vulkan 1.4 was found.\n\nDetected Vulkan instance API version: ";
                Message += Lumina::Format("{}", VK_API_VERSION_MAJOR(InstanceVersion)).c_str();
                Message += ".";
                Message += Lumina::Format("{}", VK_API_VERSION_MINOR(InstanceVersion)).c_str();
                Message += "\n\nLumina requires Vulkan 1.4 with dynamic rendering, synchronization2, descriptor indexing, "
                    "buffer device address, and timeline semaphores.";
                ShowVulkanInitFailure("Vulkan Device Selection Failed", Message);
                std::abort();
            }

            GDevice->PhysicsDevice = Best;
            vkGetPhysicalDeviceProperties(Best, &GDevice->Properties);

            // Asked once more, this time reporting. The selection loop stayed quiet so that a
            // rejected candidate's limits could never be mistaken for the chosen device's.
            const FDeviceSuitability Chosen = EvaluateDeviceSuitability(Best, DeviceDesc.bHeadless, true);

            LOG_DISPLAY("Selected GPU '{}'.", GDevice->Properties.deviceName);

            GDevice->bMeshShaderSupported     = true;
            GDevice->MeshRequiredSubgroupSize = Chosen.MeshRequiredSubgroupSize;

            // A driver reporting UINT32_MAX here means "no limit", but every ceil-divide and
            // sub-draw stride derived from it then overflows.
            constexpr uint32 kMaxMeshGroupsPerDraw = 1u << 24;
            GDevice->MaxMeshWorkGroupCountX = (Chosen.MaxMeshWorkGroupCount < kMaxMeshGroupsPerDraw)
                                            ? Chosen.MaxMeshWorkGroupCount
                                            : kMaxMeshGroupsPerDraw;
        }

        bool bDeviceFault    = false;
        bool bNvDiagnostics  = false;
        bool bBufferMarker   = false;
        bool bMemoryPriority = false;
        bool bPipelineStats  = false;   // editor-only; see FDevice::bPipelineStats
        {
            uint32 ExtCount = 0;
            vkEnumerateDeviceExtensionProperties(GDevice->PhysicsDevice, nullptr, &ExtCount, nullptr);
            TVector<VkExtensionProperties> Available(ExtCount);
            vkEnumerateDeviceExtensionProperties(GDevice->PhysicsDevice, nullptr, &ExtCount, Available.data());

            auto HasExtension = [&](const char* Name)
            {
                for (const VkExtensionProperties& Ext : Available)
                {
                    if (strcmp(Ext.extensionName, Name) == 0)
                    {
                        return true;
                    }
                }
                return false;
            };

            if (!DeviceDesc.bHeadless)
            {
                // Presence is a selection requirement, so a device that got this far has it.
                GDevice->EnabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            }

            auto EnableIfPresent = [&](const char* Name)
            {
                if (HasExtension(Name))
                {
                    GDevice->EnabledDeviceExtensions.push_back(Name);
                    return true;
                }
                return false;
            };

            EnableIfPresent(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
            EnableIfPresent(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
            bNvDiagnostics  = EnableIfPresent(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
            bDeviceFault    = EnableIfPresent(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            bBufferMarker   = EnableIfPresent(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
            const bool bLayerKnowsUnifiedLayouts = ValidationLayerVersion == 0 || ValidationLayerVersion >= VK_MAKE_API_VERSION(0, 1, 4, 311);
            GDevice->bUnifiedImageLayouts = bLayerKnowsUnifiedLayouts && EnableIfPresent(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
            bMemoryPriority = EnableIfPresent(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
            if (bMemoryPriority)
            {
                EnableIfPresent(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
            }

            // Mesh/task shader pipeline. Presence, feature and limits were all required to get
            // through selection, so this only has to put the extension on the enable list.
            EnableIfPresent(VK_EXT_MESH_SHADER_EXTENSION_NAME);

#if USING(WITH_EDITOR)
            bPipelineStats = EnableIfPresent(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
#endif

            for (const Native::FDeviceCreationRequest& Request : GPendingDeviceRequests)
            {
                for (const char* Name : Request.DeviceExtensions)
                {
                    bool bAlready = false;
                    for (const char* Existing : GDevice->EnabledDeviceExtensions)
                    {
                        if (strcmp(Existing, Name) == 0)
                        {
                            bAlready = true;
                            break;
                        }
                    }
                    if (!bAlready && HasExtension(Name))
                    {
                        GDevice->EnabledDeviceExtensions.push_back(Name);
                    }
                }
            }
        }
        
        VkPhysicalDeviceVulkan14Features Supported14{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
        VkPhysicalDeviceVulkan13Features Supported13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &Supported14 };
        VkPhysicalDeviceVulkan12Features Supported12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &Supported13 };
        VkPhysicalDeviceVulkan11Features Supported11{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &Supported12 };
        VkPhysicalDeviceFeatures2        Supported2 { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,          .pNext = &Supported11 };
        vkGetPhysicalDeviceFeatures2(GDevice->PhysicsDevice, &Supported2);

        // Mesh shader support, its limits, and the subgroup pinning they may need were all settled
        // during selection: a device that could not satisfy them was never a candidate. What that
        // decided is already on GDevice, so nothing is re-queried here.

        VkPhysicalDeviceFeatures Features10             = {};
        Features10.fragmentStoresAndAtomics             = VK_TRUE;
        Features10.samplerAnisotropy                    = VK_TRUE;
        Features10.sampleRateShading                    = VK_TRUE;
        Features10.fillModeNonSolid                     = VK_TRUE;
        Features10.imageCubeArray                       = VK_TRUE;
        Features10.multiViewport                        = VK_TRUE;
        Features10.multiDrawIndirect                    = VK_TRUE;
        Features10.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        Features10.shaderStorageImageReadWithoutFormat  = VK_TRUE;
        Features10.shaderStorageImageExtendedFormats    = VK_TRUE;
        Features10.drawIndirectFirstInstance            = VK_TRUE;
        Features10.vertexPipelineStoresAndAtomics       = VK_TRUE;
        Features10.shaderInt16                          = VK_TRUE;
        Features10.shaderInt64                          = VK_TRUE;
        Features10.independentBlend                     = VK_TRUE;
        Features10.pipelineStatisticsQuery              = VK_TRUE;
        Features10.wideLines                            = Supported2.features.wideLines;
        Features10.geometryShader                       = Supported2.features.geometryShader;

        VkPhysicalDeviceVulkan11Features Features11{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        Features11.shaderDrawParameters   = VK_TRUE;
        Features11.multiview              = VK_TRUE;
        Features11.storageBuffer16BitAccess = Supported11.storageBuffer16BitAccess;

        VkPhysicalDeviceVulkan12Features Features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        Features12.timelineSemaphore                            = VK_TRUE;
        Features12.drawIndirectCount                            = VK_TRUE;
        Features12.bufferDeviceAddress                          = VK_TRUE;

        if (Supported12.bufferDeviceAddressCaptureReplay && FRenderDoc::IsAttached())
        {
            Features12.bufferDeviceAddressCaptureReplay = VK_TRUE;
            LOG_DISPLAY("RenderDoc attached: enabling bufferDeviceAddressCaptureReplay so captures replay.");
        }
        Features12.descriptorIndexing                           = VK_TRUE;
        Features12.descriptorBindingPartiallyBound              = VK_TRUE;
        Features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        Features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        Features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        Features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        Features12.descriptorBindingUpdateUnusedWhilePending    = VK_TRUE;
        Features12.samplerFilterMinmax                          = VK_TRUE;
        Features12.runtimeDescriptorArray                       = VK_TRUE;
        Features12.shaderInt8                                   = VK_TRUE;
        Features12.shaderFloat16                                = VK_TRUE;

        VkPhysicalDeviceVulkan13Features Features13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        Features13.dynamicRendering = VK_TRUE;
        Features13.synchronization2 = VK_TRUE;
        Features13.subgroupSizeControl = GDevice->MeshRequiredSubgroupSize != 0;

        VkPhysicalDeviceVulkan14Features Features14{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
        Features14.smoothLines = Supported14.smoothLines;

        // Feature pNext chain assembled back to front.
        void* FeatureChain = nullptr;
        auto Chain = [&FeatureChain](auto& Struct)
        {
            Struct.pNext = FeatureChain;
            FeatureChain = &Struct;
        };

        Chain(Features14);
        Chain(Features13);
        Chain(Features12);
        Chain(Features11);

        VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR UnifiedLayoutFeatures{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR };
        if (GDevice->bUnifiedImageLayouts)
        {
            UnifiedLayoutFeatures.unifiedImageLayouts = VK_TRUE;
            Chain(UnifiedLayoutFeatures);
        }
        
        VkPhysicalDeviceFaultFeaturesEXT DeviceFaultFeatures{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
        if (bDeviceFault)
        {
            DeviceFaultFeatures.deviceFault = VK_TRUE;
            Chain(DeviceFaultFeatures);
            GDevice->CrashTracker->SetDeviceFaultEnabled(true);
        }

        VkPhysicalDeviceMemoryPriorityFeaturesEXT MemoryPriorityFeatures{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT };
        VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT PageableFeatures{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT };
        if (bMemoryPriority)
        {
            MemoryPriorityFeatures.memoryPriority = VK_TRUE;
            Chain(MemoryPriorityFeatures);
            PageableFeatures.pageableDeviceLocalMemory = VK_TRUE;
            Chain(PageableFeatures);
            GDevice->bMemoryPriority = true;
        }

        VkPhysicalDeviceMeshShaderFeaturesEXT MeshShaderFeatures{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        if (GDevice->bMeshShaderSupported)
        {
            MeshShaderFeatures.meshShader = VK_TRUE;
            MeshShaderFeatures.taskShader = VK_FALSE;   // no amplification stage; see MeshletCull.slang
            Chain(MeshShaderFeatures);
        }

        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR PipelineStatsFeatures
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };
        if (bPipelineStats)
        {
            PipelineStatsFeatures.pipelineExecutableInfo = VK_TRUE;
            Chain(PipelineStatsFeatures);
            GDevice->bPipelineStats = true;
        }

        VkDeviceDiagnosticsConfigCreateInfoNV* NvDiagnostics = nullptr;
        if (bNvDiagnostics)
        {
            if (void* Diagnostics = GDevice->CrashTracker->GetDeviceCreatePNext())
            {
                NvDiagnostics = static_cast<VkDeviceDiagnosticsConfigCreateInfoNV*>(Diagnostics);
                NvDiagnostics->pNext = FeatureChain;
                FeatureChain = NvDiagnostics;
            }
        }

        for (const Native::FDeviceCreationRequest& Request : GPendingDeviceRequests)
        {
            if (Request.DeviceCreatePNext == nullptr)
            {
                continue;
            }
            auto* Head = static_cast<VkBaseOutStructure*>(Request.DeviceCreatePNext);
            VkBaseOutStructure* Tail = Head;
            while (Tail->pNext != nullptr)
            {
                Tail = Tail->pNext;
            }
            Tail->pNext = static_cast<VkBaseOutStructure*>(FeatureChain);
            FeatureChain = Head;
        }

        VkPhysicalDeviceFeatures2 Features2
        {
            .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext    = FeatureChain,
            .features = Features10,
        };

        uint32 GraphicsFamily = UINT32_MAX;
        uint32 ComputeFamily  = UINT32_MAX;
        uint32 TransferFamily = UINT32_MAX;
        {
            uint32 FamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(GDevice->PhysicsDevice, &FamilyCount, nullptr);
            TVector<VkQueueFamilyProperties> Families(FamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(GDevice->PhysicsDevice, &FamilyCount, Families.data());

            for (uint32 i = 0; i < FamilyCount; ++i)
            {
                if (GraphicsFamily == UINT32_MAX && (Families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                {
                    GraphicsFamily = i;
                }
            }
            for (uint32 i = 0; i < FamilyCount; ++i)
            {
                if (i != GraphicsFamily && (Families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
                {
                    ComputeFamily = i;
                    break;
                }
            }
            for (uint32 i = 0; i < FamilyCount; ++i)
            {
                if (i != GraphicsFamily && i != ComputeFamily && (Families[i].queueFlags & VK_QUEUE_TRANSFER_BIT))
                {
                    TransferFamily = i;
                    break;
                }
            }

            // Selection already refused any device without one, so reaching this means the two
            // disagree about the same device rather than that the hardware is short of a queue.
            if (GraphicsFamily == UINT32_MAX)
            {
                ShowVulkanInitFailure("Vulkan Device Unsuitable",
                    FString("Selected GPU '") + GDevice->Properties.deviceName
                    + "' exposes no graphics queue family, having passed a check that requires one.");
                std::abort();
            }
        }

        {
            const float Priority = 1.0f;
            TVector<VkDeviceQueueCreateInfo> QueueInfos;
            auto AddQueue = [&](uint32 Family)
            {
                if (Family == UINT32_MAX)
                {
                    return;
                }
                QueueInfos.push_back(VkDeviceQueueCreateInfo
                {
                    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    .queueFamilyIndex = Family,
                    .queueCount       = 1,
                    .pQueuePriorities = &Priority,
                });
            };
            AddQueue(GraphicsFamily);
            AddQueue(ComputeFamily);
            AddQueue(TransferFamily);

            VkDeviceCreateInfo DeviceInfo
            {
                .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .pNext                   = &Features2,
                .queueCreateInfoCount    = (uint32)QueueInfos.size(),
                .pQueueCreateInfos       = QueueInfos.data(),
                .enabledExtensionCount   = (uint32)GDevice->EnabledDeviceExtensions.size(),
                .ppEnabledExtensionNames = GDevice->EnabledDeviceExtensions.data(),
            };

            const VkResult DeviceResult = vkCreateDevice(GDevice->PhysicsDevice, &DeviceInfo, nullptr, &GDevice->Device);
            if (DeviceResult != VK_SUCCESS)
            {
                ShowVulkanInitFailure("Vulkan Device Creation Failed",
                    FString("Failed to create the Vulkan logical device on '") + GDevice->Properties.deviceName +
                    "'.\n\nReason: " + Vulkan::VkResultToString(DeviceResult));
                std::abort();
            }

            volkLoadDevice(GDevice->Device);
        }

        GDevice->CrashTracker->Initialize(GDevice->Device, GDevice->PhysicsDevice);

        // After volkLoadDevice, since it needs vkCmdWriteBufferMarkerAMD resolved.
        if (bBufferMarker)
        {
            GDevice->Breadcrumbs.Initialize(GDevice->Device, GDevice->PhysicsDevice);

            CrashHandler::AddDiagnosticProvider([]
            {
                if (GDevice != nullptr)
                {
                    (void)GDevice->Breadcrumbs.ReportOutstanding();
                }
            });
        }
        else
        {
            LOG_WARN("VK_AMD_buffer_marker not present; a device loss will not report which pass was running.");
        }

        {
            VkQueue GraphicsQueue = VK_NULL_HANDLE;
            vkGetDeviceQueue(GDevice->Device, GraphicsFamily, 0, &GraphicsQueue);

            VkQueue ComputeQueue = GraphicsQueue;
            uint32  ComputeQueueFamily = GraphicsFamily;
            if (ComputeFamily != UINT32_MAX)
            {
                vkGetDeviceQueue(GDevice->Device, ComputeFamily, 0, &ComputeQueue);
                ComputeQueueFamily = ComputeFamily;
            }
            else
            {
                LOG_DISPLAY("No dedicated compute queue family; routing compute submissions to the graphics queue.");
            }

            VkQueue TransferQueue = GraphicsQueue;
            uint32  TransferQueueFamily = GraphicsFamily;
            if (TransferFamily != UINT32_MAX)
            {
                vkGetDeviceQueue(GDevice->Device, TransferFamily, 0, &TransferQueue);
                TransferQueueFamily = TransferFamily;
            }
            else
            {
                LOG_DISPLAY("No dedicated transfer queue family; routing transfer submissions to a shared queue.");
            }

            GDevice->Queues[(uint32)EQueueType::Graphics] = GraphicsQueue;
            GDevice->Queues[(uint32)EQueueType::Compute]  = ComputeQueue;
            GDevice->Queues[(uint32)EQueueType::Transfer] = TransferQueue;

            GDevice->QueueFamilies[(uint32)EQueueType::Graphics] = GraphicsFamily;
            GDevice->QueueFamilies[(uint32)EQueueType::Compute]  = ComputeQueueFamily;
            GDevice->QueueFamilies[(uint32)EQueueType::Transfer] = TransferQueueFamily;

            GDevice->QueueLockIndex[(uint32)EQueueType::Graphics] = (uint32)EQueueType::Graphics;
            GDevice->QueueLockIndex[(uint32)EQueueType::Compute]  = (ComputeFamily != UINT32_MAX)
                ? (uint32)EQueueType::Compute  : (uint32)EQueueType::Graphics;
            GDevice->QueueLockIndex[(uint32)EQueueType::Transfer] = (TransferFamily != UINT32_MAX)
                ? (uint32)EQueueType::Transfer : (uint32)EQueueType::Graphics;

            GDevice->bHasAsyncComputeQueue  = (ComputeFamily != UINT32_MAX);
            GDevice->bHasAsyncTransferQueue = (TransferFamily != UINT32_MAX);

            GDevice->NumSharedQueueFamilies = 0;
            GDevice->SharedQueueFamilies[GDevice->NumSharedQueueFamilies++] = GraphicsFamily;
            if (ComputeFamily != UINT32_MAX)
            {
                GDevice->SharedQueueFamilies[GDevice->NumSharedQueueFamilies++] = ComputeFamily;
            }
            if (TransferFamily != UINT32_MAX)
            {
                GDevice->SharedQueueFamilies[GDevice->NumSharedQueueFamilies++] = TransferFamily;
            }

            LOG_DISPLAY("Queue families: graphics {}, compute {}, transfer {}. Async compute {}, async transfer {}.",
                GraphicsFamily, ComputeQueueFamily, TransferQueueFamily,
                GDevice->bHasAsyncComputeQueue  ? "available" : "unavailable (aliased to graphics)",
                GDevice->bHasAsyncTransferQueue ? "available" : "unavailable (aliased to graphics)");
        }

        {
            VmaVulkanFunctions Functions = {};
            Functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
            Functions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

            VmaAllocatorCreateInfo AllocatorInfo = {};
            AllocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
            AllocatorInfo.instance         = GDevice->Instance;
            AllocatorInfo.physicalDevice   = GDevice->PhysicsDevice;
            AllocatorInfo.device           = GDevice->Device;
            AllocatorInfo.pVulkanFunctions = &Functions;
            AllocatorInfo.flags            = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT | VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
            if (GDevice->bMemoryPriority)
            {
                AllocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
            }

            VK_CHECK(vmaCreateAllocator(&AllocatorInfo, &GDevice->Allocator));
        }

        {
            const uint32 APIVer = GDevice->Properties.apiVersion;
            LOG_TRACE("Vulkan RHI - {} - API: {}.{}.{} - Validation: {}", GDevice->Properties.deviceName,
                VK_API_VERSION_MAJOR(APIVer), VK_API_VERSION_MINOR(APIVer), VK_API_VERSION_PATCH(APIVer), DeviceDesc.bValidation);
        }

        if (DeviceDesc.bValidation || DeviceDesc.bDebugUtils)
        {
            if (vkSetDebugUtilsObjectNameEXT != nullptr)
            {
                LOG_TRACE("Vulkan RHI - debug-utils object naming active (crash reports resolve resources by name).");
            }
            else
            {
                LOG_WARN("Vulkan RHI - VK_EXT_debug_utils requested but vkSetDebugUtilsObjectNameEXT is unavailable; "
                         "GPU crash reports will show unnamed resources.");
            }
        }

        constexpr auto Flags =    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                                | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                                | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        
        VkDescriptorBindingFlags VariableFlag[] = 
        {
            Flags,
            Flags,
            Flags,
        };
        
        VkDescriptorSetLayoutBindingFlagsCreateInfo BindingFlags
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .pNext = nullptr,
            .bindingCount = std::size(VariableFlag),
            .pBindingFlags = VariableFlag
        };
        
        VkDescriptorPoolSize Pools[] =
        {
            {
                .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .descriptorCount = kMaxTextureHeapSize
            },
            {
                .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = kMaxTextureHeapSize
            },
            {
                .type = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = kMaxNumSamplers
            }
        };
        
        VkDescriptorSetLayoutBinding Bindings[] = 
        {
            {
                .binding            = kSamplerBindingSlot,
                .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount    = kMaxNumSamplers,
                .stageFlags         = VK_SHADER_STAGE_ALL,
                .pImmutableSamplers = nullptr,
            },
            {
                .binding            = kImageBindingSlot,
                .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .descriptorCount    = kMaxTextureHeapSize,
                .stageFlags         = VK_SHADER_STAGE_ALL,
                .pImmutableSamplers = nullptr
            },
            {
                .binding            = kRWImageBindingSlot,
                .descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount    = kMaxTextureHeapSize,
                .stageFlags         = VK_SHADER_STAGE_ALL,
                .pImmutableSamplers = nullptr
            },
        };
        
        VkDescriptorSetLayoutCreateInfo LayoutInfo
        {
            .sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext          = &BindingFlags,
            .flags          = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount   = std::size(Bindings),
            .pBindings      = Bindings
        };
        
        VkDescriptorPoolCreateInfo PoolInfo
        {
            .sType          = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext          = nullptr,
            .flags          = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets        = kMaxNumTextureHeaps,
            .poolSizeCount  = std::size(Pools),
            .pPoolSizes     = Pools
        };

        vkCreateDescriptorSetLayout(*GDevice, &LayoutInfo, nullptr, &GDevice->DescriptorLayout);
        vkCreateDescriptorPool(*GDevice, &PoolInfo, nullptr, &GDevice->DescriptorPool);
        
        VkPushConstantRange PushConstantRanges
        {
            .stageFlags = VK_SHADER_STAGE_ALL,
            .offset = 0,
            .size = sizeof(VkDeviceAddress)
        };
        
        VkPipelineLayoutCreateInfo CreateInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = &GDevice->DescriptorLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &PushConstantRanges
        };
        
        VK_CHECK(vkCreatePipelineLayout(*GDevice, &CreateInfo, nullptr, &GDevice->PipelineLayout));

        for (uint32 QueueIndex = 0; QueueIndex < 3; ++QueueIndex)
        {
            VkCommandPoolCreateInfo TransientPoolInfo
            {
                .sType              = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext              = nullptr,
                .flags              = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex   = GDevice->QueueFamilies[QueueIndex]
            };
            VK_CHECK(vkCreateCommandPool(*GDevice, &TransientPoolInfo, nullptr, &GDevice->TransientPools[QueueIndex]));
        }

#if defined(TRACY_ENABLE)
        {
            uint32 FamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(GDevice->PhysicsDevice, &FamilyCount, nullptr);
            TVector<VkQueueFamilyProperties> Families(FamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(GDevice->PhysicsDevice, &FamilyCount, Families.data());

            auto CreateQueueContext = [&](EQueueType Queue, const char* Name, uint32 NameLength)
            {
                const uint32 QueueIndex  = (uint32)Queue;
                const uint32 QueueFamily = GDevice->QueueFamilies[QueueIndex];

                if (QueueFamily >= FamilyCount || Families[QueueFamily].timestampValidBits == 0)
                {
                    LOG_WARN("Queue '{}' reports no valid timestamp bits; GPU zones disabled for it.", Name);
                    return;
                }

                VkCommandBufferAllocateInfo TracyAllocInfo
                {
                    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                    .commandPool        = GDevice->TransientPools[QueueIndex],
                    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                    .commandBufferCount = 1
                };
                VkCommandBuffer TracyCmd = VK_NULL_HANDLE;
                VK_CHECK(vkAllocateCommandBuffers(*GDevice, &TracyAllocInfo, &TracyCmd));

                tracy::VkCtx* Context = TracyVkContext(GDevice->PhysicsDevice, GDevice->Device,
                    GDevice->Queues[QueueIndex], TracyCmd);
                TracyVkContextName(Context, Name, NameLength);

                GTracyGPUContexts[QueueIndex]   = Context;
                GTracyOwnedContexts[QueueIndex] = Context;

                // TEMPORARY DIAGNOSTIC (leaked query pool at vkDestroyDevice).
                LOG_DISPLAY("Tracy ctx '{}' queue {} -> ctx {:#x}, query pool {:#x}",
                    Name, QueueIndex, (uint64)(uintptr_t)Context, (uint64)Context->GetQueryPool());

                vkFreeCommandBuffers(*GDevice, GDevice->TransientPools[QueueIndex], 1, &TracyCmd);
            };

            CreateQueueContext(EQueueType::Graphics, "Graphics", 8);

            if (GDevice->bHasAsyncComputeQueue)
            {
                CreateQueueContext(EQueueType::Compute, "Async Compute", 13);
            }
            else
            {
                GTracyGPUContexts[(uint32)EQueueType::Compute] = GTracyGPUContexts[(uint32)EQueueType::Graphics];
            }
        }
#endif

        GDevice->Semaphores.SetDtor([](FSemaphore* Semaphore)
        {
            vkDestroySemaphore(*GDevice, *Semaphore, nullptr);
            
            Semaphore->~FSemaphore();
        });
        
        GDevice->Pipelines.SetDtor([](FPipeline* Pipeline)
        {
            vkDestroyPipeline(*GDevice, *Pipeline, nullptr); 
            
            Pipeline->~FPipeline();
        });
        
        GDevice->Textures.SetDtor([](FTexture* Texture)
        {
            if (Texture->DefaultImageView != VK_NULL_HANDLE)
            {
                vkDestroyImageView(*GDevice, Texture->DefaultImageView, nullptr);
            }

            if (!Texture->bSwapchainImage)
            {
                vmaDestroyImage(GDevice->Allocator, Texture->Image, Texture->Allocation);
            }

            Texture->~FTexture();
        });
        
        GDevice->TextureHeaps.SetDtor([](FTextureHeap* Heap)
        {
            vkFreeDescriptorSets(*GDevice, Heap->DescriptorPool, 1, &Heap->DescriptorSet);

            // Sampled slots reference texture-owned views; only RW views and samplers are heap-owned.
            for (VkImageView View : Heap->RWImageViews)
            {
                if (View != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(*GDevice, View, nullptr);
                }
            }

            for (VkSampler Sampler : Heap->Samplers)
            {
                if (Sampler != VK_NULL_HANDLE)
                {
                    vkDestroySampler(*GDevice, Sampler, nullptr);
                }
            }

            Heap->~FTextureHeap();

        });
        
        GDevice->DepthStates.SetDtor([](FDepthStencilState* State)
        {
            State->~FDepthStencilState();
        });

        GDevice->CommandLists.SetDtor([](FCommandList* CommandList)
        {
            vkDestroyCommandPool(*GDevice, CommandList->Pool, nullptr);

            CommandList->~FCommandList();
        });

        GDevice->Swapchains.SetDtor([](FSwapchain* Swapchain)
        {
            for (FTextureH Image : Swapchain->Images)
            {
                GDevice->Textures.Erase(Image);   // external: dtor destroys the view, keeps the VkImage
            }
            for (VkSemaphore Semaphore : Swapchain->AcquireSemaphores)
            {
                vkDestroySemaphore(*GDevice, Semaphore, nullptr);
            }
            for (VkSemaphore Semaphore : Swapchain->PresentSemaphores)
            {
                vkDestroySemaphore(*GDevice, Semaphore, nullptr);
            }
            vkDestroySwapchainKHR(*GDevice, Swapchain->Swapchain, nullptr);
            vkDestroySurfaceKHR(GDevice->Instance, Swapchain->Surface, nullptr);

            Swapchain->~FSwapchain();
        });

        GDevice->Surfaces.SetDtor([](FSurface* Surface)
        {
            // Null once CreateSwapchain has taken ownership; vkDestroySurfaceKHR accepts VK_NULL_HANDLE.
            vkDestroySurfaceKHR(GDevice->Instance, Surface->Surface, nullptr);

            Surface->~FSurface();
        });
    }

    void FreeDevice()
    {
        vkDeviceWaitIdle(*GDevice);

#if defined(TRACY_ENABLE)
        for (uint32 QueueIndex = 0; QueueIndex < 3; ++QueueIndex)
        {
            if (GTracyOwnedContexts[QueueIndex] != nullptr)
            {
                TracyVkDestroy(GTracyOwnedContexts[QueueIndex]);
                GTracyOwnedContexts[QueueIndex] = nullptr;
            }
            GTracyGPUContexts[QueueIndex] = nullptr;
        }
#endif

        if (!GDevice->PendingTransient.empty())
        {
            for (const FDeviceImpl::FPendingTransition& Pending : GDevice->PendingTransient)
            {
                vkFreeCommandBuffers(*GDevice, Pending.Pool, 1, &Pending.Buffer);
            }
            GDevice->PendingTransient.clear();
        }

        FlushPendingHeapDestroysLocked(0, /*bForce*/ true);

        GDevice->Swapchains.Clear();
        GDevice->Surfaces.Clear();
        GDevice->Semaphores.Clear();
        GDevice->Pipelines.Clear();
        GDevice->Textures.Clear();
        GDevice->TextureHeaps.Clear();
        GDevice->DepthStates.Clear();
        GDevice->CommandLists.Clear();

        for (FMemoryBlock& Block : GDevice->MemoryBlocks)
        {
            vmaDestroyBuffer(GDevice->Allocator, Block.Buffer, Block.Allocation);
        }
        GDevice->MemoryBlocks.clear();

        GDevice->Breadcrumbs.Shutdown(GDevice->Device);

        for (VkCommandPool TransientPool : GDevice->TransientPools)
        {
            vkDestroyCommandPool(*GDevice, TransientPool, nullptr);
        }
        vkDestroyPipelineLayout(*GDevice, GDevice->PipelineLayout, nullptr);
        vkDestroyDescriptorPool(*GDevice, GDevice->DescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(*GDevice, GDevice->DescriptorLayout, nullptr);

        GDevice->CrashTracker->Shutdown();
        GDevice->CrashTracker = nullptr;

        vmaDestroyAllocator(GDevice->Allocator);
        vkDestroyDevice(GDevice->Device, nullptr);

        if (GDevice->DebugMessenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr)
        {
            vkDestroyDebugUtilsMessengerEXT(GDevice->Instance, GDevice->DebugMessenger, nullptr);
        }
        vkDestroyInstance(GDevice->Instance, nullptr);

        delete GDevice;
        GDevice = nullptr;
    }

    // Caller holds HeapMutex, or has otherwise guaranteed the device is idle.
    static void FlushPendingHeapDestroysLocked(uint32 Slot, bool bForce)
    {
        for (size_t i = 0; i < GDevice->PendingHeapDestroys.size(); )
        {
            const FDeviceImpl::FPendingHeapDestroy& Pending = GDevice->PendingHeapDestroys[i];
            if (!bForce && Pending.Slot != Slot)
            {
                ++i;
                continue;
            }

            if (Pending.View != VK_NULL_HANDLE)
            {
                vkDestroyImageView(*GDevice, Pending.View, nullptr);
            }
            if (Pending.Sampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(*GDevice, Pending.Sampler, nullptr);
            }

            GDevice->PendingHeapDestroys[i] = GDevice->PendingHeapDestroys.back();
            GDevice->PendingHeapDestroys.pop_back();
        }
    }

    // Backend half of resource retirement. Called from Core::BeginFrame once this slot's queue timelines
    // have been waited, i.e. at the exact point everything recorded into it is known to be done.
    void RetireSlot(uint32 Slot)
    {
        {
            FScopeLock Lock(GDevice->TransientMutex);
            for (size_t i = 0; i < GDevice->PendingTransient.size(); )
            {
                const FDeviceImpl::FPendingTransition& Pending = GDevice->PendingTransient[i];
                if (Pending.Slot == Slot)
                {
                    vkFreeCommandBuffers(*GDevice, Pending.Pool, 1, &Pending.Buffer);
                    GDevice->PendingTransient[i] = GDevice->PendingTransient.back();
                    GDevice->PendingTransient.pop_back();
                }
                else
                {
                    ++i;
                }
            }
        }

        {
            FScopeLock HeapLock(GDevice->HeapMutex);
            FlushPendingHeapDestroysLocked(Slot, /*bForce*/ false);
        }

        GDevice->CurrentRetireSlot.store(Slot, std::memory_order_release);
    }

    void WaitDeviceIdle()
    {
        FAllQueuesLock QueueLock;
        vkDeviceWaitIdle(*GDevice);

        {
            FScopeLock HeapLock(GDevice->HeapMutex);
            FlushPendingHeapDestroysLocked(0, /*bForce*/ true);
        }

        FScopeLock TransientLock(GDevice->TransientMutex);
        if (!GDevice->PendingTransient.empty())
        {
            for (const FDeviceImpl::FPendingTransition& Pending : GDevice->PendingTransient)
            {
                vkFreeCommandBuffers(*GDevice, Pending.Pool, 1, &Pending.Buffer);
            }
            GDevice->PendingTransient.clear();
        }
    }

    uint64 GetSemaphoreValue(FSemaphoreH Semaphore)
    {
        uint64 Value = 0;
        VK_CHECK(vkGetSemaphoreCounterValue(*GDevice, GDevice->Semaphores[Semaphore].Semaphore, &Value));
        return Value;
    }

    void WaitSemaphore(FSemaphoreH Semaphore, uint64 Value)
    {
        VkSemaphore VulkanSemaphore = GDevice->Semaphores[Semaphore].Semaphore;
        
        VkSemaphoreWaitInfo WaitInfo
        {
            .sType              = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext              = nullptr,
            .flags              = 0,
            .semaphoreCount     = 1,
            .pSemaphores        = &VulkanSemaphore,
            .pValues            = &Value
        };
        
        VK_CHECK(vkWaitSemaphores(*GDevice, &WaitInfo, UINT64_MAX));
    }

    GPUPtr Malloc(uint64 Size, uint64 Alignment, EMemoryType Type)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        if (Size == 0)
        {
            return 0;
        }

        VmaAllocationCreateInfo Info = {};
        Info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        switch (Type)
        {
        case EMemoryType::CPUWrite:
            Info.flags  = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            Info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case EMemoryType::CPURead:
            Info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            Info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            Info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case EMemoryType::GPUOnly:
            Info.flags = 0;
            break;
        }
        
        const VkPhysicalDeviceLimits& Limits = GDevice->Properties.limits;
        Alignment = Math::Max<uint64>(Alignment, Math::Max<uint64>(Limits.optimalBufferCopyOffsetAlignment, Limits.nonCoherentAtomSize));
        Size = Math::AlignUp(Size, Alignment);

        if (Size > kDedicatedMemoryThreshold)
        {
            Info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        }
        
        VkBufferCreateInfo SampleInfo = {};
        SampleInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        SampleInfo.size   = Size;
        SampleInfo.usage  = kDefaultBufferUsages;

        if (GDevice->NumSharedQueueFamilies > 1)
        {
            SampleInfo.sharingMode           = VK_SHARING_MODE_CONCURRENT;
            SampleInfo.queueFamilyIndexCount = GDevice->NumSharedQueueFamilies;
            SampleInfo.pQueueFamilyIndices   = GDevice->SharedQueueFamilies.data();
        }
        else
        {
            SampleInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        
        VmaAllocation Allocation = nullptr;
        VmaAllocationInfo AllocationInfo = {};

        VkBuffer VulkanBuffer = VK_NULL_HANDLE;

        const VkResult AllocResult = vmaCreateBufferWithAlignment(GDevice->Allocator, &SampleInfo, &Info, Alignment, &VulkanBuffer, &Allocation, &AllocationInfo);
        if (AllocResult != VK_SUCCESS || VulkanBuffer == VK_NULL_HANDLE || Allocation == nullptr)
        {
            PanicOutOfGPUMemory(::Lumina::Format("a {} KiB {} buffer", Size / 1024, MemoryTypeToString(Type)).c_str(), AllocResult);
        }
        
        // GPU-read memory that fell out of the BAR means every shader/transfer read crosses PCIe.
        if (Type == EMemoryType::CPUWrite)
        {
            VkMemoryPropertyFlags Props = 0;
            vmaGetAllocationMemoryProperties(GDevice->Allocator, Allocation, &Props);
            if ((Props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)
            {
                static bool bWarnedHostFallback = false;
                if (!bWarnedHostFallback)
                {
                    bWarnedHostFallback = true;
                    LOG_WARN("RHI: CPUWrite allocation ({} KiB) landed in host memory instead of the ReBAR heap (budget pressure or ReBAR disabled). GPU reads of it will cross PCIe.", Size / 1024);
                }
            }
        }

        VkBufferDeviceAddressInfo AddressInfo
        {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .pNext  = nullptr,
            .buffer = VulkanBuffer,
        };

        GPUPtr Gpu = vkGetBufferDeviceAddress(*GDevice, &AddressInfo);

        FMemoryBlock Block
        {
            .Buffer     = VulkanBuffer,
            .Allocation = Allocation,
            .Host       = AllocationInfo.pMappedData,
            .Device     = Gpu,
            .Size       = Size
        };

#if USING(WITH_EDITOR)
        Block.Name[0] = '\0';
        Block.MemType = Type;
#endif

        FWriteScopeLock Lock(GDevice->MemoryMutex);
        auto It = std::ranges::lower_bound(GDevice->MemoryBlocks, Gpu, {}, &FMemoryBlock::Device);
        GDevice->MemoryBlocks.insert(It, Block);

        return Block.Device;
    }

    GPUPtr Malloc(uint64 Size, EMemoryType Type)
    {
        return Malloc(Size, 16, Type);
    }

    void* ToHost(GPUPtr GPU)
    {
        FReadScopeLock Lock(GDevice->MemoryMutex);
        const FMemoryBlock* Block = FindMemory(GPU);

        // GPU-only memory has no mapping
        if (Block != nullptr && Block->Host != nullptr)
        {
            return static_cast<std::byte*>(Block->Host) + (GPU - Block->Device);
        }

        return nullptr;
    }

    static void NameObject(VkObjectType Type, uint64 Handle, const char* Name)
    {
        if (vkSetDebugUtilsObjectNameEXT == nullptr || Name == nullptr || Handle == 0)
        {
            return;
        }

        const VkDebugUtilsObjectNameInfoEXT Info
        {
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .pNext        = nullptr,
            .objectType   = Type,
            .objectHandle = Handle,
            .pObjectName  = Name
        };

        vkSetDebugUtilsObjectNameEXT(*GDevice, &Info);
    }

    void SetDebugName(GPUPtr GPU, const char* Name)
    {
        if (GDevice == nullptr)
        {
            return;
        }

#if USING(WITH_EDITOR)
        // Exclusive: this branch writes the name back into the block.
        FWriteScopeLock Lock(GDevice->MemoryMutex);

        if (FMemoryBlock* Block = FindMemoryMutable(GPU))
        {
            NameObject(VK_OBJECT_TYPE_BUFFER, (uint64)Block->Buffer, Name);
            CopyBlockName(Block->Name, Name);
        }
#else
        FReadScopeLock Lock(GDevice->MemoryMutex);

        if (const FMemoryBlock* Block = FindMemory(GPU))
        {
            NameObject(VK_OBJECT_TYPE_BUFFER, (uint64)Block->Buffer, Name);
        }
#endif
    }

    void SetDebugName(FTextureH Texture, const char* Name)
    {
        if (GDevice == nullptr || !IsValid(Texture))
        {
            return;
        }

        FTexture& TextureData = GDevice->Textures[Texture];

        NameObject(VK_OBJECT_TYPE_IMAGE, (uint64)TextureData.Image, Name);

#if USING(WITH_EDITOR)
        FScopeLock Lock(GDevice->TextureLedgerMutex);
        auto It = GDevice->TextureLedger.find(Texture.Handle);
        if (It != GDevice->TextureLedger.end())
        {
            CopyBlockName(It->second.Name, Name);
        }
#endif
    }

    void Free(GPUPtr GPU)
    {
        if (GDevice == nullptr)
        {
            return;
        }

        FWriteScopeLock Lock(GDevice->MemoryMutex);
        auto It = std::ranges::lower_bound(GDevice->MemoryBlocks, GPU, {}, &FMemoryBlock::Device);

        if (It != GDevice->MemoryBlocks.end() && It->Device == GPU)
        {
#if USING(WITH_EDITOR)
            RecordFreedBlockLocked(*It);
#endif
            vmaDestroyBuffer(GDevice->Allocator, It->Buffer, It->Allocation);
            GDevice->MemoryBlocks.erase(It);
        }
    }

    FString DescribeDeviceAddress(uint64 AddressLow, uint64 AddressHigh)
    {
        #if USING(WITH_EDITOR)
        if (GDevice == nullptr || AddressHigh < AddressLow)
        {
            return {};
        }

        // Past this, the "nearest" allocation is just whichever one happened to be closest in a 40-bit
        // address space, which reads as evidence and is not.
        constexpr uint64 kNeighborWindow = 64ull * 1024 * 1024;

        // try_lock, not a lock: this runs from the device-lost path, and another thread stuck mid-Malloc
        // would otherwise turn a crash report into a hang. A missed report is the better failure.
        FReadScopeLock Lock(GDevice->MemoryMutex, TryToLock);
        if (!Lock.OwnsLock())
        {
            return "allocation ledger was locked by another thread; no attribution";
        }

        const uint64 CurrentSubmit = GDevice->SubmitOrdinal.load(std::memory_order_relaxed);

        // The fault address carries only page precision, so the question is whether the allocation
        // INTERSECTS the reported window -- an exact-address probe misses by up to a page.
        const auto Overlaps = [&](GPUPtr Base, uint64 Size)
        {
            return Size != 0ull && Base <= AddressHigh && AddressLow < Base + Size;
        };

        const auto NameOf = [](const char* Name) { return (Name != nullptr && Name[0] != '\0') ? Name : "<unnamed>"; };

        // Nearest allocation that does NOT contain the address, either side. An overrun lands past the end
        // of a buffer and inside nobody, which is the case a containment-only lookup reports as "unknown".
        struct FNeighbor
        {
            bool        bValid   = false;
            bool        bFreed   = false;
            bool        bPastEnd = false;   // the allocation ends before the fault: an overrun off its end
            uint64      Distance = 0;
            GPUPtr      Base     = 0;
            uint64      Size     = 0;
            uint64      Submit   = 0;
            const char* Name     = nullptr;
        };

        FNeighbor Nearest;

        const auto ConsiderNeighbor = [&](GPUPtr Base, uint64 Size, const char* Name, bool bFreed, uint64 Submit)
        {
            if (Size == 0ull)
            {
                return;
            }

            bool   bPastEnd  = false;
            uint64 Distance  = 0;

            if (Base + Size <= AddressLow)
            {
                bPastEnd = true;
                Distance = AddressLow - (Base + Size);
            }
            else if (Base > AddressHigh)
            {
                Distance = Base - AddressHigh;
            }
            else
            {
                return;   // overlapping; containment is reported instead
            }

            if (Distance > kNeighborWindow || (Nearest.bValid && Distance >= Nearest.Distance))
            {
                return;
            }

            Nearest = FNeighbor{ true, bFreed, bPastEnd, Distance, Base, Size, Submit, Name };
        };

        // Linear over both tables. This runs exactly once, on a dying device: clarity beats a clever
        // bound, and a large allocation whose START sorts far below the fault defeats one anyway.
        for (const FMemoryBlock& Block : GDevice->MemoryBlocks)
        {
            if (Overlaps(Block.Device, Block.Size))
            {
                return FString(::Lumina::Format("LIVE \"{}\" [{:#x} +{:#x}], fault {:#x} into it",
                                           NameOf(Block.Name), Block.Device, Block.Size,
                                           AddressLow - Block.Device).c_str());
            }

            ConsiderNeighbor(Block.Device, Block.Size, Block.Name, /*bFreed*/ false, 0ull);
        }

        for (const FFreedBlock& Entry : GDevice->FreedBlocks)
        {
            if (Overlaps(Entry.Device, Entry.Size))
            {
                return FString(::Lumina::Format("FREED \"{}\" [{:#x} +{:#x}], fault {:#x} into it, freed {} submit(s) before the loss",
                                           NameOf(Entry.Name), Entry.Device, Entry.Size,
                                           AddressLow - Entry.Device,
                                           CurrentSubmit - Entry.SubmitOrdinal).c_str());
            }

            ConsiderNeighbor(Entry.Device, Entry.Size, Entry.Name, /*bFreed*/ true, Entry.SubmitOrdinal);
        }

        if (!Nearest.bValid)
        {
            return FString(::Lumina::Format("no live or freed allocation within {} MiB",
                                       kNeighborWindow / (1024ull * 1024ull)).c_str());
        }

        const char* Relation = Nearest.bPastEnd ? "past the end of" : "before the start of";

        if (Nearest.bFreed)
        {
            return FString(::Lumina::Format("{:#x} {} FREED \"{}\" [{:#x} +{:#x}], freed {} submit(s) before the loss",
                                       Nearest.Distance, Relation, NameOf(Nearest.Name), Nearest.Base, Nearest.Size,
                                       CurrentSubmit - Nearest.Submit).c_str());
        }

        return FString(::Lumina::Format("{:#x} {} LIVE \"{}\" [{:#x} +{:#x}]",
                                   Nearest.Distance, Relation, NameOf(Nearest.Name),
                                   Nearest.Base, Nearest.Size).c_str());
        #else
        (void)AddressLow;
        (void)AddressHigh;
        return {};
        #endif
    }

    bool GetAllocationRange(GPUPtr Ptr, GPUPtr& OutBase, uint64& OutSize)
    {
        if (GDevice == nullptr || Ptr == 0)
        {
            return false;
        }

        FReadScopeLock Lock(GDevice->MemoryMutex);

        const FMemoryBlock* Block = FindMemory(Ptr);
        if (Block == nullptr)
        {
            return false;
        }

        OutBase = Block->Device;
        OutSize = Block->Size;
        return true;
    }

    // FreeH after FreeDevice is a no-op: everything was already destroyed with the device.

    void FreeH(FSemaphoreH Semaphore)
    {
        if (GDevice != nullptr)
        {
            GDevice->Semaphores.Erase(Semaphore);
        }
    }

    void FreeH(FPipelineH Pipeline)
    {
        if (GDevice != nullptr)
        {
            GDevice->Pipelines.Erase(Pipeline);
        }
    }

    static void PointSampledSlotAtFallback(FTextureHeap& HeapData, uint32 Slot);

    void FreeH(FTextureH Texture)
    {
        LUMINA_PROFILE_SECTION("RHI::FreeTexture");

        if (GDevice != nullptr)
        {
            // Tripwire, and the last line of defense for the whole bindless design. A ResourceID is a bare
            // uint32: nothing in the descriptor heap keeps a texture alive, and PARTIALLY_BOUND +
            // UPDATE_AFTER_BIND suppress every layer check, so a slot left naming a destroyed image is
            // silent until the GPU page-faults inside a SampleGrad with "failed to translate". Every
            // release path is supposed to unbind first (Textures::Release -> RetireSampledSlot, and
            // Release/RetireSceneImage). If one does not, sample magenta and say so loudly rather than
            // hand the GPU a freed address.
            {
                FScopeLock Lock(GDevice->HeapMutex);
                FTexture& TextureData = GDevice->Textures[Texture];
                FTextureHeap* HeapData = TextureData.BoundSampledSlot != kInvalidHeapSlot
                    ? GDevice->TextureHeaps.TryGet(TextureData.BoundHeap) : nullptr;

                // The slot is a hint and can be stale (the heap may have moved on to another image, or the
                // heap itself may be gone at teardown). The heap's own record is the authority.
                if (HeapData != nullptr
                    && TextureData.BoundSampledSlot < HeapData->SampledOwners.size()
                    && HeapData->SampledOwners[TextureData.BoundSampledSlot].Handle == Texture.Handle)
                {
                    LOG_ERROR("RHI: destroying a {}x{} {}-mip texture (format {}) while bindless slot {} still "
                              "names it. The slot is being repointed at the fallback; whichever release path "
                              "skipped the unbind is a use-after-free waiting to happen.",
                        TextureData.Desc.Dimension.x, TextureData.Desc.Dimension.y, TextureData.Desc.MipCount,
                        (uint32)TextureData.Desc.Format, TextureData.BoundSampledSlot);

                    PointSampledSlotAtFallback(*HeapData, TextureData.BoundSampledSlot);
                }
            }

            {
                FScopeLock Lock(GDevice->InitMutex);
                TVector<FTextureH>& Pending = GDevice->UninitializedTextures;
                for (size_t i = 0; i < Pending.size(); )
                {
                    if (Pending[i].Handle == Texture.Handle)
                    {
                        Pending[i] = Pending.back();
                        Pending.pop_back();
                    }
                    else
                    {
                        ++i;
                    }
                }
                GDevice->PendingImageInits.store((uint32)Pending.size(), std::memory_order_release);
            }

#if USING(WITH_EDITOR)
            {
                FScopeLock Lock(GDevice->TextureLedgerMutex);
                GDevice->TextureLedger.erase(Texture.Handle);
            }
#endif

            GDevice->Textures.Erase(Texture);
        }
    }

    void FreeH(FTextureHeapH Heap)
    {
        if (GDevice != nullptr)
        {
            // Textures outlive the heap at shutdown (Core::Shutdown frees the heap, then asset destructors
            // keep running). Drop the back-references first or FreeH's tripwire would index a dead heap.
            {
                FScopeLock Lock(GDevice->HeapMutex);
                FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];
                for (FTextureH& Owner : HeapData.SampledOwners)
                {
                    if (IsValid(Owner))
                    {
                        if (FTexture* OwnerData = GDevice->Textures.TryGet(Owner))
                        {
                            OwnerData->BoundSampledSlot = kInvalidHeapSlot;
                        }
                        Owner = {};
                    }
                }
            }

            GDevice->TextureHeaps.Erase(Heap);
        }
    }

    void FreeH(FDepthStencilH DepthStencil)
    {
        if (GDevice != nullptr)
        {
            GDevice->DepthStates.Erase(DepthStencil);
        }
    }

    FDepthStencilH CreateDepthStencil(const FDepthStencilDesc& Desc)
    {
        return GDevice->DepthStates.Emplace(Desc);
    }

    FPipelineH CreateGraphicsPipeline(const FShaderSource& Vertex, const FShaderSource& Fragment, const FRasterDesc& Desc, TSpan<const FSpecializationConstant> Constants)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        VkShaderModuleCreateInfo VertInfo
        {
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext    = nullptr,
            .flags    = 0,
            .codeSize = Vertex.Source.size(),
            .pCode    = reinterpret_cast<const uint32*>(Vertex.Source.data()),
        };
        
        VkShaderModule VertModule;
        VK_CHECK(vkCreateShaderModule(*GDevice, &VertInfo, nullptr, &VertModule));
        
        // Fragment stage is optional.
        VkShaderModule FragModule = VK_NULL_HANDLE;
        if (!Fragment.Source.empty())
        {
            VkShaderModuleCreateInfo FragInfo
            {
                .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext    = nullptr,
                .flags    = 0,
                .codeSize = Fragment.Source.size(),
                .pCode    = reinterpret_cast<const uint32*>(Fragment.Source.data()),
            };

            VK_CHECK(vkCreateShaderModule(*GDevice, &FragInfo, nullptr, &FragModule));
        }

        FMemMark Mark;
        const VkSpecializationInfo SpecializationInfo = ConstructSpecializationInfo(Mark, Constants);
        
        VkPipelineShaderStageCreateInfo Stages[] = 
        {
            VkPipelineShaderStageCreateInfo
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_VERTEX_BIT,
                .module              = VertModule,
                .pName               = "main",
                .pSpecializationInfo = &SpecializationInfo,
            },
            VkPipelineShaderStageCreateInfo
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module              = FragModule,
                .pName               = "main",
                .pSpecializationInfo = &SpecializationInfo,
            },
        };
        
        VkPipelineVertexInputStateCreateInfo VertexInputAssembly
        {
            .sType                              = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext                              = nullptr,
            .flags                              = 0,
            .vertexBindingDescriptionCount      = 0,
            .pVertexBindingDescriptions         = nullptr,
            .vertexAttributeDescriptionCount    = 0,
            .pVertexAttributeDescriptions       = nullptr,
        };
        
        VkPipelineInputAssemblyStateCreateInfo InputAssemblyState
        {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext                  = nullptr,
            .flags                  = 0,
            .topology               = ToVkTopology(Desc.Topology),
            .primitiveRestartEnable = false,
        };
        
        VkFormat DepthAttachmentFormat   = ConvertFormat(Desc.DepthFormat);
        VkFormat StencilAttachmentFormat = ConvertFormat(Desc.StencilFormat);

        const uint32 ColorTargetCount = static_cast<uint32>(Desc.ColorTargets.size());

        auto* ColorBlendAttachments = Mark.AllocArray<VkPipelineColorBlendAttachmentState>(ColorTargetCount);
        auto* ColorAttachmentFormats = Mark.AllocArray<VkFormat>(ColorTargetCount);

        for (uint32 i = 0; i < ColorTargetCount; ++i)
        {
            const FBlendDesc& Blend = Desc.ColorTargets[i].Blend;

            ColorBlendAttachments[i].blendEnable         = Blend.bBlendEnable;
            ColorBlendAttachments[i].srcColorBlendFactor = ToVkBlendFactor(Blend.SrcColorFactor);
            ColorBlendAttachments[i].dstColorBlendFactor = ToVkBlendFactor(Blend.DstColorFactor);
            ColorBlendAttachments[i].colorBlendOp        = ToVkBlendOp(Blend.ColorOp);
            ColorBlendAttachments[i].srcAlphaBlendFactor = ToVkBlendFactor(Blend.SrcAlphaFactor);
            ColorBlendAttachments[i].dstAlphaBlendFactor = ToVkBlendFactor(Blend.DstAlphaFactor);
            ColorBlendAttachments[i].alphaBlendOp        = ToVkBlendOp(Blend.AlphaOp);
            ColorBlendAttachments[i].colorWriteMask      = static_cast<VkColorComponentFlags>(Blend.ColorWriteMask & 0xF);

            ColorAttachmentFormats[i] = ConvertFormat(Desc.ColorTargets[i].Format);
        }

        VkPipelineColorBlendStateCreateInfo ColorBlendStates
        {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext           = nullptr,
            .flags           = 0,
            .logicOpEnable   = false,
            .logicOp         = VK_LOGIC_OP_NO_OP,
            .attachmentCount = ColorTargetCount,
            .pAttachments    = ColorBlendAttachments,
            .blendConstants  = {1.f, 1.f, 1.f, 1.f},
        };
        
        VkPipelineRasterizationStateCreateInfo RasterState
        {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .depthClampEnable        = false,
            .rasterizerDiscardEnable = false,
            .polygonMode             = Desc.bWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
            .cullMode                = VK_CULL_MODE_BACK_BIT,
            .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable         = true,
            .depthBiasConstantFactor = 0,
            .depthBiasClamp          = 0,
            .depthBiasSlopeFactor    = 0,
            .lineWidth               = 1.0f,
        };

        const VkSampleCountFlagBits SampleCount = static_cast<VkSampleCountFlagBits>(Desc.SampleCount == 0 ? 1 : Desc.SampleCount);

        VkPipelineMultisampleStateCreateInfo MultiSampleState
        {
            .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .rasterizationSamples  = SampleCount,
            .sampleShadingEnable   = false,
            .minSampleShading      = 1.0f,
            .pSampleMask           = nullptr,
            .alphaToCoverageEnable = Desc.bAlphaToCoverage,
            .alphaToOneEnable      = false,
        };

        VkPipelineDepthStencilStateCreateInfo DepthStencilState
        {
            .sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .depthTestEnable       = VK_FALSE,
            .depthWriteEnable      = VK_FALSE,
            .depthCompareOp        = VK_COMPARE_OP_ALWAYS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable     = VK_FALSE,
            .minDepthBounds        = 0.0f,
            .maxDepthBounds        = 1.0f,
        };

        VkDynamicState DynamicState[] =
        {
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS,
            VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
            VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_LINE_WIDTH,
        };
        
        VkPipelineViewportStateCreateInfo ViewportState
        {
            .sType          = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext          = nullptr,
            .flags          = 0,
            .viewportCount  = 0,
            .pViewports     = nullptr,
            .scissorCount   = 0,
            .pScissors      = nullptr 
        };
        
        VkPipelineDynamicStateCreateInfo DynamicStateInfo
        {
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext             = nullptr,
            .flags             = 0,
            .dynamicStateCount = std::size(DynamicState),
            .pDynamicStates    = DynamicState,
        };
        
        VkPipelineRenderingCreateInfo PipelineCreate
        {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext                   = nullptr,
            .viewMask                = 0,
            .colorAttachmentCount    = ColorTargetCount,
            .pColorAttachmentFormats = ColorAttachmentFormats,
            .depthAttachmentFormat   = DepthAttachmentFormat,
            .stencilAttachmentFormat = StencilAttachmentFormat,
        };
        
        VkGraphicsPipelineCreateInfo CreateInfo
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &PipelineCreate,
            .flags               = GDevice->bPipelineStats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0u,
            .stageCount          = FragModule != VK_NULL_HANDLE ? 2u : 1u,
            .pStages             = Stages,
            .pVertexInputState   = &VertexInputAssembly,
            .pInputAssemblyState = &InputAssemblyState,
            .pTessellationState  = nullptr,
            .pViewportState      = &ViewportState,
            .pRasterizationState = &RasterState,
            .pMultisampleState   = &MultiSampleState,
            .pDepthStencilState  = &DepthStencilState,
            .pColorBlendState    = &ColorBlendStates,
            .pDynamicState       = &DynamicStateInfo,
            .layout              = GDevice->PipelineLayout,
            .renderPass          = VK_NULL_HANDLE,
            .subpass             = 0,
            .basePipelineHandle  = VK_NULL_HANDLE,
            .basePipelineIndex   = 0,
        };
        
        VkPipeline VulkanPipeline;
        VK_CHECK(vkCreateGraphicsPipelines(*GDevice, nullptr, 1, &CreateInfo, nullptr, &VulkanPipeline));

        vkDestroyShaderModule(*GDevice, VertModule, nullptr);
        if (FragModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(*GDevice, FragModule, nullptr);
        }

        return GDevice->Pipelines.Emplace(VulkanPipeline, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }

    bool GetPipelineStatistics(FPipelineH Pipeline, TVector<FPipelineStat>& Out)
    {
        if (GDevice == nullptr || !GDevice->bPipelineStats || Pipeline.Handle == 0)
        {
            return false;
        }

        const FPipeline Resolved = GDevice->Pipelines[Pipeline];
        if (Resolved.Pipeline == VK_NULL_HANDLE)
        {
            return false;
        }

        VkPipelineInfoKHR Info{ .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = Resolved.Pipeline };

        uint32 ExecutableCount = 0;
        if (vkGetPipelineExecutablePropertiesKHR(*GDevice, &Info, &ExecutableCount, nullptr) != VK_SUCCESS || ExecutableCount == 0)
        {
            return false;
        }

        TVector<VkPipelineExecutablePropertiesKHR> Executables(ExecutableCount,
            VkPipelineExecutablePropertiesKHR{ .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR });
        if (vkGetPipelineExecutablePropertiesKHR(*GDevice, &Info, &ExecutableCount, Executables.data()) != VK_SUCCESS)
        {
            return false;
        }

        for (uint32 Index = 0; Index < ExecutableCount; ++Index)
        {
            VkPipelineExecutableInfoKHR ExecInfo
            {
                .sType           = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
                .pipeline        = Resolved.Pipeline,
                .executableIndex = Index,
            };

            uint32 StatCount = 0;
            if (vkGetPipelineExecutableStatisticsKHR(*GDevice, &ExecInfo, &StatCount, nullptr) != VK_SUCCESS || StatCount == 0)
            {
                continue;
            }

            TVector<VkPipelineExecutableStatisticKHR> Stats(StatCount,
                VkPipelineExecutableStatisticKHR{ .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR });
            if (vkGetPipelineExecutableStatisticsKHR(*GDevice, &ExecInfo, &StatCount, Stats.data()) != VK_SUCCESS)
            {
                continue;
            }

            for (const VkPipelineExecutableStatisticKHR& Stat : Stats)
            {
                FPipelineStat& Emitted = Out.emplace_back();
                Emitted.Stage = Executables[Index].name;
                Emitted.Name  = Stat.name;

                switch (Stat.format)
                {
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:   Emitted.Value = Stat.value.b32 ? 1.0 : 0.0;   break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:    Emitted.Value = (double)Stat.value.i64;       break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:   Emitted.Value = (double)Stat.value.u64;       break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:  Emitted.Value = Stat.value.f64;               break;
                    default:                                                   Emitted.Value = 0.0;                          break;
                }
                Emitted.bIsFloat = (Stat.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR);
            }
        }

        return !Out.empty();
    }

    FPipelineH CreateComputePipeline(const FShaderSource& Compute, TSpan<const FSpecializationConstant> Constants)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        // @TODO Decide if we should load this and keep a shader handle instead.
        VkShaderModuleCreateInfo ModuleInfo
        {
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext    = nullptr,
            .flags    = 0,
            .codeSize = Compute.Source.size(),
            .pCode    = reinterpret_cast<const uint32*>(Compute.Source.data()),
        };
        
        VkShaderModule ShaderModule{};
        VK_CHECK(vkCreateShaderModule(*GDevice, &ModuleInfo, nullptr, &ShaderModule));
     
        FMemMark Mark{};
        VkSpecializationInfo SpecializationInfo = ConstructSpecializationInfo(Mark, Constants);
        
        VkComputePipelineCreateInfo Info
        {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = GDevice->bPipelineStats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0u,
            .stage =
                {
                    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext                  = nullptr,
                    .flags                  = 0,
                    .stage                  = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module                 = ShaderModule,
                    .pName                  = Compute.EntryPoint.data(),
                    .pSpecializationInfo    = &SpecializationInfo
                },
            .layout                 = GDevice->PipelineLayout,
            .basePipelineHandle     = VK_NULL_HANDLE,
            .basePipelineIndex      = 0 
        };
        
        VkPipeline Pipeline{};
        VK_CHECK(vkCreateComputePipelines(*GDevice, nullptr, 1, &Info, nullptr, &Pipeline));

        vkDestroyShaderModule(*GDevice, ShaderModule, nullptr);

        return GDevice->Pipelines.Emplace(Pipeline, VK_PIPELINE_BIND_POINT_COMPUTE);
    }

    uint32 GetMaxMeshWorkGroupCount()
    {
        return GDevice->MaxMeshWorkGroupCountX;
    }

    bool SupportsAsyncTransfer()
    {
        return GDevice != nullptr && GDevice->bHasAsyncTransferQueue;
    }

    bool SupportsAsyncCompute()
    {
        return GDevice != nullptr && GDevice->bHasAsyncComputeQueue;
    }

    FPipelineH CreateMeshShaderPipeline(const FShaderSource& Task, const FShaderSource& Mesh, const FShaderSource& Fragment, const FRasterDesc& Desc, TSpan<const FSpecializationConstant> Constants)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        auto MakeModule = [](const FShaderSource& Src) -> VkShaderModule
        {
            if (Src.Source.empty())
            {
                return VK_NULL_HANDLE;
            }

            VkShaderModuleCreateInfo Info
            {
                .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext    = nullptr,
                .flags    = 0,
                .codeSize = Src.Source.size(),
                .pCode    = reinterpret_cast<const uint32*>(Src.Source.data()),
            };

            VkShaderModule Module;
            VK_CHECK(vkCreateShaderModule(*GDevice, &Info, nullptr, &Module));
            return Module;
        };

        // Mesh stage is required; task and fragment are optional.
        VkShaderModule TaskModule = MakeModule(Task);
        VkShaderModule MeshModule = MakeModule(Mesh);
        VkShaderModule FragModule = MakeModule(Fragment);

        FMemMark Mark;
        const VkSpecializationInfo SpecializationInfo = ConstructSpecializationInfo(Mark, Constants);

        VkPipelineShaderStageCreateInfo Stages[3];
        uint32 StageCount = 0;
        if (TaskModule != VK_NULL_HANDLE)
        {
            Stages[StageCount++] = VkPipelineShaderStageCreateInfo
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_TASK_BIT_EXT,
                .module              = TaskModule,
                .pName               = "main",
                .pSpecializationInfo = &SpecializationInfo,
            };
        }
        // Only chained where the device could otherwise pick a subgroup narrower than the mesh workgroup,
        // which would split it across two waves and break ShuffleMeshletClip's WaveReadLaneAt. Device init
        // leaves MeshRequiredSubgroupSize at 0 on hardware that cannot do this (AMD, NVIDIA).
        const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo MeshSubgroupSize
        {
            .sType                = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
            .pNext                = nullptr,
            .requiredSubgroupSize = GDevice->MeshRequiredSubgroupSize,
        };

        Stages[StageCount++] = VkPipelineShaderStageCreateInfo
        {
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext               = GDevice->MeshRequiredSubgroupSize != 0 ? &MeshSubgroupSize : nullptr,
            .flags               = 0,
            .stage               = VK_SHADER_STAGE_MESH_BIT_EXT,
            .module              = MeshModule,
            .pName               = "main",
            .pSpecializationInfo = &SpecializationInfo,
        };
        if (FragModule != VK_NULL_HANDLE)
        {
            Stages[StageCount++] = VkPipelineShaderStageCreateInfo
            {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = nullptr,
                .flags               = 0,
                .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module              = FragModule,
                .pName               = "main",
                .pSpecializationInfo = &SpecializationInfo,
            };
        }

        VkFormat DepthAttachmentFormat   = ConvertFormat(Desc.DepthFormat);
        VkFormat StencilAttachmentFormat = ConvertFormat(Desc.StencilFormat);

        const uint32 ColorTargetCount = static_cast<uint32>(Desc.ColorTargets.size());

        auto* ColorBlendAttachments  = Mark.AllocArray<VkPipelineColorBlendAttachmentState>(ColorTargetCount);
        auto* ColorAttachmentFormats = Mark.AllocArray<VkFormat>(ColorTargetCount);

        for (uint32 i = 0; i < ColorTargetCount; ++i)
        {
            const FBlendDesc& Blend = Desc.ColorTargets[i].Blend;

            ColorBlendAttachments[i].blendEnable         = Blend.bBlendEnable;
            ColorBlendAttachments[i].srcColorBlendFactor = ToVkBlendFactor(Blend.SrcColorFactor);
            ColorBlendAttachments[i].dstColorBlendFactor = ToVkBlendFactor(Blend.DstColorFactor);
            ColorBlendAttachments[i].colorBlendOp        = ToVkBlendOp(Blend.ColorOp);
            ColorBlendAttachments[i].srcAlphaBlendFactor = ToVkBlendFactor(Blend.SrcAlphaFactor);
            ColorBlendAttachments[i].dstAlphaBlendFactor = ToVkBlendFactor(Blend.DstAlphaFactor);
            ColorBlendAttachments[i].alphaBlendOp        = ToVkBlendOp(Blend.AlphaOp);
            ColorBlendAttachments[i].colorWriteMask      = static_cast<VkColorComponentFlags>(Blend.ColorWriteMask & 0xF);

            ColorAttachmentFormats[i] = ConvertFormat(Desc.ColorTargets[i].Format);
        }

        VkPipelineColorBlendStateCreateInfo ColorBlendStates
        {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext           = nullptr,
            .flags           = 0,
            .logicOpEnable   = false,
            .logicOp         = VK_LOGIC_OP_NO_OP,
            .attachmentCount = ColorTargetCount,
            .pAttachments    = ColorBlendAttachments,
            .blendConstants  = {1.f, 1.f, 1.f, 1.f},
        };

        VkPipelineRasterizationStateCreateInfo RasterState
        {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .depthClampEnable        = false,
            .rasterizerDiscardEnable = false,
            .polygonMode             = Desc.bWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
            .cullMode                = VK_CULL_MODE_BACK_BIT,
            .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable         = true,
            .depthBiasConstantFactor = 0,
            .depthBiasClamp          = 0,
            .depthBiasSlopeFactor    = 0,
            .lineWidth               = 1.0f,
        };

        const VkSampleCountFlagBits SampleCount = static_cast<VkSampleCountFlagBits>(Desc.SampleCount == 0 ? 1 : Desc.SampleCount);

        VkPipelineMultisampleStateCreateInfo MultiSampleState
        {
            .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .rasterizationSamples  = SampleCount,
            .sampleShadingEnable   = false,
            .minSampleShading      = 1.0f,
            .pSampleMask           = nullptr,
            .alphaToCoverageEnable = Desc.bAlphaToCoverage,
            .alphaToOneEnable      = false,
        };

        VkPipelineDepthStencilStateCreateInfo DepthStencilState
        {
            .sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .depthTestEnable       = VK_FALSE,
            .depthWriteEnable      = VK_FALSE,
            .depthCompareOp        = VK_COMPARE_OP_ALWAYS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable     = VK_FALSE,
            .minDepthBounds        = 0.0f,
            .maxDepthBounds        = 1.0f,
        };

        VkDynamicState DynamicState[] =
        {
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS,
            VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
            VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_LINE_WIDTH,
        };

        VkPipelineViewportStateCreateInfo ViewportState
        {
            .sType          = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext          = nullptr,
            .flags          = 0,
            .viewportCount  = 0,
            .pViewports     = nullptr,
            .scissorCount   = 0,
            .pScissors      = nullptr
        };

        VkPipelineDynamicStateCreateInfo DynamicStateInfo
        {
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext             = nullptr,
            .flags             = 0,
            .dynamicStateCount = std::size(DynamicState),
            .pDynamicStates    = DynamicState,
        };

        VkPipelineRenderingCreateInfo PipelineCreate
        {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext                   = nullptr,
            .viewMask                = 0,
            .colorAttachmentCount    = ColorTargetCount,
            .pColorAttachmentFormats = ColorAttachmentFormats,
            .depthAttachmentFormat   = DepthAttachmentFormat,
            .stencilAttachmentFormat = StencilAttachmentFormat,
        };

        // Mesh pipelines have no input assembler: pVertexInputState / pInputAssemblyState are ignored (left null).
        VkGraphicsPipelineCreateInfo CreateInfo
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &PipelineCreate,
            .flags               = GDevice->bPipelineStats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0u,
            .stageCount          = StageCount,
            .pStages             = Stages,
            .pVertexInputState   = nullptr,
            .pInputAssemblyState = nullptr,
            .pTessellationState  = nullptr,
            .pViewportState      = &ViewportState,
            .pRasterizationState = &RasterState,
            .pMultisampleState   = &MultiSampleState,
            .pDepthStencilState  = &DepthStencilState,
            .pColorBlendState    = &ColorBlendStates,
            .pDynamicState       = &DynamicStateInfo,
            .layout              = GDevice->PipelineLayout,
            .renderPass          = VK_NULL_HANDLE,
            .subpass             = 0,
            .basePipelineHandle  = VK_NULL_HANDLE,
            .basePipelineIndex   = 0,
        };

        VkPipeline VulkanPipeline;
        VK_CHECK(vkCreateGraphicsPipelines(*GDevice, nullptr, 1, &CreateInfo, nullptr, &VulkanPipeline));

        if (TaskModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(*GDevice, TaskModule, nullptr);
        }
        vkDestroyShaderModule(*GDevice, MeshModule, nullptr);
        if (FragModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(*GDevice, FragModule, nullptr);
        }

        return GDevice->Pipelines.Emplace(VulkanPipeline, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }

    FSemaphoreH CreateTimelineSemaphore(uint64 Value)
    {
        VkSemaphoreTypeCreateInfo TypeInfo
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = Value
        };
        
        VkSemaphoreCreateInfo Info
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &TypeInfo,
            .flags = 0
        };
        
        VkSemaphore Semaphore;
        vkCreateSemaphore(*GDevice, &Info, nullptr, &Semaphore);

        return GDevice->Semaphores.Emplace(Semaphore);
    }

    FTextureH CreateTexture(const FTextureDesc& Desc, GPUPtr Location)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        const VkFormat Format = ConvertFormat(Desc.Format);
        const VkImageAspectFlags Aspect = GuessImageAspectFlags(Format);

        VkImageType ImageType = VK_IMAGE_TYPE_2D;
        VkImageViewType ViewType = VK_IMAGE_VIEW_TYPE_2D;
        VkImageCreateFlags CreateFlags = 0;

        switch (Desc.Type)
        {
            case ETextureType::Tex1D:        ImageType = VK_IMAGE_TYPE_1D; ViewType = VK_IMAGE_VIEW_TYPE_1D; break;
            case ETextureType::Tex2D:        ImageType = VK_IMAGE_TYPE_2D; ViewType = VK_IMAGE_VIEW_TYPE_2D; break;
            case ETextureType::Tex3D:        ImageType = VK_IMAGE_TYPE_3D; ViewType = VK_IMAGE_VIEW_TYPE_3D; break;
            case ETextureType::TexCube:      ImageType = VK_IMAGE_TYPE_2D; ViewType = VK_IMAGE_VIEW_TYPE_CUBE; CreateFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
            case ETextureType::Tex2DArray:   ImageType = VK_IMAGE_TYPE_2D; ViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
            case ETextureType::TexCubeArray: ImageType = VK_IMAGE_TYPE_2D; ViewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; CreateFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
        }

        VkImageUsageFlags Usage = 0;
        Usage |= EnumHasAnyFlags(Desc.Usage, EImageUsageFlags::Sampled)         ? VK_IMAGE_USAGE_SAMPLED_BIT : 0;
        Usage |= EnumHasAnyFlags(Desc.Usage, EImageUsageFlags::Storage)         ? VK_IMAGE_USAGE_STORAGE_BIT : 0;
        Usage |= EnumHasAnyFlags(Desc.Usage, EImageUsageFlags::ColorAttachment) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0;
        Usage |= EnumHasAnyFlags(Desc.Usage, EImageUsageFlags::DepthAttachment) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0;
        Usage |= EnumHasAnyFlags(Desc.Usage, EImageUsageFlags::TransferSrc)     ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0;
        Usage |= EnumHasAnyFlags(Desc.Usage, EImageUsageFlags::TransferDst)     ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0;

        const uint32 Depth = Desc.Type == ETextureType::Tex3D ? Math::Max(Desc.Dimension.z, 1u) : 1u;

        VkImageCreateInfo Info
        {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext         = nullptr,
            .flags         = CreateFlags,
            .imageType     = ImageType,
            .format        = Format,
            .extent        = { Desc.Dimension.x, Desc.Dimension.y, Depth },
            .mipLevels     = Math::Max(Desc.MipCount, 1u),
            .arrayLayers   = Math::Max(Desc.LayerCount, 1u),
            .samples       = static_cast<VkSampleCountFlagBits>(Desc.SampleCount == 0 ? 1 : Desc.SampleCount),
            .tiling        = VK_IMAGE_TILING_OPTIMAL,
            .usage         = Usage,
            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo AllocationCreateInfo{};
        AllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        VkImage Image = VK_NULL_HANDLE;
        VmaAllocation Allocation = VK_NULL_HANDLE;

#if USING(WITH_EDITOR)
        // Only the memory tool asks how big an image actually landed; the driver's answer is free to
        // collect but pointless to ask for in a game build.
        VmaAllocationInfo  AllocationInfo    = {};
        VmaAllocationInfo* AllocationInfoPtr = &AllocationInfo;
#else
        VmaAllocationInfo* AllocationInfoPtr = nullptr;
#endif

        // Its own zone: a 16-21 MiB device-local image allocation can hit a fresh VMA block and a
        // vkAllocateMemory, which is a very different spike from anything else in a residency change.
        VkResult ImageResult;
        {
            LUMINA_PROFILE_SECTION("RHI::vmaCreateImage");
            ImageResult = vmaCreateImage(GDevice->Allocator, &Info, &AllocationCreateInfo, &Image, &Allocation, AllocationInfoPtr);
        }
        if (ImageResult != VK_SUCCESS || Image == VK_NULL_HANDLE || Allocation == nullptr)
        {
            PanicOutOfGPUMemory(::Lumina::Format("a {}x{}x{} texture, {} mips, {} layers, format {}",
                Desc.Dimension.x, Desc.Dimension.y, Depth, Info.mipLevels, Info.arrayLayers, (uint32)Format).c_str(), ImageResult);
        }

        VkImageViewCreateInfo ViewCreateInfo
        {
            .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext      = nullptr,
            .flags      = 0,
            .image      = Image,
            .viewType   = ViewType,
            .format     = Format,
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange =
            {
                .aspectMask     = Aspect,
                .baseMipLevel   = 0,
                .levelCount     = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount     = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        VkImageView View = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(*GDevice, &ViewCreateInfo, nullptr, &View));

        FTextureH Handle = GDevice->Textures.Emplace(FTexture
        {
            .Image              = Image,
            .DefaultImageView   = View,
            .Allocation         = Allocation,
            .Type               = ViewType,
            .Format             = Desc.Format,
            .Desc               = Desc,
            .bSwapchainImage    = false
        });

        {
            FScopeLock Lock(GDevice->InitMutex);
            GDevice->UninitializedTextures.push_back(Handle);
            GDevice->PendingImageInits.store((uint32)GDevice->UninitializedTextures.size(), std::memory_order_release);
        }

#if USING(WITH_EDITOR)
        {
            FScopeLock Lock(GDevice->TextureLedgerMutex);
            FTextureRecord& Record = GDevice->TextureLedger[Handle.Handle];
            Record.Size    = AllocationInfo.size;
            Record.Desc    = Desc;
            Record.Name[0] = '\0';   // filled in by SetDebugName, which nearly every site calls
        }
#endif

        return Handle;
    }

    FTextureDesc GetTextureDesc(FTextureH Texture)
    {
        return GDevice->Textures[Texture].Desc;
    }

    FTextureHeapH CreateTextureHeap(uint32 TextureCount, uint32 RWTextureCount, uint32 SamplerCount)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        VkDescriptorSetAllocateInfo Info
        {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext              = nullptr,
            .descriptorPool     = GDevice->DescriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &GDevice->DescriptorLayout
        };
        
        VkDescriptorSet DescriptorSet;
        vkAllocateDescriptorSets(*GDevice, &Info, &DescriptorSet);

        return GDevice->TextureHeaps.Emplace(FTextureHeap
        {
            .DescriptorSet          = DescriptorSet,
            .DescriptorPool         = GDevice->DescriptorPool,
            .SamplerSlots           = FHandleAllocator{SamplerCount},
            .SampledImageSlots      = FHandleAllocator{TextureCount},
            .RWImageSlots           = FHandleAllocator{RWTextureCount},
            .Samplers               = TVector<VkSampler>{SamplerCount, nullptr},
            .ImageViews             = TVector<VkImageView>{TextureCount, nullptr},
            .RWImageViews           = TVector<VkImageView>{RWTextureCount, nullptr},
            .SampledOwners          = TVector<FTextureH>{TextureCount, FTextureH{}}
        });
    }

    // Caller holds HeapMutex.
    static void WriteHeapDescriptor(FTextureHeap& HeapData, uint32 Binding, uint32 Slot, VkDescriptorType Type, const VkDescriptorImageInfo& ImageInfo)
    {
        VkWriteDescriptorSet Write
        {
            .sType              = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext              = nullptr,
            .dstSet             = HeapData.DescriptorSet,
            .dstBinding         = Binding,
            .dstArrayElement    = Slot,
            .descriptorCount    = 1,
            .descriptorType     = Type,
            .pImageInfo         = &ImageInfo,
            .pBufferInfo        = nullptr,
            .pTexelBufferView   = nullptr
        };

        vkUpdateDescriptorSets(*GDevice, 1, &Write, 0, nullptr);
    }

    // Caller holds HeapMutex. Slot must already be marked occupied.
    static void PointSampledSlotAt(FTextureHeapH Heap, FTextureHeap& HeapData, uint32 Slot, FTextureH Texture)
    {
        FTexture& TextureData = GDevice->Textures[Texture];

        // Whatever this slot used to name is no longer bound anywhere, so its tripwire has to be cleared
        // or destroying it later would report a slot that has since moved on to a different image.
        // Whatever this slot used to name is no longer bound anywhere. TryGet, not operator[]: the previous
        // owner may already be destroyed, and the generation check is what makes reading a handle that
        // outlived its resource a branch instead of a read of a recycled entry.
        const FTextureH Previous = HeapData.SampledOwners[Slot];
        if (IsValid(Previous) && Previous.Handle != Texture.Handle)
        {
            if (FTexture* PreviousData = GDevice->Textures.TryGet(Previous))
            {
                if (PreviousData->BoundSampledSlot == Slot)
                {
                    PreviousData->BoundSampledSlot = kInvalidHeapSlot;
                }
            }
        }

        HeapData.ImageViews[Slot] = TextureData.DefaultImageView;
        HeapData.SampledOwners[Slot] = Texture;
        TextureData.BoundSampledSlot = Slot;
        TextureData.BoundHeap        = Heap;

        const VkDescriptorImageInfo ImageInfo
        {
            .sampler        = VK_NULL_HANDLE,
            .imageView      = TextureData.DefaultImageView,
            .imageLayout    = VK_IMAGE_LAYOUT_GENERAL
        };

        WriteHeapDescriptor(HeapData, kImageBindingSlot, Slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ImageInfo);
    }

    uint32 HeapWriteTexture(FTextureHeapH Heap, FTextureH Texture)
    {
        if (!IsValid(Texture))
        {
            return FHandleAllocator::kInvalidHandle;
        }

        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        const uint32 Slot = HeapData.SampledImageSlots.Alloc();
        if (Slot == FHandleAllocator::kInvalidHandle)
        {
            LOG_ERROR("RHI: sampled texture heap exhausted ({} slots); texture not registered.", HeapData.SampledImageSlots.GetCapacity());
            return FHandleAllocator::kInvalidHandle;
        }

        PointSampledSlotAt(Heap, HeapData, Slot, Texture);

        return Slot;
    }

    void HeapRepointTexture(FTextureHeapH Heap, uint32 Slot, FTextureH Texture)
    {
        if (!IsValid(Texture))
        {
            return;
        }

        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        if (Slot >= HeapData.SampledImageSlots.GetCapacity())
        {
            return;
        }

        HeapData.SampledImageSlots.MarkAllocated(Slot);
        PointSampledSlotAt(Heap, HeapData, Slot, Texture);
    }

    uint32 HeapWriteRWTexture(FTextureHeapH Heap, FTextureH Texture, uint32 Mip)
    {
        if (!IsValid(Texture))
        {
            return FHandleAllocator::kInvalidHandle;
        }

        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];
        const FTexture& TextureData = GDevice->Textures[Texture];

        // Storage views of cube images must be 2D arrays.
        VkImageViewType ViewType = TextureData.Type;
        if (ViewType == VK_IMAGE_VIEW_TYPE_CUBE || ViewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        {
            ViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        }

        VkImageViewCreateInfo ViewCreateInfo
        {
            .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext      = nullptr,
            .flags      = 0,
            .image      = TextureData.Image,
            .viewType   = ViewType,
            .format     = ConvertFormat(TextureData.Format),
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange =
            {
                .aspectMask     = AspectsForFormat(TextureData.Format),
                .baseMipLevel   = Mip,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        FScopeLock Lock(GDevice->HeapMutex);
        const uint32 Slot = HeapData.RWImageSlots.Alloc();
        if (Slot == FHandleAllocator::kInvalidHandle)
        {
            LOG_ERROR("RHI: RW texture heap exhausted ({} slots); storage view not registered.", HeapData.RWImageSlots.GetCapacity());
            return FHandleAllocator::kInvalidHandle;
        }

        VkImageView View = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(*GDevice, &ViewCreateInfo, nullptr, &View));

        HeapData.RWImageViews[Slot] = View;

        const VkDescriptorImageInfo ImageInfo
        {
            .sampler        = VK_NULL_HANDLE,
            .imageView      = View,
            .imageLayout    = VK_IMAGE_LAYOUT_GENERAL
        };

        WriteHeapDescriptor(HeapData, kRWImageBindingSlot, Slot, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ImageInfo);

        return Slot;
    }

    uint32 HeapWriteSampler(FTextureHeapH Heap, const FSamplerDesc& Desc)
    {
        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        VkSamplerReductionModeCreateInfo ReductionInfo
        {
            .sType         = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
            .pNext         = nullptr,
            .reductionMode = Desc.Reduction == EReduction::Min ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX
        };

        VkSamplerCreateInfo SamplerInfo
        {
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = Desc.Reduction != EReduction::None ? &ReductionInfo : nullptr,
            .flags                   = 0,
            .magFilter               = ToVkFilter(Desc.MagFilter),
            .minFilter               = ToVkFilter(Desc.MinFilter),
            .mipmapMode              = ToVkMipmapMode(Desc.MipFilter),
            .addressModeU            = ToVkAddressMode(Desc.AddressU),
            .addressModeV            = ToVkAddressMode(Desc.AddressV),
            .addressModeW            = ToVkAddressMode(Desc.AddressW),
            .mipLodBias              = Desc.MipBias,
            .anisotropyEnable        = Desc.MaxAnisotropy > 1.0f,
            .maxAnisotropy           = Desc.MaxAnisotropy,
            .compareEnable           = Desc.CompareOp != EOp::Never,
            .compareOp               = ToVkCompareOp(Desc.CompareOp),
            .minLod                  = 0.0f,
            .maxLod                  = VK_LOD_CLAMP_NONE,
            .borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
            .unnormalizedCoordinates = false,
        };

        FScopeLock Lock(GDevice->HeapMutex);
        const uint32 Slot = HeapData.SamplerSlots.Alloc();
        if (Slot == FHandleAllocator::kInvalidHandle)
        {
            LOG_ERROR("RHI: sampler heap exhausted ({} slots); sampler not registered.", HeapData.SamplerSlots.GetCapacity());
            return FHandleAllocator::kInvalidHandle;
        }

        VkSampler Sampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(*GDevice, &SamplerInfo, nullptr, &Sampler));

        HeapData.Samplers[Slot] = Sampler;

        const VkDescriptorImageInfo ImageInfo
        {
            .sampler        = Sampler,
            .imageView      = VK_NULL_HANDLE,
            .imageLayout    = VK_IMAGE_LAYOUT_UNDEFINED
        };

        WriteHeapDescriptor(HeapData, kSamplerBindingSlot, Slot, VK_DESCRIPTOR_TYPE_SAMPLER, ImageInfo);

        if (Slot == 0)
        {
            TVector<VkDescriptorImageInfo> SeedInfos;
            SeedInfos.resize(HeapData.SamplerSlots.GetCapacity() - 1, VkDescriptorImageInfo
            {
                .sampler        = Sampler,
                .imageView      = VK_NULL_HANDLE,
                .imageLayout    = VK_IMAGE_LAYOUT_UNDEFINED
            });

            if (!SeedInfos.empty())
            {
                const VkWriteDescriptorSet Write
                {
                    .sType              = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext              = nullptr,
                    .dstSet             = HeapData.DescriptorSet,
                    .dstBinding         = kSamplerBindingSlot,
                    .dstArrayElement    = 1,
                    .descriptorCount    = (uint32)SeedInfos.size(),
                    .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
                    .pImageInfo         = SeedInfos.data(),
                    .pBufferInfo        = nullptr,
                    .pTexelBufferView   = nullptr
                };

                vkUpdateDescriptorSets(*GDevice, 1, &Write, 0, nullptr);
            }
        }

        return Slot;
    }

    void HeapSetFallbackTexture(FTextureHeapH Heap, FTextureH Texture)
    {
        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        HeapData.FallbackView = IsValid(Texture) ? GDevice->Textures[Texture].DefaultImageView : VK_NULL_HANDLE;

        if (HeapData.FallbackView == VK_NULL_HANDLE)
        {
            return;
        }

        TVector<VkDescriptorImageInfo> ImageInfos;
        ImageInfos.reserve(HeapData.SampledImageSlots.GetCapacity());

        uint32 First = 0;
        auto FlushRun = [&]()
        {
            if (ImageInfos.empty())
            {
                return;
            }

            const VkWriteDescriptorSet Write
            {
                .sType              = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext              = nullptr,
                .dstSet             = HeapData.DescriptorSet,
                .dstBinding         = kImageBindingSlot,
                .dstArrayElement    = First,
                .descriptorCount    = (uint32)ImageInfos.size(),
                .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo         = ImageInfos.data(),
                .pBufferInfo        = nullptr,
                .pTexelBufferView   = nullptr
            };

            vkUpdateDescriptorSets(*GDevice, 1, &Write, 0, nullptr);
            ImageInfos.clear();
        };

        for (uint32 Slot = 0; Slot < HeapData.SampledImageSlots.GetCapacity(); ++Slot)
        {
            if (HeapData.SampledImageSlots.IsAllocated(Slot))
            {
                // A live slot must keep its own view, so the run breaks here rather than overwriting it.
                FlushRun();
                continue;
            }

            if (ImageInfos.empty())
            {
                First = Slot;
            }

            ImageInfos.push_back(VkDescriptorImageInfo
            {
                .sampler        = VK_NULL_HANDLE,
                .imageView      = HeapData.FallbackView,
                .imageLayout    = VK_IMAGE_LAYOUT_GENERAL
            });
        }
        FlushRun();
    }

    // Caller holds HeapMutex.
    static void PointSampledSlotAtFallback(FTextureHeap& HeapData, uint32 Slot)
    {
        const FTextureH Previous = HeapData.SampledOwners[Slot];
        if (IsValid(Previous))
        {
            if (FTexture* PreviousData = GDevice->Textures.TryGet(Previous))
            {
                if (PreviousData->BoundSampledSlot == Slot)
                {
                    PreviousData->BoundSampledSlot = kInvalidHeapSlot;
                }
            }
        }

        HeapData.ImageViews[Slot] = VK_NULL_HANDLE;
        HeapData.SampledOwners[Slot] = {};

        if (HeapData.FallbackView != VK_NULL_HANDLE)
        {
            const VkDescriptorImageInfo ImageInfo
            {
                .sampler        = VK_NULL_HANDLE,
                .imageView      = HeapData.FallbackView,
                .imageLayout    = VK_IMAGE_LAYOUT_GENERAL
            };

            WriteHeapDescriptor(HeapData, kImageBindingSlot, Slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ImageInfo);
        }
    }

    void HeapUnbindTexture(FTextureHeapH Heap, uint32 Slot)
    {
        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        if (Slot >= HeapData.ImageViews.size())
        {
            return;
        }

        PointSampledSlotAtFallback(HeapData, Slot);
    }

    void HeapFreeTexture(FTextureHeapH Heap, uint32 Slot)
    {
        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        if (Slot >= HeapData.ImageViews.size())
        {
            return;
        }

        HeapData.SampledImageSlots.Free(Slot);
        PointSampledSlotAtFallback(HeapData, Slot);
    }

    void GetTextureHeapTextures(FTextureHeapH Heap, TVector<FHeapTextureInfo>& OutTextures)
    {
        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        // Occupied slots only, so this skips empty words wholesale rather than testing every index.
        HeapData.SampledImageSlots.ForEachAllocated([&](uint32 Slot)
        {
            if (HeapData.ImageViews[Slot] == VK_NULL_HANDLE)
            {
                return;
            }
            OutTextures.push_back(FHeapTextureInfo
            {
                .Slot = Slot,
                .Desc = GDevice->Textures[HeapData.SampledOwners[Slot]].Desc
            });
        });
    }

    void HeapFreeRWTexture(FTextureHeapH Heap, uint32 Slot)
    {
        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        if (Slot >= HeapData.RWImageViews.size())
        {
            return;
        }

        if (HeapData.RWImageViews[Slot] != VK_NULL_HANDLE)
        {
            GDevice->PendingHeapDestroys.push_back(FDeviceImpl::FPendingHeapDestroy
            {
                .View    = HeapData.RWImageViews[Slot],
                .Sampler = VK_NULL_HANDLE,
                .Slot    = GDevice->CurrentRetireSlot.load(std::memory_order_acquire)
            });
            HeapData.RWImageViews[Slot] = VK_NULL_HANDLE;
        }
        HeapData.RWImageSlots.Free(Slot);
    }

    void HeapFreeSampler(FTextureHeapH Heap, uint32 Slot)
    {
        FTextureHeap& HeapData = GDevice->TextureHeaps[Heap];

        FScopeLock Lock(GDevice->HeapMutex);
        if (Slot >= HeapData.Samplers.size())
        {
            return;
        }

        if (HeapData.Samplers[Slot] != VK_NULL_HANDLE)
        {
            GDevice->PendingHeapDestroys.push_back(FDeviceImpl::FPendingHeapDestroy
            {
                .View    = VK_NULL_HANDLE,
                .Sampler = HeapData.Samplers[Slot],
                .Slot    = GDevice->CurrentRetireSlot.load(std::memory_order_acquire)
            });
            HeapData.Samplers[Slot] = VK_NULL_HANDLE;
        }
        HeapData.SamplerSlots.Free(Slot);
    }
    
    static bool GVSyncEnabled = true;

    void SetVSync(bool bEnabled)
    {
        GVSyncEnabled = bEnabled;
    }

    bool GetVSync()
    {
        return GVSyncEnabled;
    }

    static VkPresentModeKHR ChoosePresentMode(VkSurfaceKHR Surface)
    {
        // FIFO (always supported) caps to the display refresh = vsync.
        if (GVSyncEnabled)
        {
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        uint32 Count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(GDevice->PhysicsDevice, Surface, &Count, nullptr);
        TVector<VkPresentModeKHR> Modes(Count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(GDevice->PhysicsDevice, Surface, &Count, Modes.data());

        auto Supports = [&](VkPresentModeKHR Mode)
        {
            for (VkPresentModeKHR M : Modes) 
            { 
                if (M == Mode)
                {
                    return true;
                }
            }
            return false;
        };

        // Uncapped: MAILBOX (low-latency, no tearing) preferred, then IMMEDIATE, then FIFO.
        if (Supports(VK_PRESENT_MODE_MAILBOX_KHR))
        {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        if (Supports(VK_PRESENT_MODE_IMMEDIATE_KHR))
        {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    static bool BuildSwapchainImages(FSwapchain& SC, const FUIntVector2& Extent, VkSwapchainKHR OldSwapchain)
    {
        VkSurfaceCapabilitiesKHR Caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(GDevice->PhysicsDevice, SC.Surface, &Caps) != VK_SUCCESS)
        {
            return false;   // surface transiently unqueryable; caller retries next frame
        }

        VkExtent2D ActualExtent = (Caps.currentExtent.width != UINT32_MAX)
            ? Caps.currentExtent
            : VkExtent2D{ Extent.x, Extent.y };
        ActualExtent.width  = Math::Clamp(ActualExtent.width,  Caps.minImageExtent.width,  Caps.maxImageExtent.width);
        ActualExtent.height = Math::Clamp(ActualExtent.height, Caps.minImageExtent.height, Caps.maxImageExtent.height);

        // Skip a zero-area surface (minimized / mid-resize): imageExtent {0,0} is rejected by AMD; min extent can also be {0,0}.
        if (ActualExtent.width == 0 || ActualExtent.height == 0)
        {
            return false;
        }

        uint32 ImageCount = Math::Max((uint32)kFramesInFlight, Caps.minImageCount);
        if (Caps.maxImageCount != 0)
        {
            ImageCount = Math::Min(ImageCount, Caps.maxImageCount);
        }

        const VkFormat Format = VK_FORMAT_B8G8R8A8_UNORM;

        VkSwapchainCreateInfoKHR Info
        {
            .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface          = SC.Surface,
            .minImageCount    = ImageCount,
            .imageFormat      = Format,
            .imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent      = ActualExtent,
            .imageArrayLayers = 1,
            .imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform     = Caps.currentTransform,
            .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode      = ChoosePresentMode(SC.Surface),
            .clipped          = VK_TRUE,
            .oldSwapchain     = OldSwapchain,
        };

        if (vkCreateSwapchainKHR(*GDevice, &Info, nullptr, &SC.Swapchain) != VK_SUCCESS)
        {
            SC.Swapchain = VK_NULL_HANDLE;
            return false;   // transient create failure during resize; retry next frame
        }

        SC.Format = Format;
        SC.Extent = FUIntVector2(ActualExtent.width, ActualExtent.height);

        uint32 Count = 0;
        vkGetSwapchainImagesKHR(*GDevice, SC.Swapchain, &Count, nullptr);
        TVector<VkImage> Raw(Count);
        vkGetSwapchainImagesKHR(*GDevice, SC.Swapchain, &Count, Raw.data());

        FTextureDesc Desc;
        Desc.Type      = ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(SC.Extent.x, SC.Extent.y, 1);
        Desc.Format    = EFormat::BGRA8_UNORM;
        Desc.Usage     = EImageUsageFlags::ColorAttachment | EImageUsageFlags::TransferDst;

        SC.Images.reserve(Count);
        for (uint32 i = 0; i < Count; ++i)
        {
            VkImageViewCreateInfo ViewInfo
            {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .flags    = 0,
                .image    = Raw[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = Format,
                .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };

            VkImageView View = VK_NULL_HANDLE;
            VK_CHECK(vkCreateImageView(*GDevice, &ViewInfo, nullptr, &View));

            SC.Images.push_back(GDevice->Textures.Emplace(FTexture
            {
                .Image            = Raw[i],
                .DefaultImageView = View,
                .Allocation       = nullptr,
                .Type             = VK_IMAGE_VIEW_TYPE_2D,
                .Format           = EFormat::BGRA8_UNORM,
                .Desc             = Desc,
                .bSwapchainImage  = true,
            }));
        }

        // Present semaphores: one per image. Acquire semaphores: a small ring.
        const uint32 AcquireCount = Math::Max((uint32)kFramesInFlight, Count);
        SC.PresentSemaphores.resize(Count);
        SC.AcquireSemaphores.resize(AcquireCount);

        const VkSemaphoreCreateInfo SemInfo { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (uint32 i = 0; i < Count; ++i)        { VK_CHECK(vkCreateSemaphore(*GDevice, &SemInfo, nullptr, &SC.PresentSemaphores[i])); }
        for (uint32 i = 0; i < AcquireCount; ++i) { VK_CHECK(vkCreateSemaphore(*GDevice, &SemInfo, nullptr, &SC.AcquireSemaphores[i])); }

        return true;
    }

    static void DestroySwapchainImages(FSwapchain& SC)
    {
        for (FTextureH Image : SC.Images)
        {
            GDevice->Textures.Erase(Image);
        }
        SC.Images.clear();

        for (VkSemaphore Semaphore : SC.PresentSemaphores) { vkDestroySemaphore(*GDevice, Semaphore, nullptr); }
        for (VkSemaphore Semaphore : SC.AcquireSemaphores) { vkDestroySemaphore(*GDevice, Semaphore, nullptr); }
        SC.PresentSemaphores.clear();
        SC.AcquireSemaphores.clear();
    }

    FSurfaceH CreateSurface(void* WindowHandle)
    {
        if (GDevice->bHeadless)
        {
            ShowVulkanInitFailure("Headless Device Cannot Present",
                "CreateSurface was called on a device created with FDeviceDesc::bHeadless. That device "
                "requested neither the window-system instance extensions nor VK_KHR_swapchain, so it can "
                "never present. Check for a POSITIONAL FDeviceDesc initializer -- bHeadless is the third "
                "field, and a stray third argument sets it.");
            std::abort();
        }

        FSurface Surface{};
        Surface.Window = WindowHandle;

        VK_CHECK(glfwCreateWindowSurface(GDevice->Instance, static_cast<GLFWwindow*>(WindowHandle), nullptr, &Surface.Surface));

        return GDevice->Surfaces.Emplace(Move(Surface));
    }

    void FreeH(FSurfaceH Surface)
    {
        if (GDevice != nullptr)
        {
            GDevice->Surfaces.Erase(Surface);
        }
    }

    FSwapchainH CreateSwapchain(FSurfaceH SurfaceHandle, const FUIntVector2& Extent)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        FSurface& Source = GDevice->Surfaces[SurfaceHandle];

        FSwapchain SC{};
        SC.Window  = Source.Window;
        SC.Surface = Source.Surface;

        Source.Surface = VK_NULL_HANDLE;
        GDevice->Surfaces.Erase(SurfaceHandle);

        BuildSwapchainImages(SC, Extent, VK_NULL_HANDLE);
        SC.AcquireIndex = 0;
        SC.CurrentImageIndex = 0;
        SC.CurrentAcquire = VK_NULL_HANDLE;

        return GDevice->Swapchains.Emplace(Move(SC));
    }

    void FreeH(FSwapchainH Swapchain)
    {
        if (GDevice != nullptr)
        {
            GDevice->Swapchains.Erase(Swapchain);
        }
    }

    void RecreateSwapchain(FSwapchainH Swapchain, const FUIntVector2& Extent)
    {
        FAllQueuesLock QueueLock;
        vkDeviceWaitIdle(*GDevice);

        FSwapchain& SC = GDevice->Swapchains[Swapchain];
        VkSwapchainKHR Old = SC.Swapchain;

        DestroySwapchainImages(SC);
        if (!BuildSwapchainImages(SC, Extent, Old))
        {
            // No drawable area (minimized / mid-resize): leave it unbuilt; callers skip the frame and retry.
            SC.Swapchain = VK_NULL_HANDLE;
            SC.Extent = FUIntVector2(0, 0);
        }

        if (Old != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(*GDevice, Old, nullptr);
        }

        SC.AcquireIndex = 0;
        SC.CurrentImageIndex = 0;
        SC.CurrentAcquire = VK_NULL_HANDLE;
    }

    FTextureH AcquireNextImage(FSwapchainH Swapchain)
    {
        FSwapchain& SC = GDevice->Swapchains[Swapchain];

        if (SC.Swapchain == VK_NULL_HANDLE || SC.AcquireSemaphores.empty())
        {
            return {};   // swapchain not built (zero-area surface); caller retries next frame
        }

        VkSemaphore Acquire = SC.AcquireSemaphores[SC.AcquireIndex];
        const VkResult Result = vkAcquireNextImageKHR(*GDevice, SC.Swapchain, UINT64_MAX, Acquire, VK_NULL_HANDLE, &SC.CurrentImageIndex);

        if (Result != VK_SUCCESS && Result != VK_SUBOPTIMAL_KHR)
        {
            SC.CurrentAcquire = VK_NULL_HANDLE;
            return {};   // caller recreates + retries next frame
        }

        SC.CurrentAcquire = Acquire;
        SC.AcquireIndex = (SC.AcquireIndex + 1) % (uint32)SC.AcquireSemaphores.size();
        return SC.Images[SC.CurrentImageIndex];
    }

    FUIntVector2 GetSwapchainExtent(FSwapchainH Swapchain)
    {
        return GDevice->Swapchains[Swapchain].Extent;
    }

    EFormat GetSwapchainFormat(FSwapchainH Swapchain)
    {
        (void)Swapchain;
        return EFormat::BGRA8_UNORM;   // BuildSwapchainImages always requests this
    }

    static void SwapchainImageBarrier(VkCommandBuffer Cmd, VkImage Image, VkImageLayout Old, VkImageLayout New,
                                      VkPipelineStageFlags2 SrcStage, VkAccessFlags2 SrcAccess,
                                      VkPipelineStageFlags2 DstStage, VkAccessFlags2 DstAccess)
    {
        VkImageMemoryBarrier2 Barrier
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = SrcStage,
            .srcAccessMask       = SrcAccess,
            .dstStageMask        = DstStage,
            .dstAccessMask       = DstAccess,
            .oldLayout           = Old,
            .newLayout           = New,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = Image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        VkDependencyInfo Dep { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &Barrier };
        vkCmdPipelineBarrier2(Cmd, &Dep);
    }

    void CmdSwapchainBarrierToRender(FCmdListH CL, FSwapchainH Swapchain)
    {
        FSwapchain& SC = GDevice->Swapchains[Swapchain];
        const FTexture& Image = GDevice->Textures[SC.Images[SC.CurrentImageIndex]];
        
        SwapchainImageBarrier(GDevice->CommandLists[CL].CommandBuffer, Image.Image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }

    bool PresentSwapchain(FSwapchainH Swapchain, FCmdListH FinalCommandList, FSemaphoreH FrameSignal, uint64 FrameSignalValue)
    {
        FSwapchain& SC = GDevice->Swapchains[Swapchain];
        FCommandList& CL = GDevice->CommandLists[FinalCommandList];

        const FTexture& Image = GDevice->Textures[SC.Images[SC.CurrentImageIndex]];

        SwapchainImageBarrier(CL.CommandBuffer, Image.Image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

#if defined(TRACY_ENABLE)
        // Drain available GPU timestamps and reset their queries on this frame's final buffer.
        if (GTracyGPUContext)
        {
            TracyVkCollect(GTracyGPUContext, CL.CommandBuffer);
        }
#endif

        // Present submits without going through RHI::Submit, so it has to close the recording itself or the
        // open-list count never comes back down and every retire is pinned against a value that keeps moving.
        if (CL.bOpen)
        {
            CL.bOpen = false;
            GDevice->OpenCommandLists[(uint32)CL.Queue].fetch_sub(1, std::memory_order_release);
        }

        vkEndCommandBuffer(CL.CommandBuffer);

        VkSemaphore PresentSem = SC.PresentSemaphores[SC.CurrentImageIndex];

        // Present submits and presents on the graphics queue, so it takes that queue's lock.
        FScopeLock SubmitLock(QueueLockFor(EQueueType::Graphics));

        VkSemaphoreSubmitInfo WaitInfo
        {
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = SC.CurrentAcquire,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        };

        VkSemaphoreSubmitInfo SignalInfos[2]
        {
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = PresentSem, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = GDevice->Semaphores[FrameSignal].Semaphore, .value = FrameSignalValue, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
        };

        VkCommandBufferSubmitInfo CmdInfo { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = CL.CommandBuffer, .deviceMask = 1 };

        VkSubmitInfo2 Submit
        {
            .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext                    = nullptr,
            .flags                    = 0, 
            .waitSemaphoreInfoCount   = SC.CurrentAcquire != VK_NULL_HANDLE ? 1u : 0u,
            .pWaitSemaphoreInfos      = &WaitInfo,
            .commandBufferInfoCount   = 1,
            .pCommandBufferInfos      = &CmdInfo,
            .signalSemaphoreInfoCount = 2,
            .pSignalSemaphoreInfos    = SignalInfos,
        };

        VkQueue GraphicsQueue = GDevice->Queues[(uint32)EQueueType::Graphics];
        VK_CHECK(vkQueueSubmit2(GraphicsQueue, 1, &Submit, VK_NULL_HANDLE));

        VkPresentInfoKHR PresentInfo
        {
            .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext              = nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &PresentSem,
            .swapchainCount     = 1,
            .pSwapchains        = &SC.Swapchain,
            .pImageIndices      = &SC.CurrentImageIndex,
            .pResults           = nullptr
        };

        const VkResult Result = vkQueuePresentKHR(GraphicsQueue, &PresentInfo);
        return Result == VK_SUCCESS;
    }

    FCmdListH OpenCommandList(EQueueType Type)
    {
        LUMINA_MEMORY_SCOPE("RHI");
        static constexpr VkCommandBufferBeginInfo BeginInfo
        {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext              = nullptr,
            .flags              = 0,
            .pInheritanceInfo   = nullptr
        };

        {
            FScopeLock Lock(GDevice->CommandPoolMutex);
            TVector<FCmdListH>& FreeList = GDevice->FreeCommandLists[(uint32)Type];
            if (!FreeList.empty())
            {
                FCmdListH Reused = FreeList.back();
                FreeList.pop_back();

                FCommandList& CommandList = GDevice->CommandLists[Reused];
                CommandList.CurrentIndexBuffer = 0;
                CommandList.CurrentIndexType = VK_INDEX_TYPE_UINT32;
                #if defined(TRACY_ENABLE)
                CommandList.GPUZoneDepth = 0;
                #endif
                CommandList.BreadcrumbDepth = 0;
                CommandList.bOpen = true;
                GDevice->OpenCommandLists[(uint32)Type].fetch_add(1, std::memory_order_release);

                // Already in the initial state: ResetCommandList reset the pool when it recycled the list.
                vkBeginCommandBuffer(CommandList.CommandBuffer, &BeginInfo);

                return Reused;
            }
        }

        VkCommandPoolCreateInfo Info
        {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext              = nullptr,
            .flags              = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex   = GDevice->QueueFamilies[(uint32)Type]
        };

        VkCommandPool Pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(*GDevice, &Info, nullptr, &Pool));

        VkCommandBufferAllocateInfo BufferInfo = {};
        BufferInfo.sType                = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        BufferInfo.level                = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        BufferInfo.commandBufferCount   = 1;
        BufferInfo.commandPool          = Pool;

        VkCommandBuffer Buffer = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(*GDevice, &BufferInfo, &Buffer));

        vkBeginCommandBuffer(Buffer, &BeginInfo);

        GDevice->OpenCommandLists[(uint32)Type].fetch_add(1, std::memory_order_release);

        return GDevice->CommandLists.Emplace(FCommandList
        {
            .CommandBuffer      = Buffer,
            .Pool               = Pool,
            .CurrentIndexBuffer = 0,
            .CurrentIndexType   = VK_INDEX_TYPE_UINT32,
            .Queue              = Type,
            .bOpen              = true
        });
    }

    void ResetCommandList(FCmdListH CommandList)
    {
        FCommandList& List = GDevice->CommandLists[CommandList];

        FScopeLock Lock(GDevice->CommandPoolMutex);

        // Reset without a submit: the recording is discarded, so it can never name anything.
        if (List.bOpen)
        {
            List.bOpen = false;
            GDevice->OpenCommandLists[(uint32)List.Queue].fetch_sub(1, std::memory_order_release);
        }

        VK_CHECK(vkResetCommandPool(*GDevice, List.Pool, 0));

        GDevice->FreeCommandLists[(uint32)List.Queue].push_back(CommandList);
    }

    uint32 GetOpenCommandListCount(EQueueType Queue)
    {
        if (GDevice == nullptr)
        {
            return 0;
        }
        return GDevice->OpenCommandLists[(uint32)Queue].load(std::memory_order_acquire);
    }

    void Submit(EQueueType Queue, TSpan<const FCmdListH> CommandLists, TSpan<const FSemaphoreInfo> Waits, TSpan<const FSemaphoreInfo> Signals)
    {
        LUMINA_PROFILE_SCOPE();

        FMemMark Scratch;

        auto* SignalInfos = Scratch.AllocArray<VkSemaphoreSubmitInfo>(Signals.size());
        auto* WaitInfos   = Scratch.AllocArray<VkSemaphoreSubmitInfo>(Waits.size());

        // Images are EXCLUSIVE; a layout transition is a write, so transfer must never claim them.
        TVector<FTextureH> UninitializedTextures;
        if (Queue != EQueueType::Transfer && GDevice->PendingImageInits.load(std::memory_order_acquire) != 0)
        {
            FScopeLock Lock(GDevice->InitMutex);
            UninitializedTextures.swap(GDevice->UninitializedTextures);
            GDevice->PendingImageInits.store(0, std::memory_order_release);
        }

        for (size_t i = 0; i < Signals.size(); ++i)
        {
            const FSemaphoreInfo& Signal = Signals[i];
            VkSemaphore VulkanSemaphore = GDevice->Semaphores[Signal.Semaphore];

            VkSemaphoreSubmitInfo& SubmitInfo = SignalInfos[i];

            SubmitInfo.sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            SubmitInfo.pNext       = nullptr;
            SubmitInfo.semaphore   = VulkanSemaphore;
            SubmitInfo.value       = Signal.Value;
            SubmitInfo.stageMask   = ToVkPipelineState(Signal.Stage);
            SubmitInfo.deviceIndex = 0;
        }

        for (size_t i = 0; i < Waits.size(); ++i)
        {
            const FSemaphoreInfo& Wait = Waits[i];
            VkSemaphore VulkanSemaphore = GDevice->Semaphores[Wait.Semaphore];

            VkSemaphoreSubmitInfo& SubmitInfo = WaitInfos[i];

            SubmitInfo.sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            SubmitInfo.pNext       = nullptr;
            SubmitInfo.semaphore   = VulkanSemaphore;
            SubmitInfo.value       = Wait.Value;
            SubmitInfo.stageMask   = ToVkPipelineState(Wait.Stage);
            SubmitInfo.deviceIndex = 0;
        }

        VkCommandBuffer TransitionBuffer = VK_NULL_HANDLE;
        VkCommandPool   TransitionPool   = GDevice->TransientPools[(uint32)Queue];
        if (!UninitializedTextures.empty())
        {
            FScopeLock TransientLock(GDevice->TransientMutex);

            auto* TextureBarriers = Scratch.AllocArray<VkImageMemoryBarrier2>(UninitializedTextures.size());
            for (size_t i = 0; i < UninitializedTextures.size(); ++i)
            {
                const FTexture& Texture = GDevice->Textures[UninitializedTextures[i]];

                TextureBarriers[i] =
                {
                    .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext                  = nullptr,
                    .srcStageMask           = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    .srcAccessMask          = 0,
                    .dstStageMask           = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dstAccessMask          = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .oldLayout              = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout              = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
                    .image                  = Texture.Image,
                    .subresourceRange =
                        {
                            .aspectMask         = AspectsForFormat(Texture.Format),
                            .baseMipLevel       = 0,
                            .levelCount         = VK_REMAINING_MIP_LEVELS,
                            .baseArrayLayer     = 0,
                            .layerCount         = VK_REMAINING_ARRAY_LAYERS
                        }
                };
            }

            VkCommandBufferAllocateInfo AllocInfo = {};
            AllocInfo.sType                 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            AllocInfo.level                 = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            AllocInfo.commandBufferCount    = 1;
            AllocInfo.commandPool           = TransitionPool;
            VK_CHECK(vkAllocateCommandBuffers(*GDevice, &AllocInfo, &TransitionBuffer));

            VkCommandBufferBeginInfo BeginInfo
            {
                .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext              = nullptr,
                .flags              = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                .pInheritanceInfo   = nullptr
            };
            vkBeginCommandBuffer(TransitionBuffer, &BeginInfo);

            VkDependencyInfo DependencyInfo
            {
                .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext                      = nullptr,
                .dependencyFlags            = 0,
                .memoryBarrierCount         = 0,
                .pMemoryBarriers            = nullptr,
                .bufferMemoryBarrierCount   = 0,
                .pBufferMemoryBarriers      = nullptr,
                .imageMemoryBarrierCount    = static_cast<uint32>(UninitializedTextures.size()),
                .pImageMemoryBarriers       = TextureBarriers
            };
            vkCmdPipelineBarrier2(TransitionBuffer, &DependencyInfo);
            vkEndCommandBuffer(TransitionBuffer);

            GDevice->PendingTransient.push_back({ TransitionBuffer, TransitionPool,
                GDevice->CurrentRetireSlot.load(std::memory_order_acquire) });
        }

        const uint32 TransitionCount = TransitionBuffer != VK_NULL_HANDLE ? 1u : 0u;
        const uint32 TotalCommandBuffers = static_cast<uint32>(CommandLists.size()) + TransitionCount;

        auto* CommandSubmitInfos = Scratch.AllocArray<VkCommandBufferSubmitInfo>(TotalCommandBuffers);

        if (TransitionCount != 0)
        {
            CommandSubmitInfos[0].sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            CommandSubmitInfos[0].pNext         = nullptr;
            CommandSubmitInfos[0].deviceMask    = 1;
            CommandSubmitInfos[0].commandBuffer = TransitionBuffer;
        }

        for (size_t i = 0; i < CommandLists.size(); ++i)
        {
            FCommandList& CommandList = GDevice->CommandLists[CommandLists[i]];

            // From here the recording is the queue's problem, not a pending reference no fence covers.
            // Cleared before vkQueueSubmit2 but after the caller took its submit lock, which is what lets
            // the retire drain sample "counter + open lists" as one consistent pair.
            if (CommandList.bOpen)
            {
                CommandList.bOpen = false;
                GDevice->OpenCommandLists[(uint32)Queue].fetch_sub(1, std::memory_order_release);
            }

            #if defined(TRACY_ENABLE)
            if (Queue != EQueueType::Graphics
                && i + 1 == CommandLists.size()
                && GTracyOwnedContexts[(uint32)Queue] != nullptr)
            {
                TracyVkCollect(GTracyOwnedContexts[(uint32)Queue], CommandList.CommandBuffer);
            }
            #endif

            vkEndCommandBuffer(CommandList.CommandBuffer);

            VkCommandBufferSubmitInfo& SubmitInfo = CommandSubmitInfos[TransitionCount + i];

            SubmitInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            SubmitInfo.pNext            = nullptr;
            SubmitInfo.deviceMask       = 1;
            SubmitInfo.commandBuffer    = CommandList.CommandBuffer;
        }

        VkSubmitInfo2 SubmitInfo
        {
            .sType                      = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext                      = nullptr,
            .flags                      = 0,
            .waitSemaphoreInfoCount     = static_cast<uint32>(Waits.size()),
            .pWaitSemaphoreInfos        = WaitInfos,
            .commandBufferInfoCount     = TotalCommandBuffers,
            .pCommandBufferInfos        = CommandSubmitInfos,
            .signalSemaphoreInfoCount   = static_cast<uint32>(Signals.size()),
            .pSignalSemaphoreInfos      = SignalInfos
        };

        VkQueue VulkanQueue = GDevice->Queues[(uint32)Queue];
        FScopeLock SubmitLock(QueueLockFor(Queue));

#if !defined(LE_SHIPPING)
        {
            // Flat and fixed: a hash map here rehashes and allocates while the queue lock is held.
            struct FSignalHighWater { uint64 Native; uint64 Value; };
            static FSignalHighWater HighestSignaled[16] = {};

            for (const FSemaphoreInfo& Signal : Signals)
            {
                const uint64 Native = (uint64)GDevice->Semaphores[Signal.Semaphore].Semaphore;

                FSignalHighWater* Entry = nullptr;
                for (FSignalHighWater& Candidate : HighestSignaled)
                {
                    if (Candidate.Native == Native || Candidate.Native == 0)
                    {
                        Candidate.Native = Native;
                        Entry = &Candidate;
                        break;
                    }
                }

                // More distinct timelines than the table holds; the check lapses rather than allocating.
                if (Entry == nullptr)
                {
                    continue;
                }

                uint64& High = Entry->Value;
                if (Signal.Value <= High && High != 0)
                {
                    LOG_ERROR("Timeline regression: native {:#x} signaled {} on queue {}, already at {}. "
                              "{} command list(s), {} wait(s).",
                        Native, Signal.Value, (uint32)Queue, High, CommandLists.size(), Waits.size());
                }
                else
                {
                    High = Signal.Value;
                }
            }
        }
#endif

#if USING(WITH_EDITOR)
        // Stamped into the freed-block ledger. Incremented before the submit so a block freed during
        // recording reads the ordinal of the submit that may still reference it, never one earlier.
        GDevice->SubmitOrdinal.fetch_add(1, std::memory_order_relaxed);
#endif

        VK_CHECK(vkQueueSubmit2(VulkanQueue, 1, &SubmitInfo, VK_NULL_HANDLE));
    }

    void Submit(FCmdListH CommandList, EQueueType Type)
    {
        Submit(Type, TSpan<const FCmdListH>{&CommandList, 1});
    }

    void CmdMemcpy(FCmdListH CL, GPUPtr Dest, GPUPtr Source, size_t Size)
    {
        VkBuffer DestBuffer;
        VkBuffer SourceBuffer;
        VkDeviceSize DestOffset;
        VkDeviceSize SourceOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* DestIt   = FindMemory(Dest);
            const FMemoryBlock* SourceIt = FindMemory(Source);

            if (DestIt == nullptr || SourceIt == nullptr)
            {
                return;
            }

            DestBuffer   = DestIt->Buffer;
            SourceBuffer = SourceIt->Buffer;
            DestOffset   = Dest - DestIt->Device;
            SourceOffset = Source - SourceIt->Device;
        }

        VkBufferCopy Region
        {
            .srcOffset  = SourceOffset,
            .dstOffset  = DestOffset,
            .size       = Size
        };

        auto* VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        if (GLogCopies)
        {
            LOG_TRACE("CmdMemcpy: dst=0x{:016X} off={} size={} (src=0x{:016X} off={})",
                (uint64)DestBuffer, (uint64)DestOffset, (uint64)Size, (uint64)SourceBuffer, (uint64)SourceOffset);
        }

        vkCmdCopyBuffer(VkCmdBuf, SourceBuffer, DestBuffer, 1, &Region);
    }

    void CmdMemset(FCmdListH CL, GPUPtr Dest, uint64 Size, uint32 Value)
    {
        VkBuffer DestBuffer;
        VkDeviceSize DestOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* DestIt = FindMemory(Dest);
            if (DestIt == nullptr)
            {
                return;
            }

            DestBuffer = DestIt->Buffer;
            DestOffset = Dest - DestIt->Device;
        }

        // vkCmdFillBuffer requires a 4-multiple size (VUID-vkCmdFillBuffer-size-00028).
        const uint64 FillSize = Size & ~3ull;
        if (FillSize == 0)
        {
            return;
        }

        auto* VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdFillBuffer(VkCmdBuf, DestBuffer, DestOffset, FillSize, Value);
    }

    void CmdMemzero(FCmdListH CL, GPUPtr Dest, uint64 Size)
    {
        CmdMemset(CL, Dest, Size, 0u);
    }

    void CmdWriteMemory(FCmdListH CL, GPUPtr Dest, const void* Data, uint64 Size)
    {
        ASSERT(Size <= kMaxInlineWrite, "CmdWriteMemory is for inline writes (<= 64 KiB); stage larger data through CmdMemcpy");
        ASSERT((Dest & 3) == 0 && (Size & 3) == 0, "vkCmdUpdateBuffer needs 4-byte aligned offset and size");

        VkBuffer DestBuffer;
        VkDeviceSize DestOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* DestIt = FindMemory(Dest);
            if (DestIt == nullptr)
            {
                return;
            }

            DestBuffer = DestIt->Buffer;
            DestOffset = Dest - DestIt->Device;
        }

        auto* VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdUpdateBuffer(VkCmdBuf, DestBuffer, DestOffset, Size, Data);
    }

    static VkImageSubresourceLayers SliceLayers(const FTexture& Texture, const FTextureSlice& Slice)
    {
        return VkImageSubresourceLayers
        {
            .aspectMask     = AspectsForFormat(Texture.Format),
            .mipLevel       = Slice.Mip,
            .baseArrayLayer = Slice.Layer,
            .layerCount     = Slice.LayerCount
        };
    }

    static VkExtent3D SliceExtent(const FTexture& Texture, const FTextureSlice& Slice)
    {
        if (Slice.Extent.x != 0)
        {
            return { Slice.Extent.x, Math::Max(Slice.Extent.y, 1u), Math::Max(Slice.Extent.z, 1u) };
        }

        const FTextureDesc& Desc = Texture.Desc;
        const uint32 DepthDim = Desc.Type == ETextureType::Tex3D ? Math::Max(Desc.Dimension.z, 1u) : 1u;

        return
        {
            Math::Max(Desc.Dimension.x >> Slice.Mip, 1u),
            Math::Max(Desc.Dimension.y >> Slice.Mip, 1u),
            Math::Max(DepthDim >> Slice.Mip, 1u)
        };
    }

    void CmdCopyTexture(FCmdListH CL, FTextureH Source, const FTextureSlice& SourceSlice, FTextureH Dest, const FTextureSlice& DestSlice)
    {
        const FTexture& SourceTexture = GDevice->Textures[Source];
        const FTexture& DestTexture   = GDevice->Textures[Dest];

        const VkExtent3D Extent = SliceExtent(SourceTexture, SourceSlice);

        VkImageCopy Region
        {
            .srcSubresource = SliceLayers(SourceTexture, SourceSlice),
            .srcOffset      = { (int32)SourceSlice.Offset.x, (int32)SourceSlice.Offset.y, (int32)SourceSlice.Offset.z },
            .dstSubresource = SliceLayers(DestTexture, DestSlice),
            .dstOffset      = { (int32)DestSlice.Offset.x, (int32)DestSlice.Offset.y, (int32)DestSlice.Offset.z },
            .extent         = Extent
        };

        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdCopyImage(VkCmdBuf, SourceTexture.Image, VK_IMAGE_LAYOUT_GENERAL, DestTexture.Image, VK_IMAGE_LAYOUT_GENERAL, 1, &Region);
    }

    void CmdCopyMemoryToTexture(FCmdListH CL, GPUPtr Source, uint32 RowLength, FTextureH Dest, const FTextureSlice& Slice)
    {
        VkBuffer SourceBuffer;
        VkDeviceSize SourceOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(Source);
            if (BufferIt == nullptr)
            {
                return;
            }

            SourceBuffer = BufferIt->Buffer;
            SourceOffset = Source - BufferIt->Device;
        }

        const FTexture& DestTexture = GDevice->Textures[Dest];

        const uint8 BlockW = RHI::Format::Info(DestTexture.Format).BlockSize;
        const uint32 RowLengthBlocks = (BlockW > 1) ? Math::AlignUp(RowLength, (uint32)BlockW) : RowLength;

        // Clamped against what is left PAST THE OFFSET, because the caller's mip size comes from cooked data.
        // imageOffset has no clamp of its own, so an offset already outside the subresource is dropped.
        VkExtent3D Extent = SliceExtent(DestTexture, Slice);
        {
            const FTextureDesc& Desc = DestTexture.Desc;
            const uint32 DepthDim = (Desc.Type == ETextureType::Tex3D) ? Math::Max(Desc.Dimension.z, 1u) : 1u;

            const uint32 MipW = Math::Max(Desc.Dimension.x >> Slice.Mip, 1u);
            const uint32 MipH = Math::Max(Desc.Dimension.y >> Slice.Mip, 1u);
            const uint32 MipD = Math::Max(DepthDim >> Slice.Mip, 1u);

            if (Slice.Offset.x >= MipW || Slice.Offset.y >= MipH || Slice.Offset.z >= MipD)
            {
                LOG_ERROR("RHI: dropped a texture copy whose offset ({}, {}, {}) is outside mip {} "
                          "({}x{}x{}). The caller's mip dimensions disagree with the image's own chain.",
                    Slice.Offset.x, Slice.Offset.y, Slice.Offset.z, Slice.Mip, MipW, MipH, MipD);
                return;
            }

            Extent.width  = Math::Min(Extent.width,  MipW - Slice.Offset.x);
            Extent.height = Math::Min(Extent.height, MipH - Slice.Offset.y);
            Extent.depth  = Math::Min(Extent.depth,  MipD - Slice.Offset.z);
        }

        // bufferRowLength must be 0 or >= imageExtent.width. A shorter row is out of spec and the copy
        // engine walks past the source, surfacing only as a device-lost with a write fault at address 0.
        if (RowLengthBlocks != 0 && RowLengthBlocks < Extent.width)
        {
            LOG_ERROR("RHI: dropped a texture copy with an out-of-spec region (rowLength {} < extent width {}, mip {}). "
                      "Pass the mip's own dimensions to Upload.", RowLengthBlocks, Extent.width, Slice.Mip);
            return;
        }

        VkBufferImageCopy Region
        {
            .bufferOffset       = SourceOffset,
            .bufferRowLength    = RowLengthBlocks,
            .bufferImageHeight  = 0,
            .imageSubresource   = SliceLayers(DestTexture, Slice),
            .imageOffset        = { (int32)Slice.Offset.x, (int32)Slice.Offset.y, (int32)Slice.Offset.z },
            .imageExtent        = Extent
        };

        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdCopyBufferToImage(VkCmdBuf, SourceBuffer, DestTexture.Image, VK_IMAGE_LAYOUT_GENERAL, 1, &Region);
    }

    void CmdCopyTextureToMemory(FCmdListH CL, FTextureH Source, const FTextureSlice& Slice, GPUPtr Dest, uint32 RowLength)
    {
        VkBuffer DestBuffer;
        VkDeviceSize DestOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(Dest);
            if (BufferIt == nullptr)
            {
                return;
            }

            DestBuffer = BufferIt->Buffer;
            DestOffset = Dest - BufferIt->Device;
        }

        const FTexture& SourceTexture = GDevice->Textures[Source];

        // See CmdCopyMemoryToTexture: block-compressed formats need bufferRowLength block-aligned.
        const uint8 BlockW = RHI::Format::Info(SourceTexture.Format).BlockSize;
        const uint32 RowLengthBlocks = (BlockW > 1) ? Math::AlignUp(RowLength, (uint32)BlockW) : RowLength;

        VkBufferImageCopy Region
        {
            .bufferOffset       = DestOffset,
            .bufferRowLength    = RowLengthBlocks,
            .bufferImageHeight  = 0,
            .imageSubresource   = SliceLayers(SourceTexture, Slice),
            .imageOffset        = { (int32)Slice.Offset.x, (int32)Slice.Offset.y, (int32)Slice.Offset.z },
            .imageExtent        = SliceExtent(SourceTexture, Slice)
        };

        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdCopyImageToBuffer(VkCmdBuf, SourceTexture.Image, VK_IMAGE_LAYOUT_GENERAL, DestBuffer, 1, &Region);
    }

    void CmdBlitTexture(FCmdListH CL, FTextureH Source, const FTextureSlice& SourceSlice, FTextureH Dest, const FTextureSlice& DestSlice, EFilter Filter)
    {
        const FTexture& SourceTexture = GDevice->Textures[Source];
        const FTexture& DestTexture   = GDevice->Textures[Dest];

        const VkExtent3D SourceExtent = SliceExtent(SourceTexture, SourceSlice);
        const VkExtent3D DestExtent   = SliceExtent(DestTexture, DestSlice);

        VkImageBlit Region
        {
            .srcSubresource = SliceLayers(SourceTexture, SourceSlice),
            .srcOffsets =
            {
                { (int32)SourceSlice.Offset.x, (int32)SourceSlice.Offset.y, (int32)SourceSlice.Offset.z },
                { (int32)(SourceSlice.Offset.x + SourceExtent.width), (int32)(SourceSlice.Offset.y + SourceExtent.height), (int32)(SourceSlice.Offset.z + SourceExtent.depth) }
            },
            .dstSubresource = SliceLayers(DestTexture, DestSlice),
            .dstOffsets =
            {
                { (int32)DestSlice.Offset.x, (int32)DestSlice.Offset.y, (int32)DestSlice.Offset.z },
                { (int32)(DestSlice.Offset.x + DestExtent.width), (int32)(DestSlice.Offset.y + DestExtent.height), (int32)(DestSlice.Offset.z + DestExtent.depth) }
            }
        };

        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdBlitImage(VkCmdBuf, SourceTexture.Image, VK_IMAGE_LAYOUT_GENERAL, DestTexture.Image, VK_IMAGE_LAYOUT_GENERAL, 1, &Region, ToVkFilter(Filter));
    }

    void CmdResolveTexture(FCmdListH CL, FTextureH Source, FTextureH Dest)
    {
        const FTexture& SourceTexture = GDevice->Textures[Source];
        const FTexture& DestTexture   = GDevice->Textures[Dest];

        VkImageResolve Region
        {
            .srcSubresource = SliceLayers(SourceTexture, {}),
            .srcOffset      = { 0, 0, 0 },
            .dstSubresource = SliceLayers(DestTexture, {}),
            .dstOffset      = { 0, 0, 0 },
            .extent         = SliceExtent(SourceTexture, {})
        };

        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdResolveImage(VkCmdBuf, SourceTexture.Image, VK_IMAGE_LAYOUT_GENERAL, DestTexture.Image, VK_IMAGE_LAYOUT_GENERAL, 1, &Region);
    }

    static constexpr VkImageSubresourceRange FullSubresourceRange(VkImageAspectFlags Aspect)
    {
        return { Aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    }

    void CmdClearTexture(FCmdListH CL, FTextureH Texture, const float Value[4])
    {
        const FTexture& TextureData = GDevice->Textures[Texture];
        const VkImageAspectFlags Aspect = AspectsForFormat(TextureData.Format);
        const VkImageSubresourceRange Range = FullSubresourceRange(Aspect);

        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        if (Aspect & VK_IMAGE_ASPECT_COLOR_BIT)
        {
            VkClearColorValue Clear;
            Clear.float32[0] = Value[0];
            Clear.float32[1] = Value[1];
            Clear.float32[2] = Value[2];
            Clear.float32[3] = Value[3];

            vkCmdClearColorImage(VkCmdBuf, TextureData.Image, VK_IMAGE_LAYOUT_GENERAL, &Clear, 1, &Range);
        }
        else
        {
            const VkClearDepthStencilValue Clear { Value[0], static_cast<uint32>(Value[1]) };
            vkCmdClearDepthStencilImage(VkCmdBuf, TextureData.Image, VK_IMAGE_LAYOUT_GENERAL, &Clear, 1, &Range);
        }
    }

    void CmdClearTextureUInt(FCmdListH CL, FTextureH Texture, const uint32 Value[4])
    {
        const FTexture& TextureData = GDevice->Textures[Texture];
        const VkImageSubresourceRange Range = FullSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);

        VkClearColorValue Clear;
        Clear.uint32[0] = Value[0];
        Clear.uint32[1] = Value[1];
        Clear.uint32[2] = Value[2];
        Clear.uint32[3] = Value[3];

        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdClearColorImage(VkCmdBuf, TextureData.Image, VK_IMAGE_LAYOUT_GENERAL, &Clear, 1, &Range);
    }

    static VkAccessFlags2 AccessForStages(EStageFlags Stages)
    {
        VkAccessFlags2 Access = 0;

        if (EnumHasAnyFlags(Stages, EStageFlags::AllCommands))
        {
            return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        }
        if (EnumHasAnyFlags(Stages, EStageFlags::FragmentTests))
        {
            Access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }
        if (EnumHasAnyFlags(Stages, EStageFlags::RasterColorOut))
        {
            Access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        }
        if (EnumHasAnyFlags(Stages, EStageFlags::Compute | EStageFlags::PixelShader | EStageFlags::VertexShader |
                                    EStageFlags::MeshShader))
        {
            Access |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        }
        if (EnumHasAnyFlags(Stages, EStageFlags::Transfer))
        {
            Access |= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
        if (EnumHasAnyFlags(Stages, EStageFlags::Host))
        {
            Access |= VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
        }
        if (EnumHasAnyFlags(Stages, EStageFlags::IndirectArguments))
        {
            Access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }

        return Access != 0 ? Access : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    static EStageFlags ClampStagesToQueue(EStageFlags Stages, EQueueType Queue)
    {
        if (Queue == EQueueType::Graphics)
        {
            return Stages;
        }

        constexpr EStageFlags GraphicsOnly =
            EStageFlags::RasterColorOut | EStageFlags::PixelShader | EStageFlags::FragmentTests |
            EStageFlags::VertexShader   | EStageFlags::MeshShader;

        EStageFlags Clamped = Stages & ~GraphicsOnly;
        if (Queue == EQueueType::Transfer)
        {
            Clamped &= ~(EStageFlags::Compute | EStageFlags::IndirectArguments);
        }

        if (Clamped == EStageFlags::None)
        {
            Clamped = EStageFlags::AllCommands;
        }

        return Clamped;
    }

    void CmdImageBarrier(FCmdListH CL, FTextureH Texture, EStageFlags Before, EStageFlags After)
    {
        if (!IsValid(Texture))
        {
            return;
        }

        const EQueueType Queue = GDevice->CommandLists[CL].Queue;
        Before = ClampStagesToQueue(Before, Queue);
        After  = ClampStagesToQueue(After, Queue);

        const FTexture& TextureData = GDevice->Textures[Texture];

        // GENERAL on both sides: the layout is not what we are after, the named-image dependency is.
        VkImageMemoryBarrier2 BarrierInfo
        {
            .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext                  = nullptr,
            .srcStageMask           = ToVkPipelineState(Before),
            .srcAccessMask          = AccessForStages(Before),
            .dstStageMask           = ToVkPipelineState(After),
            .dstAccessMask          = AccessForStages(After),
            .oldLayout              = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout              = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex    = VK_QUEUE_FAMILY_IGNORED,
            .image                  = TextureData.Image,
            .subresourceRange =
                {
                    .aspectMask     = AspectsForFormat(TextureData.Format),
                    .baseMipLevel   = 0,
                    .levelCount     = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount     = VK_REMAINING_ARRAY_LAYERS
                }
        };

        VkDependencyInfo DepInfo
        {
            .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                      = nullptr,
            .dependencyFlags            = 0,
            .imageMemoryBarrierCount    = 1,
            .pImageMemoryBarriers       = &BarrierInfo
        };

        vkCmdPipelineBarrier2(GDevice->CommandLists[CL].CommandBuffer, &DepInfo);
    }

    static void CmdQueueOwnershipTransfer(FCmdListH CL, FTextureH Texture, EQueueType Source, EQueueType Dest,
                                          EStageFlags Stages, bool bRelease)
    {
        if (!IsValid(Texture))
        {
            return;
        }

        const uint32 SourceFamily = GDevice->QueueFamilies[(uint32)Source];
        const uint32 DestFamily   = GDevice->QueueFamilies[(uint32)Dest];

        if (SourceFamily == DestFamily)
        {
            return;
        }

        const FTexture& TextureData = GDevice->Textures[Texture];

        // Layouts must MATCH between the two halves. GENERAL on both sides, same as CmdImageBarrier.
        VkImageMemoryBarrier2 BarrierInfo
        {
            .sType                  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext                  = nullptr,
            .srcStageMask           = bRelease ? ToVkPipelineState(ClampStagesToQueue(Stages, Source)) : VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask          = bRelease ? AccessForStages(Stages) : 0,
            .dstStageMask           = bRelease ? VK_PIPELINE_STAGE_2_NONE : ToVkPipelineState(ClampStagesToQueue(Stages, Dest)),
            .dstAccessMask          = bRelease ? 0 : AccessForStages(Stages),
            .oldLayout              = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout              = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex    = SourceFamily,
            .dstQueueFamilyIndex    = DestFamily,
            .image                  = TextureData.Image,
            .subresourceRange =
                {
                    .aspectMask     = AspectsForFormat(TextureData.Format),
                    .baseMipLevel   = 0,
                    .levelCount     = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount     = VK_REMAINING_ARRAY_LAYERS
                }
        };

        VkDependencyInfo DepInfo
        {
            .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                      = nullptr,
            .dependencyFlags            = 0,
            .imageMemoryBarrierCount    = 1,
            .pImageMemoryBarriers       = &BarrierInfo
        };

        vkCmdPipelineBarrier2(GDevice->CommandLists[CL].CommandBuffer, &DepInfo);
    }

    void CmdReleaseImageToQueue(FCmdListH CL, FTextureH Texture, EQueueType Dest, EStageFlags Before)
    {
        CmdQueueOwnershipTransfer(CL, Texture, GDevice->CommandLists[CL].Queue, Dest, Before, /*bRelease*/ true);
    }

    void CmdAcquireImageFromQueue(FCmdListH CL, FTextureH Texture, EQueueType Source, EStageFlags After)
    {
        CmdQueueOwnershipTransfer(CL, Texture, Source, GDevice->CommandLists[CL].Queue, After, /*bRelease*/ false);
    }

    void CmdBarrier(FCmdListH CL, EStageFlags Before, EStageFlags After)
    {
        const EQueueType Queue = GDevice->CommandLists[CL].Queue;

        const VkPipelineStageFlags2 SrcStage = ToVkPipelineState(ClampStagesToQueue(Before, Queue));
        const VkPipelineStageFlags2 DstStage = ToVkPipelineState(ClampStagesToQueue(After, Queue));
        
        constexpr auto Access = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        
        VkMemoryBarrier2 BarrierInfo
        {
            .sType          = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext          = nullptr,
            .srcStageMask   = SrcStage,
            .srcAccessMask  = Access,
            .dstStageMask   = DstStage,
            .dstAccessMask  = Access
        };
        
        VkDependencyInfo DepInfo
        {
            .sType                      = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext                      = nullptr,
            .dependencyFlags            = 0,
            .memoryBarrierCount         = 1,
            .pMemoryBarriers            = &BarrierInfo,
            .bufferMemoryBarrierCount   = 0,
            .pBufferMemoryBarriers      = nullptr,
            .imageMemoryBarrierCount    = 0,
            .pImageMemoryBarriers       = nullptr
        };
        
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdPipelineBarrier2(VkCmdBuf, &DepInfo);
    }

    void CmdBeginRenderPass(FCmdListH CL, const FRenderPassDesc& Desc)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        FMemMark Mark;
        const uint32 ColorCount = static_cast<uint32>(Desc.ColorAttachments.size());
        auto* ColorInfos = Mark.AllocArray<VkRenderingAttachmentInfo>(ColorCount);

        for (uint32 i = 0; i < ColorCount; ++i)
        {
            const FRenderAttachment& Attachment = Desc.ColorAttachments[i];
            const bool bResolve = IsValid(Attachment.ResolveTexture);

            ColorInfos[i].sType                 = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            ColorInfos[i].pNext                 = nullptr;
            ColorInfos[i].imageView             = GDevice->Textures[Attachment.Texture].DefaultImageView;
            ColorInfos[i].imageLayout           = VK_IMAGE_LAYOUT_GENERAL;
            ColorInfos[i].resolveMode           = bResolve ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE;
            ColorInfos[i].resolveImageView      = bResolve ? GDevice->Textures[Attachment.ResolveTexture].DefaultImageView : VK_NULL_HANDLE;
            ColorInfos[i].resolveImageLayout    = bResolve ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            ColorInfos[i].loadOp                = ToVkLoadOp(Attachment.LoadOp);
            ColorInfos[i].storeOp               = ToVkStoreOp(Attachment.StoreOp);
            ColorInfos[i].clearValue.color      = { { Attachment.Color[0], Attachment.Color[1], Attachment.Color[2], Attachment.Color[3] } };
        }

        const bool bHasDepth = IsValid(Desc.DepthAttachment.Texture);
        VkRenderingAttachmentInfo DepthInfo{};
        if (bHasDepth)
        {
            const bool bResolve = IsValid(Desc.DepthAttachment.ResolveTexture);

            DepthInfo.sType                         = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            DepthInfo.pNext                         = nullptr;
            DepthInfo.imageView                     = GDevice->Textures[Desc.DepthAttachment.Texture].DefaultImageView;
            DepthInfo.imageLayout                   = VK_IMAGE_LAYOUT_GENERAL;
            DepthInfo.resolveMode                   = bResolve ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
            DepthInfo.resolveImageView              = bResolve ? GDevice->Textures[Desc.DepthAttachment.ResolveTexture].DefaultImageView : VK_NULL_HANDLE;
            DepthInfo.resolveImageLayout            = bResolve ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            DepthInfo.loadOp                        = ToVkLoadOp(Desc.DepthAttachment.LoadOp);
            DepthInfo.storeOp                       = ToVkStoreOp(Desc.DepthAttachment.StoreOp);
            DepthInfo.clearValue.depthStencil.depth = Desc.DepthAttachment.Color[0];
        }

        const bool bHasStencil = IsValid(Desc.StencilAttachment.Texture);
        VkRenderingAttachmentInfo StencilInfo{};
        if (bHasStencil)
        {
            StencilInfo.sType                           = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            StencilInfo.pNext                           = nullptr;
            StencilInfo.imageView                       = GDevice->Textures[Desc.StencilAttachment.Texture].DefaultImageView;
            StencilInfo.imageLayout                     = VK_IMAGE_LAYOUT_GENERAL;
            StencilInfo.resolveMode                     = VK_RESOLVE_MODE_NONE;
            StencilInfo.loadOp                          = ToVkLoadOp(Desc.StencilAttachment.LoadOp);
            StencilInfo.storeOp                         = ToVkStoreOp(Desc.StencilAttachment.StoreOp);
            StencilInfo.clearValue.depthStencil.stencil = static_cast<uint32>(Desc.StencilAttachment.Color[0]);
        }

        VkRenderingInfo RenderingInfo
        {
            .sType                  = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext                  = nullptr,
            .flags                  = 0,
            .renderArea             = { { 0, 0 }, { Desc.RenderArea.x, Desc.RenderArea.y } },
            .layerCount             = 1,
            .viewMask               = 0,
            .colorAttachmentCount   = ColorCount,
            .pColorAttachments      = ColorInfos,
            .pDepthAttachment       = bHasDepth ? &DepthInfo : nullptr,
            .pStencilAttachment     = bHasStencil ? &StencilInfo : nullptr,
        };

        vkCmdBeginRendering(VkCmdBuf, &RenderingInfo);

        VkViewport Viewport
        {
            .x          = 0.0f,
            .y          = 0.0f,
            .width      = static_cast<float>(Desc.RenderArea.x),
            .height     = static_cast<float>(Desc.RenderArea.y),
            .minDepth   = 0.0f,
            .maxDepth   = 1.0f,
        };

        VkRect2D Scissor { { 0, 0 }, { Desc.RenderArea.x, Desc.RenderArea.y } };

        vkCmdSetViewportWithCount(VkCmdBuf, 1, &Viewport);
        vkCmdSetScissorWithCount(VkCmdBuf, 1, &Scissor);
    }

    void CmdEndRenderPass(FCmdListH CL)
    {
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdEndRendering(VkCmdBuf);
    }

    void CmdSetTextureHeap(FCmdListH CL, FTextureHeapH Heap)
    {
        const FCommandList& List = GDevice->CommandLists[CL];
        auto* DescriptorSet = GDevice->TextureHeaps[Heap].DescriptorSet;

        if (List.Queue == EQueueType::Graphics)
        {
            vkCmdBindDescriptorSets(List.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, GDevice->PipelineLayout, 0, 1, &DescriptorSet, 0, nullptr);
        }
        vkCmdBindDescriptorSets(List.CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, GDevice->PipelineLayout, 0, 1, &DescriptorSet, 0, nullptr);
    }

    void CmdSetDepthStencilState(FCmdListH CL, FDepthStencilH DepthStencil)
    {
        auto* VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        const FDepthStencilState& State = GDevice->DepthStates[DepthStencil];
        
        vkCmdSetDepthWriteEnable(VkCmdBuf, EnumHasAnyFlags(State.DepthMode, EDepthFlags::Write));
        vkCmdSetDepthTestEnable(VkCmdBuf, EnumHasAnyFlags(State.DepthMode, EDepthFlags::Read));
        vkCmdSetDepthCompareOp(VkCmdBuf, ToVkCompareOp(State.DepthTest));
        vkCmdSetDepthBias(VkCmdBuf, State.DepthBias, State.DepthBiasClamp, State.DepthBiasSlopeFactor);
        vkCmdSetDepthBoundsTestEnable(VkCmdBuf, VK_FALSE);
        vkCmdSetDepthBounds(VkCmdBuf, 0.0f, 1.0f);

        const bool bStencilEnabled = State.StencilFront.Test != EOp::Always || State.StencilBack.Test != EOp::Always;
        vkCmdSetStencilTestEnable(VkCmdBuf, bStencilEnabled);
        vkCmdSetStencilOp(VkCmdBuf, VK_STENCIL_FACE_FRONT_BIT,
            ToVkStencilOp(State.StencilFront.FailOp),
            ToVkStencilOp(State.StencilFront.PassOp),
            ToVkStencilOp(State.StencilFront.DepthFailOp),
            ToVkCompareOp(State.StencilFront.Test));
        
        vkCmdSetStencilReference(VkCmdBuf, VK_STENCIL_FACE_FRONT_BIT, State.StencilFront.Reference);
        
        vkCmdSetStencilOp(VkCmdBuf, VK_STENCIL_FACE_BACK_BIT, 
            ToVkStencilOp(State.StencilBack.FailOp),
            ToVkStencilOp(State.StencilBack.PassOp),
            ToVkStencilOp(State.StencilBack.DepthFailOp),
            ToVkCompareOp(State.StencilBack.Test));   
        
        vkCmdSetStencilReference(VkCmdBuf, VK_STENCIL_FACE_BACK_BIT, State.StencilBack.Reference);
        vkCmdSetStencilWriteMask(VkCmdBuf, VK_STENCIL_FACE_FRONT_AND_BACK, State.StencilWriteMask);
        vkCmdSetStencilCompareMask(VkCmdBuf, VK_STENCIL_FACE_FRONT_AND_BACK, State.StencilReadMask);
    }

    void CmdSetFrontFace(FCmdListH CL, EFrontFace Front)
    {
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdSetFrontFace(VkCmdBuf, ToVkFrontFace(Front));
    }

    void CmdSetCullMode(FCmdListH CL, ECullMode Mode)
    {
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdSetCullMode(VkCmdBuf, ToVkCullModeFlags(Mode));
    }

    void CmdSetLineWidth(FCmdListH CL, float Width)
    {
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdSetLineWidth(VkCmdBuf, Width);
    }

    void CmdSetPipeline(FCmdListH CL, FPipelineH Pipeline)
    {
        FPipeline PL = GDevice->Pipelines[Pipeline];
        FCommandList& List = GDevice->CommandLists[CL];
        vkCmdBindPipeline(List.CommandBuffer, PL.BindPoint, PL.Pipeline);
    }

    void CmdSetScissor(FCmdListH CL, const FRect& Rect)
    {
        VkRect2D Scissor = {};
        Scissor.offset.x = (int32)Rect.MinX;
        Scissor.offset.y = (int32)Rect.MinY;
        Scissor.extent.width = Math::Abs((int32)(Rect.MaxX - Rect.MinX));
        Scissor.extent.height = Math::Abs((int32)(Rect.MaxY - Rect.MinY));
        
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdSetScissorWithCount(VkCmdBuf, 1, &Scissor);
    }

    void CmdSetViewport(FCmdListH CL, const FRect& Rect)
    {
        VkViewport Viewport = {};
        Viewport.x        = static_cast<float>(Rect.MinX);
        Viewport.y        = static_cast<float>(Rect.MinY);
        Viewport.width    = static_cast<float>(Rect.MaxX) - static_cast<float>(Rect.MinX);
        Viewport.height   = static_cast<float>(Rect.MaxY) - static_cast<float>(Rect.MinY);
        Viewport.minDepth = 0.0f;
        Viewport.maxDepth = 1.0f;
        
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        vkCmdSetViewportWithCount(VkCmdBuf, 1, &Viewport);
    }

    static bool BindIndexBuffer(FCommandList& CommandList, GPUPtr IndexBuffer, uint32 IndexOffset, EIndexType IndexType)
    {
        const GPUPtr BindKey = IndexBuffer + IndexOffset;
        const VkIndexType VulkanIndexType = ToVkIndexType(IndexType);

        if (BindKey == CommandList.CurrentIndexBuffer && VulkanIndexType == CommandList.CurrentIndexType)
        {
            return true;
        }

        VkBuffer VulkanIndexBuffer;
        VkDeviceSize BufferOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(IndexBuffer);
            if (BufferIt == nullptr)
            {
                return false;
            }

            VulkanIndexBuffer = BufferIt->Buffer;
            BufferOffset      = (IndexBuffer - BufferIt->Device) + IndexOffset;
        }

        vkCmdBindIndexBuffer(CommandList.CommandBuffer, VulkanIndexBuffer, BufferOffset, VulkanIndexType);
        CommandList.CurrentIndexBuffer = BindKey;
        CommandList.CurrentIndexType   = VulkanIndexType;

        return true;
    }

    void CmdSetIndexBuffer(FCmdListH CL, GPUPtr IndexBuffer, uint32 Offset, EIndexType IndexType)
    {
        BindIndexBuffer(GDevice->CommandLists[CL], IndexBuffer, Offset, IndexType);
    }

    void CmdDispatch(FCmdListH CL, GPUPtr DrawArgs, uint32 GroupX, uint32 GroupY, uint32 GroupZ)
    {
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        if (DrawArgs != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        }

        vkCmdDispatch(VkCmdBuf, GroupX, GroupY, GroupZ);
    }

    void CmdDispatchIndirect(FCmdListH CL, GPUPtr DrawArgs, uint32 Offset)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        VkBuffer ArgsBuffer;
        VkDeviceSize BufferOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(DrawArgs);
            if (BufferIt == nullptr)
            {
                return;
            }

            ArgsBuffer   = BufferIt->Buffer;
            BufferOffset = (DrawArgs - BufferIt->Device) + Offset;
        }

        vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        vkCmdDispatchIndirect(VkCmdBuf, ArgsBuffer, BufferOffset);
    }

    void CmdDraw(FCmdListH CL, GPUPtr DrawArgs, uint32 VertexCount, uint32 InstanceCount, uint32 FirstVertex, uint32 FirstInstance)
    {
        auto VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        if (DrawArgs != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        }
        
        vkCmdDraw(VkCmdBuf, VertexCount, InstanceCount, FirstVertex, FirstInstance);
    }

    void CmdDrawIndexed(FCmdListH CL, GPUPtr IndexBuffer, uint32 IndexOffset, GPUPtr DrawArgs, uint32 IndexCount, uint32 InstanceCount, uint32 FirstIndex, int32 VertexOffset, uint32 FirstInstance, EIndexType IndexType)
    {
        auto& CommandList           = GDevice->CommandLists[CL];
        VkCommandBuffer VkCmdBuf    = CommandList.CommandBuffer;

        if (DrawArgs != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        }

        if (!BindIndexBuffer(CommandList, IndexBuffer, IndexOffset, IndexType))
        {
            return;
        }

        vkCmdDrawIndexed(VkCmdBuf, IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance);
    }

    void CmdDrawIndirect(FCmdListH CL, GPUPtr DrawArgs, uint32 Offset, uint32 DrawCount, uint32 Stride)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        VkBuffer ArgsBuffer;
        VkDeviceSize BufferOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(DrawArgs);
            if (BufferIt == nullptr)
            {
                return;
            }

            ArgsBuffer   = BufferIt->Buffer;
            BufferOffset = (DrawArgs - BufferIt->Device) + Offset;
        }

        vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        vkCmdDrawIndirect(VkCmdBuf, ArgsBuffer, BufferOffset, DrawCount, Stride);
    }

    void CmdDrawIndirect(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint32 Offset, uint32 DrawCount, uint32 Stride)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        VkBuffer ArgsBuffer;
        VkDeviceSize BufferOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(IndirectBuffer);
            if (BufferIt == nullptr)
            {
                return;
            }

            ArgsBuffer   = BufferIt->Buffer;
            BufferOffset = (IndirectBuffer - BufferIt->Device) + Offset;
        }

        if (Args != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &Args);
        }
        vkCmdDrawIndirect(VkCmdBuf, ArgsBuffer, BufferOffset, DrawCount, Stride);
    }

    void CmdDispatchIndirect(FCmdListH CL, GPUPtr Args, GPUPtr IndirectBuffer, uint32 Offset)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        VkBuffer ArgsBuffer;
        VkDeviceSize BufferOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(IndirectBuffer);
            if (BufferIt == nullptr)
            {
                return;
            }

            ArgsBuffer   = BufferIt->Buffer;
            BufferOffset = (IndirectBuffer - BufferIt->Device) + Offset;
        }

        if (Args != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &Args);
        }
        vkCmdDispatchIndirect(VkCmdBuf, ArgsBuffer, BufferOffset);
    }

    void CmdDrawIndexedIndirect(FCmdListH CL, GPUPtr DrawArgs, uint32 Offset, uint32 DrawCount, uint32 Stride)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        VkBuffer ArgsBuffer;
        VkDeviceSize BufferOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(DrawArgs);
            if (BufferIt == nullptr)
            {
                return;
            }

            ArgsBuffer   = BufferIt->Buffer;
            BufferOffset = (DrawArgs - BufferIt->Device) + Offset;
        }

        vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        vkCmdDrawIndexedIndirect(VkCmdBuf, ArgsBuffer, BufferOffset, DrawCount, Stride);
    }

    void CmdDrawMeshTasks(FCmdListH CL, GPUPtr DrawArgs, uint32 GroupCountX, uint32 GroupCountY, uint32 GroupCountZ)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;
        if (DrawArgs != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        }

        vkCmdDrawMeshTasksEXT(VkCmdBuf, GroupCountX, GroupCountY, GroupCountZ);
    }

    void CmdDrawMeshTasksIndirect(FCmdListH CL, GPUPtr DrawArgs, GPUPtr IndirectBuffer, uint32 Offset, uint32 DrawCount, uint32 Stride)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        VkBuffer ArgsBuffer;
        VkDeviceSize BufferOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* BufferIt = FindMemory(IndirectBuffer);
            if (BufferIt == nullptr)
            {
                return;
            }

            ArgsBuffer   = BufferIt->Buffer;
            BufferOffset = (IndirectBuffer - BufferIt->Device) + Offset;
        }

        if (DrawArgs != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        }
        vkCmdDrawMeshTasksIndirectEXT(VkCmdBuf, ArgsBuffer, BufferOffset, DrawCount, Stride);
    }

    void CmdDrawMeshTasksIndirectCount(FCmdListH CL, GPUPtr DrawArgs, GPUPtr IndirectBuffer, uint32 Offset, GPUPtr CountBuffer, uint32 CountOffset, uint32 MaxDrawCount, uint32 Stride)
    {
        VkCommandBuffer VkCmdBuf = GDevice->CommandLists[CL].CommandBuffer;

        VkBuffer ArgsBuffer;     VkDeviceSize ArgsOffset;
        VkBuffer CountVkBuffer;  VkDeviceSize CountVkOffset;

        {
            FReadScopeLock Lock(GDevice->MemoryMutex);
            const FMemoryBlock* ArgsIt  = FindMemory(IndirectBuffer);
            const FMemoryBlock* CountIt = FindMemory(CountBuffer);
            if (ArgsIt == nullptr || CountIt == nullptr)
            {
                return;
            }

            ArgsBuffer    = ArgsIt->Buffer;
            ArgsOffset    = (IndirectBuffer - ArgsIt->Device) + Offset;
            CountVkBuffer = CountIt->Buffer;
            CountVkOffset = (CountBuffer - CountIt->Device) + CountOffset;
        }

        if (DrawArgs != 0)
        {
            vkCmdPushConstants(VkCmdBuf, GDevice->PipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(VkDeviceAddress), &DrawArgs);
        }
        vkCmdDrawMeshTasksIndirectCountEXT(VkCmdBuf, ArgsBuffer, ArgsOffset, CountVkBuffer, CountVkOffset, MaxDrawCount, Stride);
    }

    void CmdBeginMarker(FCmdListH CL, const char* Name)
    {
        FCommandList& List = GDevice->CommandLists[CL];

        if (List.BreadcrumbDepth < FGpuBreadcrumbs::MaxDepth)
        {
            List.BreadcrumbStack[List.BreadcrumbDepth] =
                GDevice->Breadcrumbs.Begin(List.CommandBuffer, Name, List.BreadcrumbDepth);
        }
        ++List.BreadcrumbDepth;

        if (vkCmdBeginDebugUtilsLabelEXT != nullptr)
        {
            VkDebugUtilsLabelEXT Label
            {
                .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pNext      = nullptr,
                .pLabelName = Name,
                .color      = { 0.0f, 0.0f, 0.0f, 0.0f }
            };

            vkCmdBeginDebugUtilsLabelEXT(List.CommandBuffer, &Label);
        }

#if defined(TRACY_ENABLE)
        tracy::VkCtx* ZoneContext = GTracyGPUContexts[(uint32)List.Queue];
        if (ZoneContext != nullptr && List.GPUZoneDepth < kMaxGPUZoneDepth)
        {
            void* Slot = &List.GPUZoneStack[List.GPUZoneDepth * sizeof(tracy::VkCtxScope)];
            new (Slot) tracy::VkCtxScope(ZoneContext, 0u, __FILE__, sizeof(__FILE__) - 1,
                "GPUMarker", 9, Name, strlen(Name), List.CommandBuffer, true);
        }
        ++List.GPUZoneDepth;
#endif
    }

    void CmdEndMarker(FCmdListH CL)
    {
        FCommandList& List = GDevice->CommandLists[CL];

        if (List.BreadcrumbDepth > 0)
        {
            --List.BreadcrumbDepth;
            if (List.BreadcrumbDepth < FGpuBreadcrumbs::MaxDepth)
            {
                GDevice->Breadcrumbs.End(List.CommandBuffer, List.BreadcrumbStack[List.BreadcrumbDepth]);
            }
        }

        #if defined(TRACY_ENABLE)
        if (List.GPUZoneDepth > 0)
        {
            --List.GPUZoneDepth;
            if (GTracyGPUContexts[(uint32)List.Queue] != nullptr && List.GPUZoneDepth < kMaxGPUZoneDepth)
            {
                void* Slot = &List.GPUZoneStack[List.GPUZoneDepth * sizeof(tracy::VkCtxScope)];
                static_cast<tracy::VkCtxScope*>(Slot)->~VkCtxScope();
            }
        }
        #endif

        if (vkCmdEndDebugUtilsLabelEXT != nullptr)
        {
            vkCmdEndDebugUtilsLabelEXT(List.CommandBuffer);
        }
    }

}
