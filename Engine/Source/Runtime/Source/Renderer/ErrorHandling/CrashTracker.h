#pragma once
#include "Containers/Array.h"
#include "Containers/String.h"

namespace Lumina::RHI
{
    using RHIDevice = void*;
    using RHIPhysicalDevice = void*;
    using RHICommandBuffer = void*;
    
    class ICrashTracker
    {
    public:
        virtual ~ICrashTracker() = default;
    
        virtual void Initialize(RHIDevice device, RHIPhysicalDevice physicalDevice) = 0;
        virtual void Shutdown() = 0;

        virtual void OnDeviceLost() = 0;
    
        virtual void RegisterShader(const TVector<uint32>& SPRIV, const FString& Name) = 0;
        
        virtual void GPUCrashDumpCallback(const void* GPUCrashDump, uint32 CrashDumpSize) = 0;
        
        virtual void OnShaderDebugInfo(const void* ShaderDebugInfo, uint32 ShaderDebugInfoSize) = 0;
    
        virtual void SetMarker(RHICommandBuffer cmdBuffer, const char* markerName) = 0;
        virtual void BeginMarker(RHICommandBuffer cmdBuffer, const char* markerName) = 0;
        virtual void EndMarker(RHICommandBuffer cmdBuffer) = 0;
    
        virtual void PollCrashDumps() = 0;
    
    };
}
