#include <gtest/gtest.h>

#if defined(LE_PLATFORM_WINDOWS)
#include <cmath>
#include <thread>
#include <Windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Tools/Screenshot/VideoEncoder.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace Lumina;

namespace
{
    struct FEncoderOutputFolder
    {
        FString Root = FString(*Paths::Combine(Filesystem::GetTempDirectory(), "LuminaVideoEncoderTests"));

        FEncoderOutputFolder()
        {
            Filesystem::RemoveTree(Root);
            Filesystem::MakeDirectoryTree(Root);
        }

        ~FEncoderOutputFolder() { Filesystem::RemoveTree(Root); }
    };
}

// MovieCapture opens and closes one encoder per recording on the same thread.
TEST(VideoEncoder, RepeatedOpenCloseBalancesComOnTheSameThread)
{
    FEncoderOutputFolder Folder;
    std::thread Recorder([&Folder]
    {
        FVideoEncoder Encoder;
        FString Error;
        for (uint32 Pass = 0; Pass < 2; ++Pass)
        {
            const FString Path = FString(*Paths::Combine(Folder.Root, Lumina::Format("Pass{}.mp4", Pass)));
            ASSERT_TRUE(Encoder.Open(Path, 64, 64, 30, 2'000'000, Error)) << Error;

            TVector<uint8> Frame;
            Frame.SetNum(64 * 64 * 4, 0);
            EXPECT_TRUE(Encoder.WriteFrame(Frame.GetData(), Error)) << Error;
            EXPECT_TRUE(Encoder.Close(Error)) << Error;
            EXPECT_FALSE(Encoder.IsOpen());
        }

        // A leaked multithreaded init would refuse a single-threaded apartment here.
        const HRESULT Com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        EXPECT_EQ(Com, S_OK);
        if (SUCCEEDED(Com))
        {
            CoUninitialize();
        }
    });
    Recorder.join();
}

namespace
{
    std::wstring WidenPath(const FString& Path)
    {
        const int Length = MultiByteToWideChar(CP_UTF8, 0, *Path, Path.Len(), nullptr, 0);
        std::wstring Wide((size_t)Length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, *Path, Path.Len(), Wide.data(), Length);
        return Wide;
    }

    // Starts its own Media Foundation session since the encoder's Close() shuts that down process-wide.
    int16 DecodeLoudestSample(const FString& Path)
    {
        const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool bInitializedCom = SUCCEEDED(Com);
        if (FAILED(Com) && Com != RPC_E_CHANGED_MODE)
        {
            return 0;
        }
        if (FAILED(MFStartup(MF_VERSION)))
        {
            if (bInitializedCom) { CoUninitialize(); }
            return 0;
        }

        IMFSourceReader* Reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(WidenPath(Path).c_str(), nullptr, &Reader)))
        {
            MFShutdown();
            if (bInitializedCom) { CoUninitialize(); }
            return 0;
        }

        IMFMediaType* PcmType = nullptr;
        MFCreateMediaType(&PcmType);
        PcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        PcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        Reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, PcmType);
        PcmType->Release();

        int16 Loudest = 0;
        for (;;)
        {
            DWORD Flags = 0;
            IMFSample* Sample = nullptr;
            if (FAILED(Reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &Flags, nullptr, &Sample)))
            {
                break;
            }
            if ((Flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || Sample == nullptr)
            {
                if (Sample != nullptr) { Sample->Release(); }
                break;
            }

            IMFMediaBuffer* Buffer = nullptr;
            if (SUCCEEDED(Sample->ConvertToContiguousBuffer(&Buffer)))
            {
                BYTE* Data = nullptr;
                DWORD Length = 0;
                if (SUCCEEDED(Buffer->Lock(&Data, nullptr, &Length)))
                {
                    const int16* Samples = reinterpret_cast<const int16*>(Data);
                    for (DWORD Index = 0; Index < Length / sizeof(int16); ++Index)
                    {
                        Loudest = Math::Max(Loudest, (int16)std::abs((int)Samples[Index]));
                    }
                    Buffer->Unlock();
                }
                Buffer->Release();
            }
            Sample->Release();
        }

        Reader->Release();
        MFShutdown();
        if (bInitializedCom) { CoUninitialize(); }
        return Loudest;
    }
}

// Channels beyond stereo must still reach the AAC track instead of being dropped.
TEST(VideoEncoder, SurroundChannelsAreDownmixedIntoTheStereoTrack)
{
    FEncoderOutputFolder Folder;
    std::thread Recorder([&Folder]
    {
        const FString Path = FString(*Paths::Combine(Folder.Root, "Surround.mp4"));
        FVideoEncoder Encoder;
        FString Error;
        ASSERT_TRUE(Encoder.Open(Path, 64, 64, 30, 2'000'000, Error, 48000, 6)) << Error;

        TVector<uint8> Frame;
        Frame.SetNum(64 * 64 * 4, 0);
        EXPECT_TRUE(Encoder.WriteFrame(Frame.GetData(), Error)) << Error;

        constexpr uint32 FrameCount = 4800;
        constexpr uint32 SourceChannels = 6;
        constexpr uint32 CenterChannel = 2;
        TVector<float> Interleaved;
        Interleaved.SetNum(FrameCount * SourceChannels, 0.0f);
        for (uint32 Frame = 0; Frame < FrameCount; ++Frame)
        {
            Interleaved[Frame * SourceChannels + CenterChannel] = std::sin((float)Frame * 0.2f) * 0.8f;
        }
        EXPECT_TRUE(Encoder.WriteAudio(Interleaved.GetData(), FrameCount, Error)) << Error;
        EXPECT_TRUE(Encoder.Close(Error)) << Error;
    });
    Recorder.join();

    const FString Path = FString(*Paths::Combine(Folder.Root, "Surround.mp4"));
    EXPECT_GT(DecodeLoudestSample(Path), (int16)1000);
}
#endif
