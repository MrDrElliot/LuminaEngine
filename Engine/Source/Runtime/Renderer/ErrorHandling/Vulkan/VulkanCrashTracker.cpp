#include "pch.h"
#include "VulkanCrashTracker.h"
#include <filesystem>
#include <fstream>
#include <volk/volk.h>
#if WITH_AFTERMATH
// MessageBoxA in the Aftermath error macro; previously reached us only via spdlog's windows.h leak.
#include <windows.h>
#include "NvidiaAftermath/GFSDK_Aftermath.h"
#include "NvidiaAftermath/GFSDK_Aftermath_GpuCrashDumpDecoding.h"
#include <NvidiaAftermath/GFSDK_Aftermath_GpuCrashDump.h>
#endif
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Process/PlatformProcess.h"

namespace Lumina::RHI
{
#if WITH_AFTERMATH
    static FString AftermathErrorMessage(GFSDK_Aftermath_Result Result)
    {
        switch (Result)
        {
        case GFSDK_Aftermath_Result_FAIL_DriverVersionNotSupported:
            return "Unsupported driver version - requires an NVIDIA R495 display driver or newer.";
        default:
            return "Aftermath Error 0x" + eastl::to_string(Result);
        }
    }

#ifdef _WIN32
#define AFTERMATH_CHECK_ERROR(FC)                                                                       \
[&]() {                                                                                                 \
    GFSDK_Aftermath_Result _result = FC;                                                                \
    if (!GFSDK_Aftermath_SUCCEED(_result))                                                              \
    {                                                                                                   \
        MessageBoxA(0, AftermathErrorMessage(_result).c_str(), "Aftermath Error", MB_OK);               \
        exit(1);                                                                                        \
    }                                                                                                   \
}()
#else
#define AFTERMATH_CHECK_ERROR(FC)                                                                       \
[&]() {                                                                                                 \
    GFSDK_Aftermath_Result _result = FC;                                                                \
    if (!GFSDK_Aftermath_SUCCEED(_result))                                                              \
    {                                                                                                   \
        printf("%s\n", AftermathErrorMessage(_result).c_str());                                         \
        fflush(stdout);                                                                                 \
        exit(1);                                                                                        \
    }                                                                                                   \
}()
#endif


    static void GpuCrashDumpCallback(const void* GpuCrashDump, uint32 GpuCrashDumpSize, void* UserData)
    {
        FVulkanCrashTracker* Tracker = static_cast<FVulkanCrashTracker*>(UserData);
        Tracker->GPUCrashDumpCallback(GpuCrashDump, GpuCrashDumpSize);
    }

    static void ShaderDebugInfoCallback(const void* ShaderDebugInfo, uint32 ShaderDebugInfoSize, void* UserData)
    {
        FVulkanCrashTracker* CrashTracker = static_cast<FVulkanCrashTracker*>(UserData);
        CrashTracker->OnShaderDebugInfo(ShaderDebugInfo, ShaderDebugInfoSize);
    }

    static void ShaderDebugInfoLookupCallback(const GFSDK_Aftermath_ShaderDebugInfoIdentifier* pIdentifier, PFN_GFSDK_Aftermath_SetData SetShaderDebugInfo, void* pUserData)
    {
        FVulkanCrashTracker* Tracker = static_cast<FVulkanCrashTracker*>(pUserData);
        Tracker->OnShaderDebugInfoLookup(*pIdentifier, SetShaderDebugInfo);
    }

    static void ShaderLookupCallback(const GFSDK_Aftermath_ShaderBinaryHash* pShaderHash, PFN_GFSDK_Aftermath_SetData SetShaderBinary, void* pUserData)
    {
        FVulkanCrashTracker* Tracker = static_cast<FVulkanCrashTracker*>(pUserData);
        Tracker->OnShaderLookup(*pShaderHash, SetShaderBinary);
    }

    static void ShaderSourceDebugInfoLookupCallback(const GFSDK_Aftermath_ShaderDebugName* pShaderDebugName, PFN_GFSDK_Aftermath_SetData SetShaderBinary, void* pUserData)
    {
        FVulkanCrashTracker* Tracker = static_cast<FVulkanCrashTracker*>(pUserData);
        Tracker->OnShaderSourceDebugInfoLookup(*pShaderDebugName, SetShaderBinary);
    }

    static void CrashDumpDescriptionCallback(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription AddDescription, void* UserData)
    {
        AddDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "Lumina Engine");
        AddDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "1.0");
    }

    static void ResolveMarkerCallback(const void* MarkerData, uint32 MarkerDataSize, void* UserData, PFN_GFSDK_Aftermath_ResolveMarker ResolveMarker)
    {
        // MarkerData is the FString* we passed to vkCmdSetCheckpointNV via MarkerStorage.
        if (MarkerData == nullptr)
        {
            return;
        }

        const FString* Marker = static_cast<const FString*>(MarkerData);
        ResolveMarker(reinterpret_cast<const void*>(Marker->c_str()), static_cast<uint32>(Marker->size()));
    }

    template<typename T>
    static FString ToHexString(T n)
    {
        std::stringstream stream;
        stream << std::setfill('0') << std::setw(2 * sizeof(T)) << std::hex << n;
        return stream.str().c_str();
    }

    static FString ToString(const GFSDK_Aftermath_ShaderDebugInfoIdentifier Identifier)
    {
        return ToHexString(Identifier.id[0]) + "-" + ToHexString(Identifier.id[1]);
    }

    static FString ToString(const GFSDK_Aftermath_ShaderBinaryHash Hash)
    {
        return ToHexString(Hash.hash);
    }

    static uint64 IdentifierKey(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& Id)
    {
        size_t Hash = 0;
        Hash::HashCombine(Hash, Id.id[0]);
        Hash::HashCombine(Hash, Id.id[1]);
        return static_cast<uint64>(Hash);
    }
#endif // WITH_AFTERMATH

    FVulkanCrashTracker::FVulkanCrashTracker()
    {
        CrashDumpDirectory = Paths::Combine(Paths::GetEngineInstallDirectory(), "CrashDumps");
        std::filesystem::create_directories(CrashDumpDirectory.c_str());

        #if WITH_AFTERMATH
        GFSDK_Aftermath_Result Result = GFSDK_Aftermath_EnableGpuCrashDumps(
            GFSDK_Aftermath_Version_API,
            GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
            GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
            GpuCrashDumpCallback,
            ShaderDebugInfoCallback,
            CrashDumpDescriptionCallback,
            ResolveMarkerCallback,
            this
        );

        if (Result != GFSDK_Aftermath_Result_Success)
        {
            LOG_ERROR("Failed to initialize Nvidia Aftermath: {}", static_cast<int>(Result));
            return;
        }

        bInitialized = true;
        LOG_INFO("Nvidia Aftermath crash tracker initialized (Vulkan)");
        #endif
    }

    void FVulkanCrashTracker::Initialize(RHIDevice InDevice, RHIPhysicalDevice InPhysicalDevice)
    {
        Device = static_cast<VkDevice>(InDevice);
        PhysicalDevice = static_cast<VkPhysicalDevice>(InPhysicalDevice);
    }

    void FVulkanCrashTracker::Shutdown()
    {
        #if WITH_AFTERMATH
        if (bInitialized)
        {
            GFSDK_Aftermath_DisableGpuCrashDumps();
            bInitialized = false;
            LOG_INFO("Nvidia Aftermath crash tracker shut down");
        }
        #endif

        {
            FWriteScopeLock Lock(ShaderRegistryMutex);
            RegisteredShaders.clear();
            DebugNameToHash.clear();
        }
        {
            FWriteScopeLock Lock(ShaderDebugInfoMutex);
            ShaderDebugInfos.clear();
        }
        {
            FWriteScopeLock Lock(MarkerMutex);
            MarkerStorage.clear();
        }

        Device = VK_NULL_HANDLE;
        PhysicalDevice = VK_NULL_HANDLE;
    }

    static const char* DriverIDToString(VkDriverId Id)
    {
        switch (Id)
        {
        case VK_DRIVER_ID_AMD_PROPRIETARY:           return "AMD Proprietary (AMDVLK)";
        case VK_DRIVER_ID_AMD_OPEN_SOURCE:           return "AMD Open Source";
        case VK_DRIVER_ID_MESA_RADV:                 return "Mesa RADV";
        case VK_DRIVER_ID_NVIDIA_PROPRIETARY:        return "NVIDIA Proprietary";
        case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS: return "Intel Proprietary";
        case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:    return "Intel Mesa";
        case VK_DRIVER_ID_MESA_LLVMPIPE:             return "Mesa LLVMpipe";
        case VK_DRIVER_ID_MOLTENVK:                  return "MoltenVK";
        default:                                     return "Unknown";
        }
    }

    void FVulkanCrashTracker::LogDeviceInfo() const
    {
        if (PhysicalDevice == VK_NULL_HANDLE)
        {
            return;
        }

        VkPhysicalDeviceDriverProperties DriverProps{};
        DriverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        VkPhysicalDeviceProperties2 Props2{};
        Props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        Props2.pNext = &DriverProps;

        vkGetPhysicalDeviceProperties2(PhysicalDevice, &Props2);

        const uint32 ApiVer = Props2.properties.apiVersion;
        LOG_ERROR("[DeviceLost] GPU: {} | Driver: {} ({}) [{}] | API: {}.{}.{}",
            Props2.properties.deviceName,
            DriverProps.driverName,
            DriverProps.driverInfo,
            DriverIDToString(DriverProps.driverID),
            VK_API_VERSION_MAJOR(ApiVer),
            VK_API_VERSION_MINOR(ApiVer),
            VK_API_VERSION_PATCH(ApiVer));
    }

    void FVulkanCrashTracker::LogDeviceFaultInfo() const
    {
        if (!bDeviceFaultEnabled || Device == VK_NULL_HANDLE || vkGetDeviceFaultInfoEXT == nullptr)
        {
            return;
        }

        VkDeviceFaultCountsEXT Counts{};
        Counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;

        VkResult Result = vkGetDeviceFaultInfoEXT(Device, &Counts, nullptr);
        if (Result != VK_SUCCESS && Result != VK_INCOMPLETE)
        {
            LOG_ERROR("[DeviceLost] vkGetDeviceFaultInfoEXT(counts) failed: 0x{:x}", (uint32)Result);
            return;
        }

        TVector<VkDeviceFaultAddressInfoEXT> AddressInfos(Counts.addressInfoCount);
        TVector<VkDeviceFaultVendorInfoEXT>  VendorInfos(Counts.vendorInfoCount);

        VkDeviceFaultInfoEXT Info{};
        Info.sType             = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        Info.pAddressInfos     = AddressInfos.empty() ? nullptr : AddressInfos.data();
        Info.pVendorInfos      = VendorInfos.empty()  ? nullptr : VendorInfos.data();
        Info.pVendorBinaryData = nullptr;
        Counts.vendorBinarySize = 0;

        Result = vkGetDeviceFaultInfoEXT(Device, &Counts, &Info);
        if (Result != VK_SUCCESS && Result != VK_INCOMPLETE)
        {
            LOG_ERROR("[DeviceLost] vkGetDeviceFaultInfoEXT(info) failed: 0x{:x}", (uint32)Result);
            return;
        }

        LOG_ERROR("[DeviceLost] Fault: {}", Info.description);
        for (uint32 i = 0; i < Counts.addressInfoCount; ++i)
        {
            const VkDeviceFaultAddressInfoEXT& A = AddressInfos[i];
            LOG_ERROR("[DeviceLost]   Address[{}] type={} reported=0x{:x} precision=0x{:x}",
                i, (uint32)A.addressType, (uint64)A.reportedAddress, (uint64)A.addressPrecision);
        }
        for (uint32 i = 0; i < Counts.vendorInfoCount; ++i)
        {
            const VkDeviceFaultVendorInfoEXT& V = VendorInfos[i];
            LOG_ERROR("[DeviceLost]   Vendor[{}] {} (code=0x{:x} data=0x{:x})",
                i, V.description, (uint64)V.vendorFaultCode, (uint64)V.vendorFaultData);
        }
    }

    void FVulkanCrashTracker::OnDeviceLost()
    {
        LogDeviceInfo();
        LogDeviceFaultInfo();

        #if WITH_AFTERMATH
        GFSDK_Aftermath_CrashDump_Status Status = GFSDK_Aftermath_CrashDump_Status_Unknown;
        AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GetCrashDumpStatus(&Status));

        auto TerminationTimeout = std::chrono::seconds(3);
        auto tStart = std::chrono::steady_clock::now();
        auto tElapsed = std::chrono::milliseconds::zero();

        while (Status != GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed && Status != GFSDK_Aftermath_CrashDump_Status_Finished && tElapsed < TerminationTimeout)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GetCrashDumpStatus(&Status));

            tElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart);
        }

        if (Status == GFSDK_Aftermath_CrashDump_Status_Finished)
        {
            LOG_INFO("Aftermath finished processing crash dump");
        }
        else
        {
            LOG_ERROR("Unexpected crash dump status: {}", static_cast<int>(Status));
        }
        #endif

        PANIC("Vulkan detected a crash");
    }

    void* FVulkanCrashTracker::GetDeviceCreatePNext()
    {
        #if WITH_AFTERMATH
        static VkDeviceDiagnosticsConfigCreateInfoNV DiagnosticsConfig = {};
        DiagnosticsConfig.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
        DiagnosticsConfig.flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV
                                | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV
                                | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
                                | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;

        return &DiagnosticsConfig;
        #else
        return nullptr;
        #endif
    }

    void FVulkanCrashTracker::GPUCrashDumpCallback(const void* GPUCrashDump, uint32 CrashDumpSize)
    {
        #if WITH_AFTERMATH
        LOG_ERROR("Aftermath: GPU crash dump received ({} bytes) - decoding...", CrashDumpSize);

        auto Now  = std::chrono::system_clock::now();
        auto Time = std::chrono::system_clock::to_time_t(Now);

        FString DumpPath = GetCrashDumpDirectory() + "/GPUCrash_" + eastl::to_string(static_cast<uint64>(Time)) + ".nv-gpudmp";
        FString JsonPath = GetCrashDumpDirectory() + "/GPUCrash_" + eastl::to_string(static_cast<uint64>(Time)) + ".json";

        {
            std::ofstream DumpFile(DumpPath.c_str(), std::ios::binary);
            if (DumpFile.is_open())
            {
                DumpFile.write(static_cast<const char*>(GPUCrashDump), CrashDumpSize);
                LOG_INFO("Aftermath: Raw dump written to '{}'", DumpPath);
                Platform::LaunchURL(UTF8_TO_TCHAR(JsonPath.c_str()));
            }
        }

        GFSDK_Aftermath_GpuCrashDump_Decoder Decoder = {};
        GFSDK_Aftermath_Result DecodeResult = GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
            GFSDK_Aftermath_Version_API,
            GPUCrashDump,
            CrashDumpSize,
            &Decoder);

        if (!GFSDK_Aftermath_SUCCEED(DecodeResult))
        {
            LOG_ERROR("Aftermath: Failed to create decoder");
            return;
        }

        const uint32 JsonDecoderFlags =
            GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO |
            GFSDK_Aftermath_GpuCrashDumpDecoderFlags_SHADER_INFO |
            GFSDK_Aftermath_GpuCrashDumpDecoderFlags_WARP_STATE_INFO |
            GFSDK_Aftermath_GpuCrashDumpDecoderFlags_SHADER_MAPPING_INFO;

        uint32 JsonSize = 0;
        GFSDK_Aftermath_Result JsonResult = GFSDK_Aftermath_GpuCrashDump_GenerateJSON(Decoder,
            JsonDecoderFlags,
            GFSDK_Aftermath_GpuCrashDumpFormatterFlags_NONE,
            ShaderDebugInfoLookupCallback,
            ShaderLookupCallback,
            ShaderSourceDebugInfoLookupCallback,
            this,
            &JsonSize);

        if (GFSDK_Aftermath_SUCCEED(JsonResult) && JsonSize > 0)
        {
            TVector<char> Json(JsonSize);
            GFSDK_Aftermath_GpuCrashDump_GetJSON(Decoder, JsonSize, Json.data());

            std::ofstream JsonFile(JsonPath.c_str());
            if (JsonFile.is_open())
            {
                JsonFile.write(Json.data(), JsonSize);
                LOG_INFO("Aftermath: Full JSON dump written to '{}' - open this for complete crash details", JsonPath);
            }
        }
        else
        {
            LOG_ERROR("Aftermath: Failed to generate JSON from crash dump (0x{:x})", static_cast<uint32>(JsonResult));
        }

        GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(Decoder);
        #endif
    }

    void FVulkanCrashTracker::OnShaderDebugInfo(const void* ShaderDebugInfo, const uint32 ShaderDebugInfoSize)
    {
        #if WITH_AFTERMATH
        GFSDK_Aftermath_ShaderDebugInfoIdentifier Identifier = {};
        AFTERMATH_CHECK_ERROR(GFSDK_Aftermath_GetShaderDebugInfoIdentifier(GFSDK_Aftermath_Version_API, ShaderDebugInfo, ShaderDebugInfoSize, &Identifier));

        TVector<uint8> Data((const uint8*)ShaderDebugInfo, (const uint8*)ShaderDebugInfo + ShaderDebugInfoSize);

        {
            FWriteScopeLock Lock(ShaderDebugInfoMutex);
            ShaderDebugInfos[IdentifierKey(Identifier)] = Move(Data);
        }

        // Persist for Nsight consumption.
        FString FilePath = GetCrashDumpDirectory() + "/Shader" + ToString(Identifier) + ".nvdbg";
        std::ofstream F(FilePath.c_str(), std::ios::out | std::ios::binary);
        if (F)
        {
            F.write((const char*)ShaderDebugInfo, ShaderDebugInfoSize);
        }
        #endif
    }

    void FVulkanCrashTracker::RegisterShader(const TVector<uint32>& SPIRV, const FString& Name)
    {
        #if WITH_AFTERMATH
        if (SPIRV.empty())
        {
            return;
        }

        GFSDK_Aftermath_SpirvCode SpirvCode = {};
        SpirvCode.pData = SPIRV.data();
        SpirvCode.size  = static_cast<uint32>(SPIRV.size() * sizeof(uint32));

        // BinaryHash is the key Aftermath uses to look up the shader during dump decoding.
        GFSDK_Aftermath_ShaderBinaryHash BinaryHash = {};
        GFSDK_Aftermath_Result HashResult = GFSDK_Aftermath_GetShaderHashSpirv(
            GFSDK_Aftermath_Version_API,
            &SpirvCode,
            &BinaryHash);

        if (!GFSDK_Aftermath_SUCCEED(HashResult))
        {
            LOG_WARN("Aftermath: Failed to compute shader hash for '{}' (0x{:x})", Name, static_cast<uint32>(HashResult));
            return;
        }

        // Same blob twice, we don't strip; deriving a debug name needs both halves.
        GFSDK_Aftermath_ShaderDebugName DebugName = {};
        GFSDK_Aftermath_Result DebugNameResult = GFSDK_Aftermath_GetShaderDebugNameSpirv(
            GFSDK_Aftermath_Version_API,
            &SpirvCode,
            &SpirvCode,
            &DebugName);

        FRegisteredShader Entry;
        Entry.Binary.assign(
            reinterpret_cast<const uint8*>(SPIRV.data()),
            reinterpret_cast<const uint8*>(SPIRV.data()) + SPIRV.size() * sizeof(uint32));
        Entry.FriendlyName = Name;

        if (GFSDK_Aftermath_SUCCEED(DebugNameResult))
        {
            Entry.DebugName = DebugName.name;
        }

        {
            FWriteScopeLock Lock(ShaderRegistryMutex);

            if (!Entry.DebugName.empty())
            {
                DebugNameToHash[Entry.DebugName] = BinaryHash.hash;
            }
            RegisteredShaders[BinaryHash.hash] = Move(Entry);
        }
        #endif
    }

#if WITH_AFTERMATH

    void FVulkanCrashTracker::OnShaderDebugInfoLookup(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& Identifier, PFN_GFSDK_Aftermath_SetData SetShaderDebugInfo) const
    {
        FReadScopeLock Lock(ShaderDebugInfoMutex);

        auto It = ShaderDebugInfos.find(IdentifierKey(Identifier));
        if (It == ShaderDebugInfos.end())
        {
            LOG_WARN("Aftermath: No shader debug info found for identifier {}", ToString(Identifier));
            return;
        }

        SetShaderDebugInfo(It->second.data(), static_cast<uint32>(It->second.size()));
    }

    void FVulkanCrashTracker::OnShaderLookup(const GFSDK_Aftermath_ShaderBinaryHash& ShaderHash, PFN_GFSDK_Aftermath_SetData SetShaderBinary) const
    {
        FReadScopeLock Lock(ShaderRegistryMutex);

        auto It = RegisteredShaders.find(ShaderHash.hash);
        if (It == RegisteredShaders.end())
        {
            LOG_WARN("Aftermath: No shader binary registered for hash {}", ToString(ShaderHash));
            return;
        }

        SetShaderBinary(It->second.Binary.data(), static_cast<uint32>(It->second.Binary.size()));
    }

    void FVulkanCrashTracker::OnShaderSourceDebugInfoLookup(const GFSDK_Aftermath_ShaderDebugName& DebugName, PFN_GFSDK_Aftermath_SetData SetShaderBinary) const
    {
        FReadScopeLock Lock(ShaderRegistryMutex);

        FString Key = DebugName.name;
        auto HashIt = DebugNameToHash.find(Key);
        if (HashIt == DebugNameToHash.end())
        {
            LOG_WARN("Aftermath: No source debug data for shader DebugName '{}'", Key);
            return;
        }

        auto ShaderIt = RegisteredShaders.find(HashIt->second);
        if (ShaderIt == RegisteredShaders.end())
        {
            return;
        }

        // Full binary doubles as source debug data (no stripping).
        SetShaderBinary(ShaderIt->second.Binary.data(), static_cast<uint32>(ShaderIt->second.Binary.size()));
    }
#endif

    const void* FVulkanCrashTracker::StoreMarker(const char* MarkerName)
    {
        FWriteScopeLock Lock(MarkerMutex);
        MarkerStorage.emplace_back(MarkerName ? MarkerName : "");
        return &MarkerStorage.back();
    }

    void FVulkanCrashTracker::SetMarker(RHICommandBuffer CmdBuffer, const char* MarkerName)
    {
        #if WITH_AFTERMATH
        if (CmdBuffer == nullptr || vkCmdSetCheckpointNV == nullptr)
        {
            return;
        }

        const void* Marker = StoreMarker(MarkerName);
        vkCmdSetCheckpointNV(static_cast<VkCommandBuffer>(CmdBuffer), Marker);
        #endif
    }

    void FVulkanCrashTracker::BeginMarker(RHICommandBuffer CmdBuffer, const char* MarkerName)
    {
        // NV checkpoints are flat; bracket via begin/end strings instead of push/pop.
        #if WITH_AFTERMATH
        if (CmdBuffer == nullptr || vkCmdSetCheckpointNV == nullptr)
        {
            return;
        }

        FString BeginName = FString("[Begin] ") + (MarkerName ? MarkerName : "");
        const void* Marker = StoreMarker(BeginName.c_str());
        vkCmdSetCheckpointNV(static_cast<VkCommandBuffer>(CmdBuffer), Marker);
        #endif
    }

    void FVulkanCrashTracker::EndMarker(RHICommandBuffer CmdBuffer)
    {
        #if WITH_AFTERMATH
        if (CmdBuffer == nullptr || vkCmdSetCheckpointNV == nullptr)
        {
            return;
        }

        const void* Marker = StoreMarker("[End]");
        vkCmdSetCheckpointNV(static_cast<VkCommandBuffer>(CmdBuffer), Marker);
        #endif
    }

    void FVulkanCrashTracker::PollCrashDumps()
    {
        #if WITH_AFTERMATH
        if (!bInitialized)
        {
            return;
        }

        GFSDK_Aftermath_CrashDump_Status Status = GFSDK_Aftermath_CrashDump_Status_Unknown;
        GFSDK_Aftermath_Result Result = GFSDK_Aftermath_GetCrashDumpStatus(&Status);
        if (!GFSDK_Aftermath_SUCCEED(Result))
        {
            return;
        }

        if (Status == GFSDK_Aftermath_CrashDump_Status_CollectingData ||
            Status == GFSDK_Aftermath_CrashDump_Status_InvokingCallback)
        {
            LOG_WARN("Aftermath: GPU crash in progress (status {})", static_cast<int>(Status));
        }
        #endif
    }
}
