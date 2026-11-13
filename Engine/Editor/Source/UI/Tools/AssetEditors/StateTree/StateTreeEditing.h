#pragma once

#include "AI/StateTree/StateTree.h"
#include "UI/Widgets/CategorizedPicker.h"

namespace Lumina
{
    class FProperty;

    enum class EStateTreeSlot : uint8
    {
        // The state itself, or nothing when no state is set.
        State,
        EnterCondition,
        Task,
        Transition,
        TransitionCondition,
        Evaluator,
    };

    // What the outline, the details and an issue point at, a node by its place in the list Slot names.
    struct FStateTreeSelection
    {
        int32          State      = Constants::kIndexNone;
        EStateTreeSlot Slot       = EStateTreeSlot::State;
        int32          Transition = Constants::kIndexNone;
        int32          Index      = Constants::kIndexNone;

        bool operator==(const FStateTreeSelection&) const = default;

        NODISCARD bool IsNode() const { return Slot != EStateTreeSlot::State && Slot != EStateTreeSlot::Transition; }
    };

    struct FStateTreeIssue
    {
        FString             Message;
        FStateTreeSelection Target;
        bool                bError = true;
    };

    // One source the binding picker offers, already filtered to the type of the field it binds.
    struct FStateTreeBindingSource
    {
        SStateTreeBindingPath Path;
        FPickerItem           Item;
    };

    // Edits on the authored tree that keep names, parents and transition targets consistent; the caller marks the asset dirty.
    namespace StateTreeEditing
    {
        EDITOR_API FName MakeUniqueStateName(const CStateTree& Tree, FName Base);

        // Inserts at InsertAt, or after every state when it is kIndexNone, and returns the new index.
        EDITOR_API int32 AddState(CStateTree& Tree, FName Parent, int32 InsertAt = Constants::kIndexNone);

        // Renames the state and every parent and transition naming it, leaving the tree alone when NewName is taken.
        EDITOR_API bool RenameState(CStateTree& Tree, int32 Index, FName NewName);

        // The states Index runs below, nearest first.
        NODISCARD EDITOR_API TVector<int32> GatherAncestors(const CStateTree& Tree, int32 Index);

        NODISCARD EDITOR_API bool IsInSubtree(const CStateTree& Tree, int32 Index, int32 Root);

        // Root first, then its descendants in list order.
        NODISCARD EDITOR_API TVector<int32> GatherSubtree(const CStateTree& Tree, int32 Root);

        // Reparents and reorders, refusing a parent inside the moved subtree, and returns the state's new index.
        EDITOR_API int32 MoveState(CStateTree& Tree, int32 Index, FName NewParent, int32 InsertBefore);

        EDITOR_API void DeleteState(CStateTree& Tree, int32 Index);

        NODISCARD EDITOR_API TVector<SStateTreeState> CopySubtree(const CStateTree& Tree, int32 Root);

        // Renames the copies where their names are taken, keeping their own parents and targets pointing at each other.
        EDITOR_API int32 PasteSubtree(CStateTree& Tree, TVector<SStateTreeState> States, FName Parent, int32 InsertAt);

        EDITOR_API int32 DuplicateState(CStateTree& Tree, int32 Index);

        // Moves the state's descendants into Into as its top level, turning the state into a link to it.
        EDITOR_API void ExtractToLinkedSubtree(CStateTree& Tree, int32 Index, CStateTree& Into);

        NODISCARD EDITOR_API FInstancedStruct* FindNode(CStateTree& Tree, const FStateTreeSelection& Node);
        NODISCARD EDITOR_API CStruct* GetNodeBaseStruct(EStateTreeSlot Slot);

        // Adds to the list Node names at Node.Index, or at its end, and returns where it landed.
        EDITOR_API int32 InsertNode(CStateTree& Tree, const FStateTreeSelection& Node, const FInstancedStruct& Value);
        EDITOR_API int32 AddNodeOfType(CStateTree& Tree, const FStateTreeSelection& Node, CStruct* Type);
        EDITOR_API void RemoveNode(CStateTree& Tree, const FStateTreeSelection& Node);

        // The name a binding reads and the type it writes into, null when the selection takes no bindings.
        NODISCARD EDITOR_API TVector<SStateTreeBinding>* FindBindings(CStateTree& Tree, const FStateTreeSelection& Reader, CStruct*& OutTargetType);

        // Every source visible from Reader whose value Target can take, the same visibility and type rules the compiler applies.
        EDITOR_API void GatherBindingSources(const CStateTree& Tree, const FStateTreeSelection& Reader, const FProperty& Target, bool bAllowFunctions, TVector<FStateTreeBindingSource>& Out);

        // Points TargetField at Path, with one unbound argument per parameter when Path calls a function.
        EDITOR_API SStateTreeBinding& SetBinding(const CStateTree& Tree, TVector<SStateTreeBinding>& Bindings, FName TargetField, const SStateTreeBindingPath& Path);

        // The function a binding calls, null when it reads a field.
        NODISCARD EDITOR_API const FFunction* FindBoundFunction(const CStateTree& Tree, const SStateTreeBindingPath& Path);

        NODISCARD EDITOR_API FString DescribeBinding(const SStateTreeBindingPath& Path);

        // Compiles, then lists every compile error and editor warning with the item it is about.
        EDITOR_API TVector<FStateTreeIssue> Validate(CStateTree& Tree);
    }
}
