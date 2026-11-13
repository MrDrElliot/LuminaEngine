#include "RuntimePCH.h"
#include "ArtifactCache.h"

#include "Config/Config.h"
#include "Config/EngineSettings.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Hash/Hash.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kMagic   = 0x5452414C; // 'LART'
        constexpr uint32 kVersion = 1;

        struct FEntryHeader
        {
            uint32 Magic;
            uint32 Version;
            uint64 Key;
            uint64 Size;
            uint64 PayloadHash;
        };
        static_assert(sizeof(FEntryHeader) == 32, "FEntryHeader is the on-disk prefix of every artifact.");

        FString EntryPath(const FString& Root, uint64 Key)
        {
            const FString Hex = *Format("{:016x}", Key);
            return FString(*Paths::Combine(Root, Hex.Mid(0, 2), Hex + ".lart"));
        }

        bool ReadEntry(const FString& Path, uint64 Key, TVector<uint8>& OutData)
        {
            TVector<uint8> Bytes;
            if (!Filesystem::IsFile(Path) || !Filesystem::ReadFile(Bytes, Path) || static_cast<size_t>(Bytes.Num()) < sizeof(FEntryHeader))
            {
                return false;
            }

            FEntryHeader Header;
            Memory::Memcpy(&Header, Bytes.GetData(), sizeof(Header));
            const uint8* Payload = Bytes.GetData() + sizeof(FEntryHeader);
            const uint64 PayloadSize = Bytes.Num() - sizeof(FEntryHeader);

            // A torn copy from a sync tool or a half-written share looks like any other file, so the payload is verified.
            if (Header.Magic != kMagic || Header.Version != kVersion || Header.Key != Key || Header.Size != PayloadSize
                || Hash::GetHash64(Payload, PayloadSize) != Header.PayloadHash)
            {
                LOG_WARN("Artifact cache entry '{}' is corrupt and was ignored.", Path);
                return false;
            }

            OutData.Reset();
            OutData.Append(Payload, Payload + PayloadSize);
            return true;
        }

        bool WriteEntry(const FString& Path, uint64 Key, TSpan<const uint8> Data)
        {
            if (Filesystem::IsFile(Path))
            {
                return true;
            }

            const FEntryHeader Header{ kMagic, kVersion, Key, (uint64)Data.Num(), Hash::GetHash64(Data.GetData(), Data.Num()) };
            TVector<uint8> Bytes(sizeof(FEntryHeader) + Data.Num());
            Memory::Memcpy(Bytes.GetData(), &Header, sizeof(Header));
            if (!Data.IsEmpty())
            {
                Memory::Memcpy(Bytes.GetData() + sizeof(FEntryHeader), Data.GetData(), Data.Num());
            }

            return Filesystem::MakeParentDirectoryTree(Path) && Filesystem::AtomicWriteFile(Path, Bytes);
        }

        FString GetLocalDirectory()
        {
            const FStringView Project = Global::GEngine != nullptr ? Global::GEngine->GetProjectPath() : FStringView();
            const FString Base = !Project.IsEmpty() ? FString(Project.GetData(), Project.Len()) : Paths::GetEngineInstallDirectory();
            return FString(*Paths::Combine(Base, "Saved", "ArtifactCache"));
        }

        FString GetSharedDirectory()
        {
            if (Global::GConfig == nullptr)
            {
                return {};
            }

            const FString& Setting = GetDefault<CProjectSettings>()->SharedArtifactCachePath;
            if (Setting.IsEmpty())
            {
                return {};
            }

            const FStringView Project = Global::GEngine != nullptr ? Global::GEngine->GetProjectPath() : FStringView();
            const bool bRelative = Setting[0] != '/' && Setting[0] != '\\' && Setting.Find(':') == Constants::kIndexNone;
            if (bRelative && !Project.IsEmpty())
            {
                return FString(*Paths::Combine(FString(Project.GetData(), Project.Len()), Setting));
            }
            return Setting;
        }
    }

    bool FArtifactCache::Load(uint64 Key, TVector<uint8>& OutData)
    {
        if (ReadEntry(EntryPath(GetLocalDirectory(), Key), Key, OutData))
        {
            return true;
        }

        const FString Shared = GetSharedDirectory();
        if (Shared.IsEmpty() || !ReadEntry(EntryPath(Shared, Key), Key, OutData))
        {
            return false;
        }

        // The share may be slow or offline next time, so a hit is kept locally too.
        WriteEntry(EntryPath(GetLocalDirectory(), Key), Key, OutData);
        return true;
    }

    void FArtifactCache::Store(uint64 Key, TSpan<const uint8> Data)
    {
        const FString Local = EntryPath(GetLocalDirectory(), Key);
        if (!WriteEntry(Local, Key, Data))
        {
            LOG_WARN("Could not write artifact cache entry '{}'.", Local);
        }

        const FString Shared = GetSharedDirectory();
        if (!Shared.IsEmpty() && !WriteEntry(EntryPath(Shared, Key), Key, Data) && !bReportedReadOnlyShare.exchange(true))
        {
            LOG_WARN("The shared artifact cache '{}' is not writable, so compiles only reach the local cache.", Shared);
        }
    }
}
