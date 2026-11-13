#include "CSharpAssetScan.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/HashTable.h"
#include "Containers/StringFormat.h"
#include "FileSystem/FileSystem.h"

namespace Lumina
{
    namespace
    {
        bool IsCSharpSource(FStringView VirtualPath)
        {
            return VFS::Extension(VirtualPath) == ".cs";
        }

        // Scripts read tables like item lists from JSON and load the meshes those name, so the data is scanned with the code.
        bool IsScannedSource(FStringView VirtualPath)
        {
            const FStringView Extension = VFS::Extension(VirtualPath);
            return (Extension == ".cs" || Extension == ".json")
                && VirtualPath.Find("/obj/") == Constants::kIndexNone
                && VirtualPath.Find("/bin/") == Constants::kIndexNone
                && VirtualPath.Find("/Config/") == Constants::kIndexNone;
        }

        // Reads one literal from its opening quote, returning the index just past its end.
        int32 ReadLiteral(FStringView Src, int32 Quote, bool bVerbatim, bool bInterpolated, FString& Out, bool& bOutCut)
        {
            int32 i = Quote + 1;
            while (i < Src.Len())
            {
                const char c = Src[i];
                if (bVerbatim && c == '"' && i + 1 < Src.Len() && Src[i + 1] == '"')
                {
                    Out.AppendChar('"');
                    i += 2;
                    continue;
                }
                if (c == '"')
                {
                    return i + 1;
                }
                if (!bVerbatim && c == '\\' && i + 1 < Src.Len())
                {
                    Out.AppendChar(Src[i + 1]);
                    i += 2;
                    continue;
                }
                if (bInterpolated && c == '{')
                {
                    if (i + 1 < Src.Len() && Src[i + 1] == '{')
                    {
                        Out.AppendChar('{');
                        i += 2;
                        continue;
                    }
                    bOutCut = true;
                }
                if (!bOutCut)
                {
                    Out.AppendChar(c);
                }
                if (!bVerbatim && c == '\n')
                {
                    return i + 1;
                }
                ++i;
            }
            return i;
        }

        bool LooksLikeContentPath(FStringView Literal)
        {
            return Literal.Len() > 1 && Literal[0] == '/' && Literal.Find(' ') == Constants::kIndexNone;
        }

        // A mount root such as /Game or /Game/Content would cook the whole project, so a folder must sit below one.
        bool IsSpecificFolder(FStringView Folder)
        {
            while (!Folder.IsEmpty() && Folder.Last() == '/')
            {
                Folder.RemoveSuffix(1);
            }
            int32 Segments = 0;
            for (const char c : Folder)
            {
                Segments += c == '/' ? 1 : 0;
            }
            return Segments >= 3;
        }
    }

    TVector<FString> FCSharpAssetScan::ExtractCandidates(FStringView Src)
    {
        TVector<FString> Out;
        int32 i = 0;
        while (i < Src.Len())
        {
            const char c = Src[i];

            if (c == '/' && i + 1 < Src.Len() && Src[i + 1] == '/')
            {
                const int32 End = Src.Find('\n', i);
                i = End == Constants::kIndexNone ? Src.Len() : End;
                continue;
            }
            if (c == '/' && i + 1 < Src.Len() && Src[i + 1] == '*')
            {
                const int32 End = Src.Find("*/", i + 2);
                i = End == Constants::kIndexNone ? Src.Len() : End + 2;
                continue;
            }
            if (c == '\'')
            {
                // A char literal, skipped so a quote inside it cannot open a string.
                const int32 Close = Src.Find('\'', i + (i + 1 < Src.Len() && Src[i + 1] == '\\' ? 3 : 2));
                i = Close == Constants::kIndexNone ? Src.Len() : Close + 1;
                continue;
            }

            bool bVerbatim = false;
            bool bInterpolated = false;
            int32 Quote = i;
            while (Quote < Src.Len() && (Src[Quote] == '@' || Src[Quote] == '$') && Quote - i < 2)
            {
                bVerbatim |= Src[Quote] == '@';
                bInterpolated |= Src[Quote] == '$';
                ++Quote;
            }

            if (Quote < Src.Len() && Src[Quote] == '"')
            {
                FString Literal;
                bool bCut = false;
                i = ReadLiteral(Src, Quote, bVerbatim, bInterpolated, Literal, bCut);
                if (bCut)
                {
                    Literal.LeftInline(VFS::Parent(FStringView(*Literal, Literal.Len())).Len());
                }
                if (LooksLikeContentPath(FStringView(*Literal, Literal.Len())))
                {
                    Out.Emplace(Move(Literal));
                }
                continue;
            }

            ++i;
        }
        return Out;
    }

    FCSharpAssetScan::FResult FCSharpAssetScan::ScanRoots(const TVector<FString>& VirtualRoots, const FAssetRegistry& Registry,
                                                           const TFunction<void(FStringView)>& LogFunc)
    {
        FResult Result;
        THashSet<FString> Seen;

        TVector<FString> Sources;
        for (const FString& Root : VirtualRoots)
        {
            VFS::RecursiveDirectoryIterator(Root, [&](const VFS::FFileInfo& Info)
            {
                if (!Info.IsDirectory() && IsScannedSource(FStringView(*Info.VirtualPath, Info.VirtualPath.Len())))
                {
                    Sources.Emplace(*Info.VirtualPath, Info.VirtualPath.Len());
                }
            });
        }

        for (const FString& Source : Sources)
        {
            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, FStringView(*Source, Source.Len())))
            {
                continue;
            }
            ++Result.FilesScanned;

            // A data file can name thousands of assets, so only code gets a line per reference.
            const bool bLogEachRef = IsCSharpSource(FStringView(*Source, Source.Len()));
            const int32 AssetsBefore = Result.AssetPaths.Num();
            const int32 FoldersBefore = Result.FolderPaths.Num();

            const FStringView Contents(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
            for (FString& Candidate : ExtractCandidates(Contents))
            {
                ++Result.RawCandidates;
                if (!Seen.TryAdd(Candidate))
                {
                    continue;
                }

                FString AssetPath = Candidate;
                if (!AssetPath.EndsWith(".lasset") && !AssetPath.EndsWith("/"))
                {
                    AssetPath += ".lasset";
                }

                const char* Kind = nullptr;
                if (Registry.GetAssetByPath(FStringView(*Candidate, Candidate.Len())) != nullptr)
                {
                    Result.AssetPaths.Add(Candidate);
                    Kind = "asset";
                }
                else if (AssetPath != Candidate && Registry.GetAssetByPath(FStringView(*AssetPath, AssetPath.Len())) != nullptr)
                {
                    Result.AssetPaths.Add(AssetPath);
                    Kind = "asset";
                }
                else if (IsSpecificFolder(FStringView(*Candidate, Candidate.Len())) && VFS::IsDirectory(FStringView(*Candidate, Candidate.Len())))
                {
                    Result.FolderPaths.Add(Candidate);
                    Kind = "folder";
                }

                if (Kind != nullptr && LogFunc && bLogEachRef)
                {
                    FString Msg = FString("  [cs-ref] ") + Kind + " " + Candidate + "  (from " + Source + ")";
                    LogFunc(FStringView(*Msg, Msg.Len()));
                }
            }

            const int32 NewAssets = Result.AssetPaths.Num() - AssetsBefore;
            const int32 NewFolders = Result.FolderPaths.Num() - FoldersBefore;
            if (!bLogEachRef && LogFunc && (NewAssets > 0 || NewFolders > 0))
            {
                const FString Msg = Format("  [data-ref] {} names {} asset(s) and {} folder(s)", *Source, NewAssets, NewFolders);
                LogFunc(FStringView(*Msg, Msg.Len()));
            }
        }
        return Result;
    }
}
