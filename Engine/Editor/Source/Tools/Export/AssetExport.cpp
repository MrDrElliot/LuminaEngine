#include "EditorPCH.h"
#include "AssetExport.h"

#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Assets/AssetTypes/Font/Font.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Containers/Format.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Memory/Memcpy.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"

namespace Lumina::AssetExport
{
    namespace
    {
        // Sized from the literal, since a signature such as TrueType's starts with a zero byte.
        template<size_t N>
        bool StartsWith(const TVector<uint8>& Bytes, const char (&Magic)[N], size_t Offset = 0)
        {
            constexpr size_t Length = N - 1;
            return static_cast<size_t>(Bytes.Num()) >= Offset + Length && Memory::Memcmp(Bytes.GetData() + Offset, Magic, Length) == 0;
        }

        // The file's own signature, for an asset that never recorded the path it came from.
        FString SniffExtension(const TVector<uint8>& Bytes)
        {
            if (StartsWith(Bytes, "\x89PNG"))                          { return ".png"; }
            if (StartsWith(Bytes, "\xFF\xD8\xFF"))                     { return ".jpg"; }
            if (StartsWith(Bytes, "DDS "))                             { return ".dds"; }
            if (StartsWith(Bytes, "\xABKTX"))                          { return ".ktx"; }
            if (StartsWith(Bytes, "#?RADIANCE") || StartsWith(Bytes, "#?RGBE")) { return ".hdr"; }
            if (StartsWith(Bytes, "v/1\x01"))                          { return ".exr"; }
            if (StartsWith(Bytes, "RIFF") && StartsWith(Bytes, "WAVE", 8)) { return ".wav"; }
            if (StartsWith(Bytes, "OggS"))                             { return ".ogg"; }
            if (StartsWith(Bytes, "fLaC"))                             { return ".flac"; }
            if (StartsWith(Bytes, "ID3") || (Bytes.Num() > 1 && Bytes[0] == 0xFF && (Bytes[1] & 0xE0) == 0xE0)) { return ".mp3"; }
            if (StartsWith(Bytes, "OTTO"))                             { return ".otf"; }
            if (StartsWith(Bytes, "ttcf"))                             { return ".ttc"; }
            if (StartsWith(Bytes, "\x00\x01\x00\x00") || StartsWith(Bytes, "true")) { return ".ttf"; }
            return FString();
        }

        FString ExtensionFor(const TVector<uint8>& Bytes, const FString& SourcePath, const char* Fallback)
        {
            FString Extension = Paths::GetExtension(SourcePath);
            if (Extension.IsEmpty())
            {
                Extension = SniffExtension(Bytes);
            }
            return Extension.IsEmpty() ? FString(Fallback) : Extension;
        }

        // Never overwrites, so exporting twice into one folder keeps both copies.
        FString FreePath(FStringView Directory, const FString& Name, const FString& Extension)
        {
            FString Candidate(*Paths::Combine(Directory, FStringView(Name + Extension)));
            for (int32 Suffix = 1; Paths::Exists(Candidate); ++Suffix)
            {
                Candidate = FString(*Paths::Combine(Directory, FStringView(Format("{}_{}{}", Name, Suffix, Extension))));
            }
            return Candidate;
        }

        // The bytes the asset was imported from, with the path that names their format.
        bool SourceBytesOf(CObject* Asset, const TVector<uint8>*& OutBytes, FString& OutSourcePath, const char*& OutFallback, FString& OutError)
        {
            if (CTexture* Texture = Cast<CTexture>(Asset))
            {
                if (!Texture->HasSourceFile() || !Texture->LoadSourceFileBytes())
                {
                    OutError = "has no stored source image";
                    return false;
                }
                OutBytes      = &Texture->GetSourceFileBytes();
                OutSourcePath = Texture->SourcePath;
                OutFallback   = ".png";
                return true;
            }
            if (CAudioStream* Stream = Cast<CAudioStream>(Asset))
            {
                if (!Stream->IsValid())
                {
                    OutError = "has no audio data";
                    return false;
                }
                OutBytes      = &Stream->GetAudioData()->Bytes;
                OutSourcePath = Stream->SourcePath;
                OutFallback   = ".wav";
                return true;
            }
            if (CFont* Font = Cast<CFont>(Asset))
            {
                if (!Font->IsValid())
                {
                    OutError = "has no font data";
                    return false;
                }
                OutBytes      = &Font->GetFontData();
                OutSourcePath = Font->SourcePath;
                OutFallback   = ".ttf";
                return true;
            }
            OutError = "is not a type that can be exported";
            return false;
        }
    }

    bool CanExport(const CClass* AssetClass)
    {
        return AssetClass != nullptr
            && (AssetClass->IsChildOf(CTexture::StaticClass())
                || AssetClass->IsChildOf(CAudioStream::StaticClass())
                || AssetClass->IsChildOf(CFont::StaticClass()));
    }

    bool Export(CObject* Asset, FStringView Directory, FString& OutPath, FString& OutError)
    {
        if (Asset == nullptr)
        {
            OutError = "did not load";
            return false;
        }

        const TVector<uint8>* Bytes = nullptr;
        FString SourcePath;
        const char* Fallback = "";
        if (!SourceBytesOf(Asset, Bytes, SourcePath, Fallback, OutError))
        {
            return false;
        }

        OutPath = FreePath(Directory, FString(*Asset->GetName()), ExtensionFor(*Bytes, SourcePath, Fallback));
        if (!FileHelper::SaveArrayToFile(*Bytes, FStringView(*OutPath, OutPath.Len())))
        {
            OutError = Format("could not be written to {}", OutPath);
            return false;
        }
        return true;
    }
}
