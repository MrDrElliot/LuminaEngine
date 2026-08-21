#include "RmlUiAssetScan.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

#include <cctype>


namespace Lumina
{
    namespace
    {
        bool IsPathChar(char c)
        {
            // A conservative VFS path set, rejecting quotes, whitespace, parens and attribute delimiters.
            return c != '\0'
                && !std::isspace(static_cast<unsigned char>(c))
                && c != '"' && c != '\''
                && c != '(' && c != ')'
                && c != '<' && c != '>'
                && c != ';' && c != ',';
        }

        // Blank out `<!-- -->` and `/* */` comment ranges with equal-length spaces, preserving byte offsets and keeping downstream scanners comment-agnostic.
        FString StripCommentRanges(FStringView Src)
        {
            FString Out(Src.data(), Src.size());
            auto Blank = [&](size_t From, size_t To)
            {
                for (size_t i = From; i < To && i < Out.size(); ++i)
                {
                    if (Out[i] != '\n') Out[i] = ' ';
                }
            };

            size_t i = 0;
            while (i < Out.size())
            {
                // HTML/RML comment.
                if (i + 3 < Out.size()
                    && Out[i] == '<' && Out[i + 1] == '!'
                    && Out[i + 2] == '-' && Out[i + 3] == '-')
                {
                    const size_t Start = i;
                    i += 4;
                    while (i + 2 < Out.size()
                        && !(Out[i] == '-' && Out[i + 1] == '-' && Out[i + 2] == '>'))
                    {
                        ++i;
                    }
                    const size_t End = (i + 2 < Out.size()) ? (i + 3) : Out.size();
                    Blank(Start, End);
                    i = End;
                    continue;
                }
                // CSS/RCSS block comment.
                if (i + 1 < Out.size() && Out[i] == '/' && Out[i + 1] == '*')
                {
                    const size_t Start = i;
                    i += 2;
                    while (i + 1 < Out.size() && !(Out[i] == '*' && Out[i + 1] == '/'))
                    {
                        ++i;
                    }
                    const size_t End = (i + 1 < Out.size()) ? (i + 2) : Out.size();
                    Blank(Start, End);
                    i = End;
                    continue;
                }
                ++i;
            }
            return Out;
        }

        // Rejects a match that is really the tail of another identifier, such as data-src or nosrc.
        bool IsAttributeBoundary(FStringView Src, size_t Hit)
        {
            if (Hit == 0) return true;
            const char Prev = Src[Hit - 1];
            return !(std::isalnum(static_cast<unsigned char>(Prev))
                || Prev == '_' || Prev == '-');
        }

        // Scans forward for a path-like substring starting with a slash and holding at least one dot.
        FStringView ScanPath(FStringView Src, size_t Pos)
        {
            // Skip leading whitespace and a single optional quote.
            while (Pos < Src.size() && std::isspace(static_cast<unsigned char>(Src[Pos]))) ++Pos;
            if (Pos < Src.size() && (Src[Pos] == '"' || Src[Pos] == '\'')) ++Pos;

            if (Pos >= Src.size() || Src[Pos] != '/')
            {
                return {};
            }

            const size_t Start = Pos;
            while (Pos < Src.size() && IsPathChar(Src[Pos]))
            {
                ++Pos;
            }
            FStringView Candidate = Src.substr(Start, Pos - Start);

            // Must contain an extension separator; rejects bare "/Game" / "/".
            if (Candidate.find('.') == FStringView::npos)
            {
                return {};
            }
            return Candidate;
        }

        // Capture the path after each `<Key>=`; left-anchored against an identifier-tail so `data-src=`/`nosrc=` don't false-match. Tolerates whitespace around `=`.
        void ScanAttribute(FStringView Src, FStringView Key, TVector<FString>& Out)
        {
            size_t Pos = 0;
            while (Pos < Src.size())
            {
                size_t Hit = Src.find(Key, Pos);
                if (Hit == FStringView::npos) return;
                Pos = Hit + Key.size();

                if (!IsAttributeBoundary(Src, Hit)) continue;

                size_t P = Pos;
                while (P < Src.size() && std::isspace(static_cast<unsigned char>(Src[P]))) ++P;
                if (P >= Src.size() || Src[P] != '=') continue;
                ++P;

                FStringView Path = ScanPath(Src, P);
                if (!Path.empty())
                {
                    Out.emplace_back(Path.data(), Path.size());
                    Pos = P + Path.size();
                }
            }
        }

        // Handles single, double and unquoted forms, with a left boundary check against ident tails.
        void ScanCssUrl(FStringView Src, TVector<FString>& Out)
        {
            const FStringView Tok("url(");
            size_t Pos = 0;
            while (Pos < Src.size())
            {
                size_t Hit = Src.find(Tok, Pos);
                if (Hit == FStringView::npos) return;
                Pos = Hit + Tok.size();

                if (!IsAttributeBoundary(Src, Hit)) continue;

                FStringView Path = ScanPath(Src, Pos);
                if (!Path.empty())
                {
                    Out.emplace_back(Path.data(), Path.size());
                    Pos += Path.size();
                }
            }
        }

        // The UI material brush URI scheme, boundary-checked so ordinary prose cannot match it.
        void ScanMaterialUri(FStringView Src, TVector<FString>& Out)
        {
            const FStringView Tok("material:");
            size_t Pos = 0;
            while (Pos < Src.size())
            {
                size_t Hit = Src.find(Tok, Pos);
                if (Hit == FStringView::npos) return;
                Pos = Hit + Tok.size();

                if (!IsAttributeBoundary(Src, Hit)) continue;

                FStringView Path = ScanPath(Src, Pos);
                if (!Path.empty())
                {
                    Out.emplace_back(Path.data(), Path.size());
                    Pos += Path.size();
                }
            }
        }

        bool IsRmlOrRcss(FStringView VirtualPath)
        {
            FStringView Ext = VFS::Extension(VirtualPath);
            return Ext == ".rml" || Ext == ".rcss";
        }
    }


    TVector<FString> FRmlUiAssetScan::ExtractCandidates(FStringView Contents)
    {
        // Stripped first so commented-out examples never show up as cook roots.
        const FString Stripped = StripCommentRanges(Contents);
        const FStringView Src(Stripped.c_str(), Stripped.size());

        TVector<FString> Out;
        Out.reserve(8);

        // RML/HTML attributes that commonly carry asset paths.
        ScanAttribute(Src, FStringView("src"),        Out);
        ScanAttribute(Src, FStringView("href"),       Out);
        ScanAttribute(Src, FStringView("sprite"),     Out);
        ScanAttribute(Src, FStringView("data-asset"), Out);

        // CSS / RCSS.
        ScanCssUrl(Src, Out);
        ScanMaterialUri(Src, Out);

        return Out;
    }


    FRmlUiAssetScan::FResult FRmlUiAssetScan::ScanRoots(
        const TVector<FString>& VirtualRoots,
        const FAssetRegistry& Registry,
        const TFunction<void(FStringView)>& LogFunc)
    {
        FResult Result;
        THashSet<FString> UniqueAssets;
        THashSet<FString> VisitedDocs;
        TVector<FString>  Worklist;

        // Seeded from every .rml and .rcss under the content roots, then @import chains are followed.
        for (const FString& Root : VirtualRoots)
        {
            VFS::RecursiveDirectoryIterator(Root, [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory()) return;
                FStringView Vp(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                if (!IsRmlOrRcss(Vp)) return;
                Worklist.emplace_back(Vp.data(), Vp.size());
            });
        }

        while (!Worklist.empty())
        {
            FString DocPath = Move(Worklist.back());
            Worklist.pop_back();

            if (VisitedDocs.find(DocPath) != VisitedDocs.end()) continue;
            VisitedDocs.insert(DocPath);

            FStringView Vp(DocPath.c_str(), DocPath.size());

            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, Vp)) continue;

            ++Result.FilesScanned;

            FStringView Contents(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
            TVector<FString> Candidates = ExtractCandidates(Contents);
            Result.RawCandidates += Candidates.size();

            for (FString& Candidate : Candidates)
            {
                FStringView CandView(Candidate.c_str(), Candidate.size());

                // Another .rml or .rcss on disk is queued for its own scan, with no registry membership required.
                if (IsRmlOrRcss(CandView) && VFS::Exists(Candidate)
                    && VisitedDocs.find(Candidate) == VisitedDocs.end())
                {
                    Worklist.emplace_back(Candidate);
                }

                if (UniqueAssets.find(Candidate) != UniqueAssets.end())
                {
                    continue;
                }
                // Only registered-asset paths count as cook roots; .ttf/.png loose files still ship via BundleLooseContent.
                if (Registry.GetAssetByPath(CandView) == nullptr)
                {
                    continue;
                }

                if (LogFunc)
                {
                    FString Msg("  [rml-ref] ");
                    Msg += Candidate;
                    Msg += "  (from ";
                    Msg.append(Vp.data(), Vp.size());
                    Msg += ")";
                    LogFunc(FStringView(Msg.c_str(), Msg.size()));
                }
                UniqueAssets.insert(Candidate);
                Result.AssetPaths.emplace_back(Move(Candidate));
                ++Result.ResolvedRefs;
            }
        }

        return Result;
    }
}
