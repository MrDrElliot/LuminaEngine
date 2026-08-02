#include "RuntimePCH.h"
#include "AssetRef.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Serialization/NetArchive.h"
#include "FileSystem/FileSystem.h"

namespace Lumina
{
    void FAssetRef::NetSerialize(FNetArchive& Ar)
    {
        if (Ar.IsWriting())
        {
            // Indexed path (replication): a compact net index; Path+GUID are exported once via AssetExport.
            if (Ar.AssetRefToNetIndex)
            {
                std::ignore = ResolvePath(); // heal/back-fill the GUID so the export keys on a stable identity
                WriteVarUInt(Ar, IsNull() ? 0u : Ar.AssetRefToNetIndex(*this));
                return;
            }

            Ar << Path;
            Ar << Guid;
        }
        else
        {
            if (Ar.NetIndexToAssetRef)
            {
                const uint32 Index = ReadVarUInt(Ar);
                Reset();
                if (Index != 0)
                {
                    Ar.NetIndexToAssetRef(Index, *this);
                }
                return;
            }

            Ar << Path;
            Ar << Guid;
        }
    }

    FGuid FAssetRef::GetGuid() const
    {
        if (Guid.empty())
        {
            return FGuid();
        }
        if (TOptional<FGuid> Parsed = FGuid::TryParse(FStringView(Guid.c_str(), Guid.size())); Parsed.has_value())
        {
            return *Parsed;
        }
        return FGuid();
    }

    FStringView FAssetRef::ResolvePath() const
    {
        FAssetRegistry& Registry = FAssetRegistry::Get();

        // GUID-first: a valid GUID locates the asset even after a rename/move; heal Path in place.
        const FGuid G = GetGuid();
        if (G.IsValid())
        {
            if (FTextAssetData* Data = Registry.GetTextAssetByGUID(G))
            {
                const FStringView Current(Data->Path.c_str(), Data->Path.size());
                if (Current != GetPath())
                {
                    const_cast<FAssetRef*>(this)->Path.assign(Current.data(), Current.size());
                }
                return GetPath();
            }
        }

        // No GUID (or it didn't resolve): fall back to path, back-filling the GUID from the discovered
        // record. This self-heals legacy path-only data once the file has a sidecar.
        if (!Path.empty())
        {
            if (FTextAssetData* Data = Registry.GetTextAssetByPath(GetPath()))
            {
                const_cast<FAssetRef*>(this)->Guid = Data->Guid.ToString();
            }
        }

        return GetPath();
    }

    bool FAssetRef::ReadText(FString& OutText) const
    {
        const FStringView ResolvedPath = ResolvePath();
        if (ResolvedPath.empty())
        {
            return false;
        }
        return VFS::ReadFile(OutText, ResolvedPath);
    }

    void FAssetRef::SetPath(FStringView InPath)
    {
        Path.assign(InPath.data(), InPath.size());
        Guid.clear();

        // Mint/resolve a sidecar identity now (editor context).
        const FGuid G = FAssetRegistry::Get().EnsureTextAsset(InPath);
        if (G.IsValid())
        {
            Guid = G.ToString();
        }
    }

    void FAssetRef::Set(FStringView InPath, const FGuid& InGuid)
    {
        Path.assign(InPath.data(), InPath.size());
        Guid = InGuid.IsValid() ? InGuid.ToString() : FString();
    }
}
