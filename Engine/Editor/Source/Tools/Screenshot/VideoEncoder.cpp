#include "EditorPCH.h"
#include "VideoEncoder.h"

#include "Containers/StringFormat.h"

#if defined(LE_PLATFORM_WINDOWS)
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace Lumina
{
    FVideoEncoder::~FVideoEncoder()
    {
        FString Ignored;
        Close(Ignored);
    }

#if defined(LE_PLATFORM_WINDOWS)
    namespace
    {
        // Media Foundation counts time in hundreds of nanoseconds.
        constexpr LONGLONG TicksPerSecond = 10000000;

        std::wstring Widen(const FString& Text)
        {
            const int Length = MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), (int)Text.size(), nullptr, 0);
            std::wstring Wide((size_t)Length, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), (int)Text.size(), Wide.data(), Length);
            return Wide;
        }

        template<typename T>
        void SafeRelease(T*& Pointer)
        {
            if (Pointer != nullptr)
            {
                Pointer->Release();
                Pointer = nullptr;
            }
        }

        bool Check(HRESULT Result, const char* Step, FString& OutError)
        {
            if (SUCCEEDED(Result))
            {
                return true;
            }
            OutError = Lumina::Format("Media Foundation failed to {} (0x{:08X}).", Step, (uint32)Result);
            return false;
        }

        HRESULT DescribeVideo(IMFMediaType* Type, const GUID& Subtype, uint32 Width, uint32 Height, uint32 FrameRate)
        {
            HRESULT Result = Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(Result)) { Result = Type->SetGUID(MF_MT_SUBTYPE, Subtype); }
            if (SUCCEEDED(Result)) { Result = Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); }
            if (SUCCEEDED(Result)) { Result = MFSetAttributeSize(Type, MF_MT_FRAME_SIZE, Width, Height); }
            if (SUCCEEDED(Result)) { Result = MFSetAttributeRatio(Type, MF_MT_FRAME_RATE, FrameRate, 1); }
            if (SUCCEEDED(Result)) { Result = MFSetAttributeRatio(Type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1); }
            return Result;
        }

        constexpr uint32 AudioBitsPerSample = 16;
        constexpr uint32 AacBytesPerSecond = 24000;

        HRESULT DescribeAudio(IMFMediaType* Type, const GUID& Subtype, uint32 SampleRate, uint32 Channels)
        {
            HRESULT Result = Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(Result)) { Result = Type->SetGUID(MF_MT_SUBTYPE, Subtype); }
            if (SUCCEEDED(Result)) { Result = Type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AudioBitsPerSample); }
            if (SUCCEEDED(Result)) { Result = Type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, SampleRate); }
            if (SUCCEEDED(Result)) { Result = Type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, Channels); }
            return Result;
        }
    }

    bool FVideoEncoder::Open(const FString& Path, uint32 InWidth, uint32 InHeight, uint32 InFrameRate, uint32 BitRate, FString& OutError,
                             uint32 InAudioSampleRate, uint32 InAudioChannels)
    {
        if (Writer != nullptr)
        {
            OutError = "The encoder is already writing a file.";
            return false;
        }
        if (InWidth == 0 || InHeight == 0 || (InWidth & 1u) != 0 || (InHeight & 1u) != 0)
        {
            OutError = Lumina::Format("{}x{} cannot be encoded; H.264 needs an even width and height.", InWidth, InHeight);
            return false;
        }

        Width = InWidth;
        Height = InHeight;
        FrameRate = InFrameRate > 0 ? InFrameRate : 30;
        FrameIndex = 0;
        AudioFramesWritten = 0;

        // The encoder Windows ships takes only these rates, and more than two channels are folded down to stereo.
        const bool bAudio = InAudioChannels > 0 && (InAudioSampleRate == 44100 || InAudioSampleRate == 48000);
        if (InAudioSampleRate != 0 && !bAudio)
        {
            OutError = Lumina::Format("Sound at {} Hz cannot be stored as AAC, which needs 44100 or 48000.", InAudioSampleRate);
            return false;
        }
        AudioSampleRate = bAudio ? InAudioSampleRate : 0;
        AudioSourceChannels = InAudioChannels;
        AudioChannels = Math::Min(InAudioChannels, 2u);

        const HRESULT Com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(Com) && Com != RPC_E_CHANGED_MODE)
        {
            return Check(Com, "start COM", OutError);
        }
        if (!Check(MFStartup(MF_VERSION), "start up", OutError))
        {
            return false;
        }
        bStartedMediaFoundation = true;

        IMFAttributes* Attributes = nullptr;
        IMFSinkWriter* SinkWriter = nullptr;
        IMFMediaType* OutputType = nullptr;
        IMFMediaType* InputType = nullptr;
        DWORD Stream = 0;

        bool bOk = Check(MFCreateAttributes(&Attributes, 2), "create writer attributes", OutError)
            && Check(Attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "enable hardware encoding", OutError)
            && Check(Attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "disable throttling", OutError)
            && Check(MFCreateSinkWriterFromURL(Widen(Path).c_str(), nullptr, Attributes, &SinkWriter), "create the output file", OutError);

        bOk = bOk
            && Check(MFCreateMediaType(&OutputType), "create the output type", OutError)
            && Check(DescribeVideo(OutputType, MFVideoFormat_H264, Width, Height, FrameRate), "describe the output", OutError)
            && Check(OutputType->SetUINT32(MF_MT_AVG_BITRATE, BitRate), "set the bitrate", OutError)
            && Check(OutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High), "set the profile", OutError)
            && Check(SinkWriter->AddStream(OutputType, &Stream), "add the video stream", OutError);

        // Top row first, which is the order the readback arrives in.
        bOk = bOk
            && Check(MFCreateMediaType(&InputType), "create the input type", OutError)
            && Check(DescribeVideo(InputType, MFVideoFormat_RGB32, Width, Height, FrameRate), "describe the input", OutError)
            && Check(InputType->SetUINT32(MF_MT_DEFAULT_STRIDE, Width * 4u), "set the input stride", OutError)
            && Check(SinkWriter->SetInputMediaType(Stream, InputType, nullptr), "accept RGB frames", OutError);

        IMFMediaType* AudioOutput = nullptr;
        IMFMediaType* AudioInput = nullptr;
        DWORD AudioStream = 0;
        if (bOk && AudioSampleRate != 0)
        {
            const uint32 BlockAlign = AudioChannels * AudioBitsPerSample / 8u;
            bOk = Check(MFCreateMediaType(&AudioOutput), "create the sound output type", OutError)
                && Check(DescribeAudio(AudioOutput, MFAudioFormat_AAC, AudioSampleRate, AudioChannels), "describe the sound output", OutError)
                && Check(AudioOutput->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AacBytesPerSecond), "set the sound bitrate", OutError)
                && Check(SinkWriter->AddStream(AudioOutput, &AudioStream), "add the sound stream", OutError)
                && Check(MFCreateMediaType(&AudioInput), "create the sound input type", OutError)
                && Check(DescribeAudio(AudioInput, MFAudioFormat_PCM, AudioSampleRate, AudioChannels), "describe the sound input", OutError)
                && Check(AudioInput->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, BlockAlign), "set the sound block size", OutError)
                && Check(AudioInput->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, BlockAlign * AudioSampleRate), "set the sound rate", OutError)
                && Check(SinkWriter->SetInputMediaType(AudioStream, AudioInput, nullptr), "accept PCM sound", OutError);
        }

        bOk = bOk && Check(SinkWriter->BeginWriting(), "begin writing", OutError);

        SafeRelease(Attributes);
        SafeRelease(OutputType);
        SafeRelease(InputType);
        SafeRelease(AudioOutput);
        SafeRelease(AudioInput);

        if (!bOk)
        {
            SafeRelease(SinkWriter);
            MFShutdown();
            bStartedMediaFoundation = false;
            return false;
        }

        Writer = SinkWriter;
        StreamIndex = Stream;
        AudioStreamIndex = AudioStream;
        return true;
    }

    bool FVideoEncoder::WriteAudio(const float* Interleaved, uint32 FrameCount, FString& OutError)
    {
        if (Writer == nullptr || AudioSampleRate == 0 || Interleaved == nullptr || FrameCount == 0)
        {
            return true;
        }

        const DWORD Bytes = FrameCount * AudioChannels * sizeof(int16);
        IMFMediaBuffer* Buffer = nullptr;
        IMFSample* Sample = nullptr;
        if (!Check(MFCreateMemoryBuffer(Bytes, &Buffer), "allocate sound", OutError))
        {
            return false;
        }

        BYTE* Data = nullptr;
        bool bOk = Check(Buffer->Lock(&Data, nullptr, nullptr), "lock sound", OutError);
        if (bOk)
        {
            int16* Out = reinterpret_cast<int16*>(Data);
            for (uint32 Frame = 0; Frame < FrameCount; ++Frame)
            {
                for (uint32 Channel = 0; Channel < AudioChannels; ++Channel)
                {
                    const float Value = Math::Clamp(Interleaved[(size_t)Frame * AudioSourceChannels + Channel], -1.0f, 1.0f);
                    Out[(size_t)Frame * AudioChannels + Channel] = (int16)(Value * 32767.0f);
                }
            }
            Buffer->Unlock();
        }

        const LONGLONG Start = (LONGLONG)(AudioFramesWritten * TicksPerSecond / AudioSampleRate);
        const LONGLONG End = (LONGLONG)((AudioFramesWritten + FrameCount) * TicksPerSecond / AudioSampleRate);
        bOk = bOk
            && Check(Buffer->SetCurrentLength(Bytes), "size sound", OutError)
            && Check(MFCreateSample(&Sample), "create a sound sample", OutError)
            && Check(Sample->AddBuffer(Buffer), "attach sound", OutError)
            && Check(Sample->SetSampleTime(Start), "time sound", OutError)
            && Check(Sample->SetSampleDuration(End - Start), "time sound", OutError)
            && Check(static_cast<IMFSinkWriter*>(Writer)->WriteSample(AudioStreamIndex, Sample), "encode sound", OutError);

        SafeRelease(Sample);
        SafeRelease(Buffer);
        if (bOk)
        {
            AudioFramesWritten += FrameCount;
        }
        return bOk;
    }

    bool FVideoEncoder::WriteFrame(const uint8* RGBA, FString& OutError)
    {
        if (Writer == nullptr || RGBA == nullptr)
        {
            OutError = "The encoder is not open.";
            return false;
        }

        const DWORD Bytes = Width * Height * 4u;
        IMFMediaBuffer* Buffer = nullptr;
        IMFSample* Sample = nullptr;
        if (!Check(MFCreateMemoryBuffer(Bytes, &Buffer), "allocate a frame", OutError))
        {
            return false;
        }

        BYTE* Data = nullptr;
        bool bOk = Check(Buffer->Lock(&Data, nullptr, nullptr), "lock a frame", OutError);
        if (bOk)
        {
            // RGB32 is laid out blue, green, red in memory.
            for (DWORD Pixel = 0; Pixel < Width * Height; ++Pixel)
            {
                Data[Pixel * 4 + 0] = RGBA[Pixel * 4 + 2];
                Data[Pixel * 4 + 1] = RGBA[Pixel * 4 + 1];
                Data[Pixel * 4 + 2] = RGBA[Pixel * 4 + 0];
                Data[Pixel * 4 + 3] = 255;
            }
            Buffer->Unlock();
        }

        const LONGLONG Duration = TicksPerSecond / FrameRate;
        bOk = bOk
            && Check(Buffer->SetCurrentLength(Bytes), "size a frame", OutError)
            && Check(MFCreateSample(&Sample), "create a sample", OutError)
            && Check(Sample->AddBuffer(Buffer), "attach a frame", OutError)
            && Check(Sample->SetSampleTime((LONGLONG)FrameIndex * Duration), "time a frame", OutError)
            && Check(Sample->SetSampleDuration(Duration), "time a frame", OutError)
            && Check(static_cast<IMFSinkWriter*>(Writer)->WriteSample(StreamIndex, Sample), "encode a frame", OutError);

        SafeRelease(Sample);
        SafeRelease(Buffer);
        if (bOk)
        {
            ++FrameIndex;
        }
        return bOk;
    }

    bool FVideoEncoder::Close(FString& OutError)
    {
        bool bOk = true;
        if (Writer != nullptr)
        {
            IMFSinkWriter* SinkWriter = static_cast<IMFSinkWriter*>(Writer);
            bOk = Check(SinkWriter->Finalize(), "finish the file", OutError);
            SafeRelease(SinkWriter);
            Writer = nullptr;
        }
        AudioSampleRate = 0;
        if (bStartedMediaFoundation)
        {
            MFShutdown();
            bStartedMediaFoundation = false;
        }
        return bOk;
    }
#else
    bool FVideoEncoder::Open(const FString& Path, uint32 InWidth, uint32 InHeight, uint32 InFrameRate, uint32 BitRate, FString& OutError,
                             uint32 InAudioSampleRate, uint32 InAudioChannels)
    {
        OutError = "Video encoding is only available on Windows; write a PNG sequence instead.";
        return false;
    }

    bool FVideoEncoder::WriteAudio(const float* Interleaved, uint32 FrameCount, FString& OutError)
    {
        return true;
    }

    bool FVideoEncoder::WriteFrame(const uint8* RGBA, FString& OutError)
    {
        OutError = "The encoder is not open.";
        return false;
    }

    bool FVideoEncoder::Close(FString& OutError)
    {
        return true;
    }
#endif
}
