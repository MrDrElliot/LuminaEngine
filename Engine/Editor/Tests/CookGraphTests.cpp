#include <gtest/gtest.h>

#include "AssetRegistryTestAccess.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Cooker/Graph/CookGraph.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"

using namespace Lumina;

namespace
{
    FAssetDataPtr CreateAsset(FAssetRegistry& Registry, const char* Path, const char* Name)
    {
        CDataTable* Table = NewObject<CDataTable>(CPackage::CreatePackage(Path), Name);
        Registry.AssetCreated(Table);
        return Registry.GetAssetByGUID(Table->GetGUID());
    }

    void SetDependency(FAssetRegistry& Registry, const FAssetDataPtr& From, const FAssetDataPtr& To, EDependencyType Type)
    {
        FAssetData Edited = *From;
        Edited.Dependencies = { { To->AssetGUID, Type } };
        FAssetRegistryTestAccess::ReplaceEntry(Registry, Move(Edited));
    }

    void ExpectOwnedEdgePullsSubtree(bool bHardRootFirst)
    {
        CDataTable::StaticClass();
        RegisterPendingTypes();

        FAssetRegistry& Registry = FAssetRegistry::Get();
        const FString Base = FString("/CookGraphRechunk") + (bHardRootFirst ? "HardFirst" : "OwnedFirst");

        FAssetDataPtr A = CreateAsset(Registry, *(Base + "/A/A"), "A");
        FAssetDataPtr P = CreateAsset(Registry, *(Base + "/P/P"), "P");
        FAssetDataPtr B = CreateAsset(Registry, *(Base + "/Shared/B"), "B");
        FAssetDataPtr C = CreateAsset(Registry, *(Base + "/Shared/C"), "C");
        ASSERT_TRUE(A && P && B && C);

        SetDependency(Registry, A, B, EDependencyType::Hard);
        SetDependency(Registry, P, B, EDependencyType::Owned);
        SetDependency(Registry, B, C, EDependencyType::Hard);

        const FName Main("Main");
        const FName Owner("Owner");
        FCookGraph Graph(Registry);
        if (bHardRootFirst)
        {
            Graph.AddFolderRoot(*(Base + "/A"), Main);
            Graph.Traverse();
            Graph.AddFolderRoot(*(Base + "/P"), Owner);
        }
        else
        {
            Graph.AddFolderRoot(*(Base + "/P"), Owner);
            Graph.Traverse();
            Graph.AddFolderRoot(*(Base + "/A"), Main);
        }
        Graph.Traverse();

        for (const FCookGraphIssue& Issue : Graph.GetIssues())
        {
            ADD_FAILURE() << *Issue.Source << ": " << *Issue.Detail;
        }
        EXPECT_EQ(Graph.GetChunkFor(A->AssetGUID), Main);
        EXPECT_EQ(Graph.GetChunkFor(P->AssetGUID), Owner);
        EXPECT_EQ(Graph.GetChunkFor(B->AssetGUID), Owner);
        EXPECT_EQ(Graph.GetChunkFor(C->AssetGUID), Owner);

        int32 OwnedNodes = 0;
        for (const FCookNode* Node : Graph.GetReachableNodesSorted())
        {
            if (Node->AssetGUID == B->AssetGUID || Node->AssetGUID == C->AssetGUID)
            {
                EXPECT_EQ(Node->Chunk, Owner);
                ++OwnedNodes;
            }
        }
        EXPECT_EQ(OwnedNodes, 2);

        for (const FGuid& Guid : { A->AssetGUID, P->AssetGUID, B->AssetGUID, C->AssetGUID })
        {
            Registry.AssetDeleted(Guid);
        }
    }
}

TEST(CookGraph, OwnedEdgeToReachedNodeMovesItsSubtree)
{
    ExpectOwnedEdgePullsSubtree(true);
}

TEST(CookGraph, OwnedEdgeBeforeHardRootKeepsItsSubtree)
{
    ExpectOwnedEdgePullsSubtree(false);
}
