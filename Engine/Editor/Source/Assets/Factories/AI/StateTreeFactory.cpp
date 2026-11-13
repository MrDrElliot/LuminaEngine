#include "EditorPCH.h"
#include "StateTreeFactory.h"

#include "Core/Math/Math.h"
#include "Thumbnails/AssetTilePainters.h"
#include "Tools/UI/ImGui/EditorColors.h"

namespace Lumina
{
    namespace
    {
        // The compiled states as indented bars, the way the editor's outline lays them out.
        void PaintStateTreeTile(CObject* Asset, ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Max)
        {
            const TSharedPtr<const FStateTreeRuntime> Runtime = static_cast<CStateTree*>(Asset)->GetRuntime();
            const float Padding = (Max.x - Min.x) * 0.12f;
            const float RowHeight = (Max.y - Min.y - Padding * 2.0f) / 8.0f;
            const float Indent = RowHeight * 0.8f;
            for (int32 Index = 0; Runtime != nullptr && Index < Math::Min(Runtime->States.Num(), 8); ++Index)
            {
                const FStateTreeRuntimeState& State = Runtime->States[Index];
                const ImVec4 Color = State.Kind == EStateTreeStateKind::State ? EditorColors::Accent() : State.Kind == EStateTreeStateKind::Group ? EditorColors::AccentAlt() : EditorColors::Warning();
                const ImVec2 BarMin(Min.x + Padding + State.Depth * Indent, Min.y + Padding + Index * RowHeight);
                const ImVec2 BarMax(Max.x - Padding, BarMin.y + RowHeight * 0.7f);
                DrawList.AddRectFilled(BarMin, BarMax, EditorColors::U32(EditorColors::WithAlpha(Color, 0.75f)), RowHeight * 0.25f);
            }
        }
    }

    void CStateTreeFactory::PostCreateCDO()
    {
        Super::PostCreateCDO();
        FAssetTilePainterRegistry::Get().Register(CStateTree::StaticClass(), &PaintStateTreeTile);
    }

    CObject* CStateTreeFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // Starts with one state, since a tree with none does not compile.
        CStateTree* Tree = NewObject<CStateTree>(Package, Name);
        Tree->States.AddDefaultedGetRef().Name = FName("Root");
        Tree->Compile();
        return Tree;
    }
}
