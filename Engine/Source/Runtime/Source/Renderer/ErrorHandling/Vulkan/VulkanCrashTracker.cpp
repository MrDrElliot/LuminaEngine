#include "RuntimePCH.h"
#include "VulkanCrashTracker.h"
#include <filesystem>
#include <fstream>
#include <volk/volk.h>
#if WITH_AFTERMATH
// MessageBoxA in the Aftermath error macro; nothing else pulls windows.h into this TU.
#include <windows.h>
#include "NvidiaAftermath/GFSDK_Aftermath.h"
#include "NvidiaAftermath/GFSDK_Aftermath_GpuCrashDumpDecoding.h"
#include <NvidiaAftermath/GFSDK_Aftermath_GpuCrashDump.h>
#endif
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/CrashHandler.h"
#include "Platform/CrashReporter.h"
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

    FString FVulkanCrashTracker::GetCrashDumpDirectory() const
    {
        FString Directory = CrashHandler::GetCrashDumpDirectory();

        std::error_code Ec;
        std::filesystem::create_directories(Directory.c_str(), Ec);

        return Directory;
    }

    FVulkanCrashTracker::FVulkanCrashTracker()
    {
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

        #if WITH_RGD
        if (IsAmdDevice())
        {
            LOG_INFO("Radeon GPU Detective support active: debug-utils object names and markers are enabled. "
                     "Arm Crash Analysis in Radeon Developer Panel to capture a .rgd dump on a GPU crash.");
        }
        #endif
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

    FString FVulkanCrashTracker::LogDeviceInfo() const
    {
        if (PhysicalDevice == VK_NULL_HANDLE)
        {
            return {};
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

        // Attributes rather than log-only: a GPU crash is almost always a driver-version or
        // specific-model story, and those want to be sortable columns in the dashboard rather than
        // something to be dug out of an attached log per report.
        CrashReporting::SetAttribute("GPU", Props2.properties.deviceName);
        CrashReporting::SetAttribute("GPUDriver", DriverProps.driverInfo);

        FString Summary = Props2.properties.deviceName;
        Summary += " (";
        Summary += DriverProps.driverInfo;
        Summary += ")";
        return Summary;
    }

    static const char* FaultAddressTypeToString(VkDeviceFaultAddressTypeEXT Type)
    {
        switch (Type)
        {
        case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:                         return "None";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:                 return "Invalid read";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:                return "Invalid write";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:              return "Invalid execute";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:  return "Instruction pointer unknown";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:  return "Instruction pointer invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:    return "Instruction pointer faulted";
        default:                                                            return "Unknown";
        }
    }

    FString FVulkanCrashTracker::LogDeviceFaultInfo() const
    {
        if (!bDeviceFaultEnabled || Device == VK_NULL_HANDLE || vkGetDeviceFaultInfoEXT == nullptr)
        {
            // Worth saying out loud: without this extension an AMD or Intel crash reports nothing at
            // all, and a silent absence reads like "the GPU had no complaint".
            LOG_ERROR("[DeviceLost] VK_EXT_device_fault unavailable; no vendor fault detail for this crash.");
            return {};
        }

        VkDeviceFaultCountsEXT Counts{};
        Counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;

        VkResult Result = vkGetDeviceFaultInfoEXT(Device, &Counts, nullptr);
        if (Result != VK_SUCCESS && Result != VK_INCOMPLETE)
        {
            LOG_ERROR("[DeviceLost] vkGetDeviceFaultInfoEXT(counts) failed: 0x{:x}", (uint32)Result);
            return {};
        }

        TVector<VkDeviceFaultAddressInfoEXT> AddressInfos(Counts.addressInfoCount);
        TVector<VkDeviceFaultVendorInfoEXT>  VendorInfos(Counts.vendorInfoCount);

        // The vendor binary is the driver's own post-mortem blob, and on AMD it is by far the richest
        // thing available -- RGD parses it into a marker tree and page-fault resource list. It was
        // previously discarded here, which left an AMD crash with nothing but a description string.
        TVector<uint8> VendorBinary(static_cast<size_t>(Counts.vendorBinarySize));

        VkDeviceFaultInfoEXT Info{};
        Info.sType             = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        Info.pAddressInfos     = AddressInfos.empty() ? nullptr : AddressInfos.data();
        Info.pVendorInfos      = VendorInfos.empty()  ? nullptr : VendorInfos.data();
        Info.pVendorBinaryData = VendorBinary.empty() ? nullptr : VendorBinary.data();

        Result = vkGetDeviceFaultInfoEXT(Device, &Counts, &Info);
        if (Result != VK_SUCCESS && Result != VK_INCOMPLETE)
        {
            LOG_ERROR("[DeviceLost] vkGetDeviceFaultInfoEXT(info) failed: 0x{:x}", (uint32)Result);
            return {};
        }

        LOG_ERROR("[DeviceLost] Fault: {}", Info.description);

        for (uint32 i = 0; i < Counts.addressInfoCount; ++i)
        {
            const VkDeviceFaultAddressInfoEXT& A = AddressInfos[i];

            // Precision is a power-of-two mask: the faulting address is somewhere in
            // [reported & ~(precision-1), reported | (precision-1)]. Logged as that range because the
            // raw pair is routinely misread as an exact address.
            const uint64 Mask  = A.addressPrecision ? A.addressPrecision - 1 : 0;
            const uint64 Lower = static_cast<uint64>(A.reportedAddress) & ~Mask;
            const uint64 Upper = static_cast<uint64>(A.reportedAddress) | Mask;

            LOG_ERROR("[DeviceLost]   Address[{}] {} at 0x{:x} (range 0x{:x}-0x{:x}, precision 0x{:x})",
                i, FaultAddressTypeToString(A.addressType),
                (uint64)A.reportedAddress, Lower, Upper, (uint64)A.addressPrecision);
        }

        for (uint32 i = 0; i < Counts.vendorInfoCount; ++i)
        {
            const VkDeviceFaultVendorInfoEXT& V = VendorInfos[i];
            LOG_ERROR("[DeviceLost]   Vendor[{}] {} (code=0x{:x} data=0x{:x})",
                i, V.description, (uint64)V.vendorFaultCode, (uint64)V.vendorFaultData);
        }

        if (Counts.vendorBinarySize > 0)
        {
            const auto Time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            const FString BinaryPath = GetCrashDumpDirectory() + "/GPUFault_"
                + eastl::to_string(static_cast<uint64>(Time)) + ".bin";

            std::ofstream File(BinaryPath.c_str(), std::ios::binary);
            if (File.is_open())
            {
                File.write(reinterpret_cast<const char*>(VendorBinary.data()),
                    static_cast<std::streamsize>(Counts.vendorBinarySize));
                File.close();

                LOG_ERROR("[DeviceLost] Vendor crash binary ({} bytes): {}",
                    (uint64)Counts.vendorBinarySize, BinaryPath.c_str());

                // Goes up with the crash report; it is the only artifact that explains an AMD fault.
                CrashReporting::AddAttachment(BinaryPath);
            }
            else
            {
                LOG_ERROR("[DeviceLost] Could not write vendor crash binary to {}", BinaryPath.c_str());
            }
        }

        // First address info is the useful one for a one-line summary; description alone is often
        // just "GPU HANG" and says nothing about where.
        FString Reason = Info.description;
        if (Counts.addressInfoCount > 0)
        {
            Reason += " (";
            Reason += FaultAddressTypeToString(AddressInfos[0].addressType);
            Reason += ")";
        }

        return Reason;
    }

#if WITH_RGD
    bool FVulkanCrashTracker::IsAmdDevice() const
    {
        if (PhysicalDevice == VK_NULL_HANDLE)
        {
            return false;
        }

        VkPhysicalDeviceDriverProperties DriverProps{};
        DriverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        VkPhysicalDeviceProperties2 Props2{};
        Props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        Props2.pNext = &DriverProps;

        vkGetPhysicalDeviceProperties2(PhysicalDevice, &Props2);

        return DriverProps.driverID == VK_DRIVER_ID_AMD_PROPRIETARY
            || DriverProps.driverID == VK_DRIVER_ID_AMD_OPEN_SOURCE
            || DriverProps.driverID == VK_DRIVER_ID_MESA_RADV;
    }
    
    void FVulkanCrashTracker::LogRgdGuidance() const
    {
        LOG_ERROR("[DeviceLost] AMD device. To capture a full post-mortem, reproduce with Radeon Developer Panel:");
        LOG_ERROR("[DeviceLost]   1. Radeon Developer Panel -> Crash Analysis, then launch the editor from the panel.");
        LOG_ERROR("[DeviceLost]   2. Enable 'Hardware Crash Analysis' for shader-level attribution.");
        LOG_ERROR("[DeviceLost]   3. After the crash: rgd --parse <dump>.rgd -o crash.txt");
        LOG_ERROR("[DeviceLost] The report's marker tree and page-fault resource list use this build's debug-utils");
        LOG_ERROR("[DeviceLost] names, so anything passed to RHI::SetDebugName resolves by name rather than by address.");
    }
#endif

    void FVulkanCrashTracker::OnDeviceLost()
    {
        LogDeviceInfo();

        const FString Reason = LogDeviceFaultInfo();

        // Tagged even when empty, so a report with no vendor detail is visibly "we could not tell"
        // rather than indistinguishable from a CPU crash in the dashboard.
        CrashReporting::SetAttribute("DeviceLostReason", Reason.empty() ? FStringView("Unknown") : FStringView(Reason));

        #if WITH_RGD
        if (IsAmdDevice())
        {
            LogRgdGuidance();
        }
        #endif

        #if WITH_AFTERMATH
        // Deliberately NOT AFTERMATH_CHECK_ERROR here. That macro calls exit(1) on any failure, and
        // a status query failing is entirely likely when the device has just been lost -- which
        // would kill the process before the local dump, the log flush or the report. exit() raises
        // no signal, so nothing downstream would have caught it either. A failed query here just
        // means no NVIDIA data; the rest of the report still has to go out.
        GFSDK_Aftermath_CrashDump_Status Status = GFSDK_Aftermath_CrashDump_Status_Unknown;

        auto PollStatus = [&Status]() -> bool
        {
            const GFSDK_Aftermath_Result Result = GFSDK_Aftermath_GetCrashDumpStatus(&Status);
            if (!GFSDK_Aftermath_SUCCEED(Result))
            {
                LOG_ERROR("[DeviceLost] Aftermath status query failed ({}); continuing without NVIDIA data.",
                    AftermathErrorMessage(Result).c_str());
                return false;
            }
            return true;
        };

        bool bStatusValid = PollStatus();

        auto TerminationTimeout = std::chrono::seconds(3);
        auto tStart = std::chrono::steady_clock::now();
        auto tElapsed = std::chrono::milliseconds::zero();

        while (bStatusValid
            && Status != GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed
            && Status != GFSDK_Aftermath_CrashDump_Status_Finished
            && tElapsed < TerminationTimeout)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            bStatusValid = PollStatus();

            tElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart);
        }

        // LOG_ERROR, not LOG_INFO: this is the line that says whether NVIDIA data exists at all, and
        // LOG_INFO compiles to nothing in Shipping, where a user's crash is the only copy there is.
        switch (bStatusValid ? Status : GFSDK_Aftermath_CrashDump_Status_Unknown)
        {
        case GFSDK_Aftermath_CrashDump_Status_Finished:
            LOG_ERROR("[DeviceLost] Aftermath finished; GPU dump and decoded JSON are in {}",
                GetCrashDumpDirectory().c_str());
            break;

        case GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed:
            LOG_ERROR("[DeviceLost] Aftermath failed to collect crash data. Usually means the fault "
                      "took the whole device down before the dump could be assembled.");
            break;

        case GFSDK_Aftermath_CrashDump_Status_NotStarted:
            // Distinct from a timeout mid-collection: the driver never began a dump at all, so it
            // never saw a GPU fault. Either this was not a real device loss (a forced
            // HandleDeviceLost, or a CPU-side failure misreported as one), or the fault was outside
            // what Aftermath watches. Chasing a missing dump here is chasing the wrong thing.
            LOG_ERROR("[DeviceLost] Aftermath never started a dump: the driver did not report a GPU "
                      "fault. Not a real device loss, or the fault was outside Aftermath's scope.");
            break;

        default:
            // Collection was genuinely in progress and ran past the 3s budget. The dump may still
            // land on disk after this, but the process is about to go away.
            LOG_ERROR("[DeviceLost] Aftermath did not finish within the timeout (status {}); "
                      "the GPU dump may be missing or truncated.", static_cast<int>(Status));
            break;
        }
        #endif

        // Returns rather than panicking. This function's job is to gather evidence; HandleDeviceLost
        // owns how the process dies, and it has to run the reporter first. The PANIC that used to be
        // here aborted mid-function, which made everything after the call site unreachable.
        Logging::Flush();
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
                LOG_ERROR("[DeviceLost] Aftermath raw dump written to '{}'", DumpPath.c_str());
                
                CrashReporting::AddAttachment(DumpPath);
            }
            else
            {
                LOG_ERROR("[DeviceLost] Could not write Aftermath dump to '{}'", DumpPath.c_str());
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
                JsonFile.close();

                LOG_ERROR("[DeviceLost] Aftermath JSON written to '{}' - decoded fault, active warps "
                          "and shader attribution are in there", JsonPath.c_str());

                // The readable half of the pair, and the one that resolves to shader source when
                // debug info was uploaded. Attached so it arrives without asking the user for it.
                CrashReporting::AddAttachment(JsonPath);
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
