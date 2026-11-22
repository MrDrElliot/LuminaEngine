#pragma once

#include "AnimGraphPin.h"
#include "Animation/AnimationGraphVM.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "AnimGraphNode.generated.h"

namespace Lumina
{
    class FAnimationGraphCompiler;
    struct FAnimGraphDebugView;

    // Pins are rebuilt by BuildNode and not serialized, so a value pin's inline default persists here by pin name.
    REFLECT()
    struct SAnimGraphPinDefault
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName PinName;

        PROPERTY(Hidden)
        float Value = 0.0f;
    };

    // NotPlaceable does not inherit, so every concrete anim node stays discoverable.
    REFLECT(NotPlaceable, Abstract)
    class EDITOR_API CAnimGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()
    public:

        // Every anim node belongs to the blend-tree graph except the two state-machine canvas nodes.
        CClass* GetSupportedGraphClass() const override;

        virtual void GenerateBytecode(FAnimationGraphCompiler& Compiler) = 0;

        FFixedString GetNodeCategory() const override { return "Animation"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(60, 110, 70, 255); }

        // The saved per-node override if one exists, else the pin's builtin DefaultValue.
        float GetValuePinDefault(const CAnimGraphPin* Pin) const;
        void  SetValuePinDefault(const CAnimGraphPin* Pin, float Value);

        // A value input's live value when something runs the graph, else the default it compiles to.
        float ReadValueInput(const CAnimGraphPin* Pin, const FAnimGraphDebugView* View) const;

        // Share of this node's own weight a pose input passes on, read from live alphas. All of it by default.
        virtual float GetPoseInputShare(const CEdNodeGraphPin* PoseInput, const FAnimGraphDebugView& View) const { return 1.0f; }

        // A picture under the pins, opened from the node's chevron. View is null when nothing runs the graph.
        virtual bool HasPreview() const;
        virtual void DrawPreview(const FAnimGraphDebugView* View);

        /** Persisted inline defaults for value pins, keyed by pin name. */
        PROPERTY(Hidden)
        TVector<SAnimGraphPinDefault> PinDefaults;

    protected:

        const CAnimGraphPin* FindFirstValueOutput() const;

        // Creates a typed animation pin already cast to CAnimGraphPin.
        CAnimGraphPin* CreateAnimPin(const FString& Name, ENodePinDirection Direction, EAnimPinType Type, float DefaultValue = 0.0f);

        // Binds an inline DragFloat that edits the node's saved default for the pin.
        void BindFloatPinEditor(CAnimGraphPin* Pin, float Speed = 0.01f, const char* Format = "%.2f");

        // Items are the enum's display names in value order, and the backing property stays the source of truth.
        void BindEnumPinEditor(CAnimGraphPin* Pin, const TVector<const char*>& Items,
                               const TFunction<int()>& Get, const TFunction<void(int)>& Set);

        // Marks the owning asset package dirty after an inline edit completes.
        void MarkGraphDirty();

        // Emits a bind-pose RefPose when the pin is unconnected.
        static uint16 ResolvePoseInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler);

        // Emits a LoadConst of the pin's DefaultValue when the pin is unconnected.
        static uint16 ResolveValueInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler);

        // Object register feeding InputPin, or Constants::kIndexNone when the pin is unconnected.
        static int32 ResolveObjectInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler);

        // ResolveValueInput reshaped by Easing, for the nodes that expose an alpha pin.
        static uint16 ResolveAlphaInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler,
            EAnimAlphaEasing Easing);
    };
}
