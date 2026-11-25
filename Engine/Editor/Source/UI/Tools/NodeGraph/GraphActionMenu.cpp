#include "GraphActionMenu.h"

#include "EdGraphNode.h"
#include "EdNodeGraph.h"
#include "EdNodeGraphPin.h"
#include "imgui.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"

#include <cstdlib>

namespace Lumina
{
    namespace
    {
        FORCEINLINE char ToLowerChar(char C)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
        }

        FORCEINLINE bool IsWordSeparator(char C)
        {
            return C == ' ' || C == '_' || C == '-' || C == '.' || C == '/';
        }

        FORCEINLINE bool IsLower(char C) { return C >= 'a' && C <= 'z'; }
        FORCEINLINE bool IsUpper(char C) { return C >= 'A' && C <= 'Z'; }
    }

    bool FuzzyMatch(const char* Query, const char* Target, int32& OutScore, TVector<uint16>& OutMatchIndices)
    {
        OutScore = 0;
        OutMatchIndices.Reset();

        if (Query == nullptr || Query[0] == '\0')
        {
            return true;
        }
        if (Target == nullptr)
        {
            return false;
        }

        const int32 StartBonus        = 50;
        const int32 PrefixBonus       = 10;
        const int32 SeparatorBonus    = 30;
        const int32 CamelCaseBonus    = 30;
        const int32 ConsecutiveBonus  = 20;
        const int32 UnmatchedLeading  = -3;
        const int32 UnmatchedPenalty  = -1;

        int32 Score = 0;
        uint32 QueryIdx = 0;
        bool bPrevMatched = false;
        bool bMatchedAny = false;

        for (uint32 i = 0; Target[i] != '\0' && Query[QueryIdx] != '\0'; ++i)
        {
            const char QC = ToLowerChar(Query[QueryIdx]);
            const char TC = ToLowerChar(Target[i]);

            if (QC == TC)
            {
                OutMatchIndices.Add(static_cast<uint16>(i));

                if (i == 0)
                {
                    Score += StartBonus;
                }
                else
                {
                    const char Prev = Target[i - 1];
                    if (IsWordSeparator(Prev))
                    {
                        Score += SeparatorBonus;
                    }
                    else if (IsLower(Prev) && IsUpper(Target[i]))
                    {
                        Score += CamelCaseBonus;
                    }
                }

                if (bPrevMatched)
                {
                    Score += ConsecutiveBonus;
                }
                else if (!bMatchedAny && i > 0)
                {
                    Score += i * UnmatchedLeading;
                }

                if (QueryIdx == 0 && i == 0)
                {
                    Score += PrefixBonus;
                }

                QueryIdx++;
                bPrevMatched = true;
                bMatchedAny = true;
            }
            else
            {
                if (bMatchedAny)
                {
                    Score += UnmatchedPenalty;
                }
                bPrevMatched = false;
            }
        }

        if (Query[QueryIdx] != '\0')
        {
            OutMatchIndices.Reset();
            OutScore = 0;
            return false;
        }

        OutScore = Score;
        return true;
    }

    FGraphActionMenu::FGraphActionMenu()
    {
        Picker.SearchHint = "Search nodes...";
        Picker.ItemNoun   = "nodes";
    }

    void FGraphActionMenu::Reset()
    {
        bOnlyAcceptingNodes = true;
    }

    void FGraphActionMenu::GatherItems(CEdNodeGraph* Graph, TVector<FPickerItem>& OutItems)
    {
        Actions.Reset();

        THashSet<CClass*> Accepting;
        bHasSourceFilter = Graph->PendingSourcePin != nullptr;
        if (bHasSourceFilter)
        {
            Graph->CollectClassesAccepting(Graph->PendingSourcePin, Accepting);
        }

        // Nodes nest their categories with a bar, where the picker files folders by slash.
        auto AddItem = [&](CClass* NodeClass, FName Variant, const FString& Name, const FString& Category, const FString& Tooltip)
        {
            FPickerItem& Item = OutItems.EmplaceGetRef();
            Item.Name     = Name;
            Item.Key      = Lumina::Format("{}", Actions.Num());
            Item.Category = Category;
            Item.Category.ReplaceInline("|", "/");
            Item.Tooltip  = Tooltip;
            Actions.Add(FGraphAction{ NodeClass, Variant });
        };

        for (CClass* NodeClass : Graph->GetSupportedNodes())
        {
            CEdGraphNode* CDO = Cast<CEdGraphNode>(NodeClass->GetDefaultObject());
            if (CDO == nullptr)
            {
                continue;
            }

            if (bHasSourceFilter && bOnlyAcceptingNodes && !Accepting.Contains(NodeClass))
            {
                continue;
            }

            TVector<FGraphNodeVariant> Variants;
            CDO->GetVariants(Variants);
            if (Variants.IsEmpty())
            {
                AddItem(NodeClass, FName(), FString(CDO->GetNodeDisplayName()), FString(*CDO->GetNodeCategory()), FString(CDO->GetNodeTooltip()));
                continue;
            }

            for (const FGraphNodeVariant& Variant : Variants)
            {
                AddItem(NodeClass, Variant.Name, Variant.DisplayName, Variant.Category, Variant.Tooltip);
            }
        }
    }

    bool FGraphActionMenu::Draw(CEdNodeGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return false;
        }

        // A menu opened from a dragged wire says what it connects to, and can widen to every node.
        if (Graph->PendingSourcePin != nullptr)
        {
            ImGui::TextDisabled("Connect to %s", *Graph->PendingSourcePin->GetPinName());
            ImGui::SameLine();
            if (ImGui::Checkbox("Only nodes that take it", &bOnlyAcceptingNodes))
            {
                Picker.Invalidate();
            }
        }

        const FPickerItem* Picked = Picker.DrawContents([this, Graph](TVector<FPickerItem>& OutItems) { GatherItems(Graph, OutItems); });
        if (Picked == nullptr)
        {
            return false;
        }

        const int32 ActionIndex = static_cast<int32>(std::strtol(*Picked->Key, nullptr, 10));
        if (ActionIndex < 0 || ActionIndex >= Actions.Num())
        {
            return false;
        }

        const FGraphAction& Action = Actions[ActionIndex];
        CEdGraphNode* NewNode = Graph->CreateNode(Action.NodeClass, Action.Variant);
        if (NewNode != nullptr)
        {
            Graph->PlaceNodeAtScreen(NewNode, ImGui::GetMousePosOnOpeningCurrentPopup());

            if (Graph->PendingSourcePin != nullptr)
            {
                if (CEdNodeGraphPin* TargetPin = Graph->FindAutoConnectPin(NewNode, Graph->PendingSourcePin))
                {
                    Graph->TryAutoConnect(Graph->PendingSourcePin, TargetPin);
                }
            }

            Graph->GetEditor().SelectNode(NewNode->GetNodeID(), false);

            if (Graph->NodeSelectedCallback)
            {
                Graph->NodeSelectedCallback(NewNode);
            }
        }
        Graph->PendingSourcePin = nullptr;
        return true;
    }
}
