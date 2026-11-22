#pragma once

#include "Assets/AssetRegistry/AssetData.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "GUID/GUID.h"

namespace Lumina
{
    class FAssetRegistry;
    struct FCookRoot;

    // One node in the cook-time reachability graph, living only for one cook pass.
    struct FCookNode
    {
        FGuid                       AssetGUID;
        FFixedString                Path;          // resolved virtual path (cached)
        FName                       AssetClass;
        FName                       Chunk;         // assigned during traversal
        EAssetFlags                 EffectiveFlags = EAssetFlags::None;
    };

    // Why an asset failed to enter the graph; surfaces in the cook log to fix dangling roots / missing deps before shipping.
    struct FCookGraphIssue
    {
        FString    Source;          // path or "<root>" of the failing entry
        FString    Detail;
    };

    // Walks FAssetData::Dependencies from the registry without loading packages. Call AddRoot per cook root, then Traverse once.
    class EDITOR_API FCookGraph
    {
    public:
        explicit FCookGraph(const FAssetRegistry& InRegistry)
            : Registry(&InRegistry) {}

        // Add a seed; resolved paths get a node with the supplied chunk, otherwise an issue is recorded and the root skipped.
        void AddRoot(const FCookRoot& Root);

        // Seeds every registered asset under a content folder, for code that builds asset paths at runtime.
        void AddFolderRoot(FStringView Folder, const FName& Chunk);

        // Convenience: bulk add.
        void AddRoots(const TVector<FCookRoot>& Roots);

        // BFS the registry's Dependencies edges by EDependencyType: Hard/Owned inherit referrer's chunk, Soft/Script use the target's own roots, EditorOnly/Generated skipped.
        void Traverse();

        // Post-Traverse queries.
        bool                IsReachable(const FGuid& A) const;
        const FCookNode*    Find(const FGuid& A) const;
        FName               GetChunkFor(const FGuid& A) const;

        // Every reached node sorted by GUID, so the cooker emits packages in a stable order.
        TVector<const FCookNode*> GetReachableNodesSorted() const;

        size_t              NumNodes() const { return Nodes.Num(); }
        const TVector<FCookGraphIssue>& GetIssues() const { return Issues; }

    private:
        // Marks target reachable, creating and enqueueing its node if new. Returns true if added.
        bool VisitTarget(const FGuid& TargetGUID,
                         const FName& InheritedChunk,
                         EDependencyType EdgeType);

        // Move a reached node and the Hard/Owned subtree that shares its chunk into Chunk.
        void Rechunk(const FGuid& TargetGUID, const FName& Chunk);

        const FAssetRegistry*           Registry;
        THashMap<FGuid, FCookNode, FGuidHash> Nodes;
        TVector<FGuid>                  Frontier;
        TVector<FCookGraphIssue>        Issues;
    };
}
