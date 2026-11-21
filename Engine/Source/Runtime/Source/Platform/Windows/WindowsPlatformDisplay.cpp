#include "RuntimePCH.h"
#ifdef _WIN32

#include "Platform/Display/PlatformDisplay.h"
#include "Containers/Vector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>

#pragma comment(lib, "dxgi.lib")

namespace Lumina::PlatformDisplay
{
    namespace
    {
        // DisplayConfig reports SDR white in thousandths of the 80 nit sRGB reference white.
        constexpr float kSDRWhiteLevelToNits = 80.0f / 1000.0f;

        template<typename T>
        void SafeRelease(T*& Interface)
        {
            if (Interface != nullptr)
            {
                Interface->Release();
                Interface = nullptr;
            }
        }

        float QuerySDRWhiteNits(HMONITOR Monitor)
        {
            MONITORINFOEXW MonitorInfo{};
            MonitorInfo.cbSize = sizeof(MonitorInfo);
            if (!GetMonitorInfoW(Monitor, &MonitorInfo))
            {
                return 0.0f;
            }

            UINT32 NumPaths = 0;
            UINT32 NumModes = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &NumPaths, &NumModes) != ERROR_SUCCESS)
            {
                return 0.0f;
            }

            TVector<DISPLAYCONFIG_PATH_INFO> DisplayPaths((int32)NumPaths);
            TVector<DISPLAYCONFIG_MODE_INFO> DisplayModes((int32)NumModes);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &NumPaths, DisplayPaths.GetData(), &NumModes, DisplayModes.GetData(), nullptr) != ERROR_SUCCESS)
            {
                return 0.0f;
            }

            for (UINT32 Index = 0; Index < NumPaths; ++Index)
            {
                const DISPLAYCONFIG_PATH_INFO& Path = DisplayPaths[(int32)Index];

                DISPLAYCONFIG_SOURCE_DEVICE_NAME Source{};
                Source.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                Source.header.size      = sizeof(Source);
                Source.header.adapterId = Path.sourceInfo.adapterId;
                Source.header.id        = Path.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&Source.header) != ERROR_SUCCESS || wcscmp(Source.viewGdiDeviceName, MonitorInfo.szDevice) != 0)
                {
                    continue;
                }

                DISPLAYCONFIG_SDR_WHITE_LEVEL White{};
                White.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                White.header.size      = sizeof(White);
                White.header.adapterId = Path.targetInfo.adapterId;
                White.header.id        = Path.targetInfo.id;
                return DisplayConfigGetDeviceInfo(&White.header) == ERROR_SUCCESS ? (float)White.SDRWhiteLevel * kSDRWhiteLevelToNits : 0.0f;
            }
            return 0.0f;
        }

        // A fresh factory each call, because a cached one keeps reporting the color space it saw when made.
        bool QueryOutputDesc(HMONITOR Monitor, DXGI_OUTPUT_DESC1& OutDesc)
        {
            IDXGIFactory1* Factory = nullptr;
            if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&Factory))))
            {
                return false;
            }

            bool bFound = false;
            IDXGIAdapter1* Adapter = nullptr;
            for (UINT AdapterIndex = 0; !bFound && Factory->EnumAdapters1(AdapterIndex, &Adapter) != DXGI_ERROR_NOT_FOUND; ++AdapterIndex)
            {
                IDXGIOutput* Output = nullptr;
                for (UINT OutputIndex = 0; !bFound && Adapter->EnumOutputs(OutputIndex, &Output) != DXGI_ERROR_NOT_FOUND; ++OutputIndex)
                {
                    DXGI_OUTPUT_DESC Desc{};
                    IDXGIOutput6* Output6 = nullptr;
                    if (SUCCEEDED(Output->GetDesc(&Desc)) && Desc.Monitor == Monitor
                        && SUCCEEDED(Output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&Output6))))
                    {
                        bFound = SUCCEEDED(Output6->GetDesc1(&OutDesc));
                    }
                    SafeRelease(Output6);
                    SafeRelease(Output);
                }
                SafeRelease(Adapter);
            }
            SafeRelease(Factory);
            return bFound;
        }
    }

    FHDRDisplayInfo QueryHDRDisplay(int32 ScreenX, int32 ScreenY)
    {
        FHDRDisplayInfo Info;
        const HMONITOR Monitor = MonitorFromPoint(POINT{ ScreenX, ScreenY }, MONITOR_DEFAULTTONEAREST);
        if (Monitor == nullptr)
        {
            return Info;
        }

        DXGI_OUTPUT_DESC1 Desc{};
        if (QueryOutputDesc(Monitor, Desc))
        {
            Info.bHDREnabled = Desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            Info.PeakNits    = Desc.MaxLuminance;
        }
        Info.SDRWhiteNits = QuerySDRWhiteNits(Monitor);
        return Info;
    }
}

#endif
