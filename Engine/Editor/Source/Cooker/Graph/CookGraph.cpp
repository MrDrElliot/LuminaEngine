#include "CookGraph.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "Containers/Algorithm.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    void FCookGraph::AddRoot(const FCookRoot& Root)
    {
        const FName Chunk = Root.Chunk.IsNone() ? FName("Main") : Root.Chunk;

        // A trailing slash or an existing folder cooks everything inside it.
        const FStringView AssetView(*Root.Asset, Root.Asset.Len());
        if (AssetView.EndsWith("/") || VFS::IsDirectory(AssetView))
        {
            AddFolderRoot(AssetView, Chunk);
            return;
        }

        // Accept either bare "/Game/Maps/X" or full "/Game/Maps/X.lasset".
        const FFixedString Resolved = VFS::ResolveToVirtualPath(Root.Asset);
        const FStringView View(*Resolved, Resolved.Len());

        FAssetDataPtr Data = Registry->GetAssetByPath(View);
        if (!Data)
        {
            FCookGraphIssue Issue;
            Issue.Source = "<root>";
            Issue.Detail = FString("Cook root '").Append(*Root.Asset, Root.Asset.Len())
                + "' did not resolve to a registered asset (configured as "
                + FString(*Resolved, Resolved.Len()) + ").";
            Issues.Emplace(Move(Issue));
            return;
        }

        VisitTarget(Data->AssetGUID, Chunk, EDependencyType::Hard);
    }

    void FCookGraph::AddFolderRoot(FStringView Folder, const FName& Chunk)
    {
        FString Prefix(Folder.GetData(), Folder.Len());
        while (!Prefix.IsEmpty() && Prefix.Last() == '/')
        {
            Prefix.LeftChopInline(1);
        }
        Prefix.AppendChar('/');

        TVector<FAssetDataPtr> Contained = Registry->FindByPredicate([&Prefix](const FAssetData& Data)
        {
            return FStringView(*Data.Path, Data.Path.Len()).StartsWith(FStringView(*Prefix, Prefix.Len()));
        });

        if (Contained.IsEmpty())
        {
            FCookGraphIssue Issue;
            Issue.Source = "<root>";
            Issue.Detail = FString("Cook folder '") + Prefix + "' holds no registered assets.";
            Issues.Emplace(Move(Issue));
            return;
        }

        // Registry order is hash order, so sort to keep the cook reproducible.
        Algo::Sort(Contained, [](const FAssetDataPtr& A, const FAssetDataPtr& B) { return A->AssetGUID < B->AssetGUID; });
        for (const FAssetDataPtr& Data : Contained)
        {
            VisitTarget(Data->AssetGUID, Chunk, EDependencyType::Hard);
        }
    }

    void FCookGraph::AddRoots(const TVector<FCookRoot>& Roots)
    {
        for (const FCookRoot& R : Roots)
        {
            AddRoot(R);
        }
    }

    bool FCookGraph::VisitTarget(const FGuid& TargetGUID,
                                 const FName& InheritedChunk,
                                 EDependencyType EdgeType)
    {
        // Editor-only edges are pruned for cooked output entirely.
        if (EdgeType == EDependencyType::EditorOnly)        return false;
        if (EdgeType == EDependencyType::Generated)         return false;

        auto It = Nodes.Find(TargetGUID);
        if (It != nullptr)
        {
            // Already in the graph, and an owned edge wins because it pulls the target into the referrer's chunk.
            if (EdgeType == EDependencyType::Owned)
            {
                Rechunk(TargetGUID, InheritedChunk);
            }
            // Soft/Script don't change chunk; first Hard root wins.
            return false;
        }

        FAssetDataPtr Data = Registry->GetAssetByGUID(TargetGUID);
        if (!Data)
        {
            // A reachable GUID with no asset record is not cookable, and real dangling refs still warn at load.
            return false;
        }

        // Asset-level NeverCook stays out of the graph and records an issue instead.
        if (HasFlag(Data->Flags, EAssetFlags::NeverCook))
        {
            FCookGraphIssue Issue;
            Issue.Source = FString(*Data->Path, Data->Path.Len());
            Issue.Detail = "Asset is marked NeverCook but was reached via the dependency graph; check inbound references.";
            Issues.Emplace(Move(Issue));
            return false;
        }
        // A package whose class is gone cannot load in the game either, so shipping it only carries dead bytes.
        if (Data->IsClassMissing())
        {
            FCookGraphIssue Issue;
            Issue.Source = FString(*Data->Path, Data->Path.Len());
            Issue.Detail = FString("Asset type '") + *Data->AssetClass + "' no longer exists, so the asset was left out; delete it or restore the type.";
            Issues.Emplace(Move(Issue));
            return false;
        }

        // Editor-only assets always stripped.
        if (HasFlag(Data->Flags, EAssetFlags::EditorOnly))
        {
            return false;
        }

        FCookNode Node;
        Node.AssetGUID      = Data->AssetGUID;
        Node.Path           = Data->Path;
        Node.AssetClass     = Data->AssetClass;
        Node.Chunk          = InheritedChunk;
        Node.EffectiveFlags = Data->Flags;

        Nodes.TryAdd(TargetGUID, Move(Node));
        Frontier.Add(TargetGUID);
        return true;
    }

    void FCookGraph::Rechunk(const FGuid& TargetGUID, const FName& Chunk)
    {
        const FName OldChunk = Nodes.Find(TargetGUID)->Chunk;
        if (OldChunk == Chunk)
        {
            return;
        }

        // Only nodes that inherited the old chunk follow, so a subtree another root placed elsewhere stays put.
        TVector<FGuid> Pending = { TargetGUID };
        while (!Pending.IsEmpty())
        {
            const FGuid Current = Pending.Last();
            Pending.Pop();

            FCookNode* Node = Nodes.Find(Current);
            if (Node == nullptr || Node->Chunk != OldChunk)
            {
                continue;
            }
            Node->Chunk = Chunk;

            const FAssetDataPtr Data = Registry->GetAssetByGUID(Current);
            if (!Data) continue;

            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                if (Dep.Type == EDependencyType::Hard || Dep.Type == EDependencyType::Owned)
                {
                    Pending.Add(Dep.TargetGUID);
                }
            }
        }
    }

    void FCookGraph::Traverse()
    {
        while (!Frontier.IsEmpty())
        {
            const FGuid Current = Frontier.Last();
            Frontier.Pop();

            auto NodeIt = Nodes.Find(Current);
            if (NodeIt == nullptr) continue; // defensive
            const FName ParentChunk = NodeIt->Chunk;

            FAssetDataPtr Data = Registry->GetAssetByGUID(Current);
            if (!Data) continue;

            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                // Owned edges inherit the parent's chunk, and so does everything else until chunk routing lands.
                VisitTarget(Dep.TargetGUID, ParentChunk, Dep.Type);
            }
        }
    }

    bool FCookGraph::IsReachable(const FGuid& A) const
    {
        return Nodes.Contains(A);
    }

    const FCookNode* FCookGraph::Find(const FGuid& A) const
    {
        auto It = Nodes.Find(A);
        return It == nullptr ? nullptr : &(*It);
    }

    FName FCookGraph::GetChunkFor(const FGuid& A) const
    {
        auto It = Nodes.Find(A);
        return It == nullptr ? FName() : It->Chunk;
    }

    TVector<const FCookNode*> FCookGraph::GetReachableNodesSorted() const
    {
        // Sorted by GUID so identical inputs give identical PAK order and a reproducible build hash.
        TVector<const FCookNode*> Out;
        Out.Reserve(Nodes.Num());
        for (const auto& [Guid, Node] : Nodes)
        {
            Out.Add(&Node);
        }
        Algo::Sort(Out, [](const FCookNode* A, const FCookNode* B)
        {
            return A->AssetGUID < B->AssetGUID;
        });
        return Out;
    }
}
