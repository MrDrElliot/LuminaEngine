#pragma once

#include "AnimGraphPin.h"
#include "Animation/AnimationGraphVM.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "AnimGraphNode.generated.h"

namespace Lumina
{
    class FAnimationGraphCompiler;
    struct FAnimGraphDebugView;

    // Persisted per-pin default. Pins are rebuilt by BuildNode() and not serialized, so a value pin's
    // editable inline default lives here, keyed by pin name.
    REFLECT()
    struct SAnimGraphPinDefault
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName PinName;

        PROPERTY(Hidden)
        float Value = 0.0f;
    };

    // Base class for animation node-graph nodes: subclasses build pins in BuildNode() and emit bytecode
    // in GenerateBytecode(); the graph walks them in topological order during compile.
    // NotPlaceable: family base, not a node. The specifier does not inherit, so every concrete
    // anim node below stays discoverable.
    REFLECT(NotPlaceable)
    class EDITOR_API CAnimGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()
    public:

        // Every anim node belongs to the blend-tree graph; the two state-machine-canvas nodes
        // override this to say otherwise.
        CClass* GetSupportedGraphClass() const override;

        virtual void GenerateBytecode(FAnimationGraphCompiler& Compiler) { UNREACHABLE(); }

        FFixedString GetNodeCategory() const override { return "Animation"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(60, 110, 70, 255); }

        // Effective default for an unconnected value pin: the per-node saved
        // override if one exists, else the pin's builtin DefaultValue.
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

        // Creates a typed animation pin, sets its name / color / default, and
        // returns it already cast to CAnimGraphPin.
        CAnimGraphPin* CreateAnimPin(const FString& Name, ENodePinDirection Direction, EAnimPinType Type, float DefaultValue = 0.0f);

        // Binds a compact DragFloat inline editor to a value input pin; edits the
        // node's saved default for that pin (see PinDefaults).
        void BindFloatPinEditor(CAnimGraphPin* Pin, float Speed = 0.01f, const char* Format = "%.2f");

        // Binds a combo inline editor to an enum-backed value input pin (Items are the enum's display names
        // in value order). The backing enum property stays the source of truth; the pin is an optional override.
        void BindEnumPinEditor(CAnimGraphPin* Pin, const TVector<const char*>& Items,
                               const TFunction<int()>& Get, const TFunction<void(int)>& Set);

        // Marks the owning asset package dirty after an inline edit completes.
        void MarkGraphDirty();

        // Resolves the pose register feeding InputPin, or emits a bind-pose
        // RefPose when the pin is unconnected.
        static uint16 ResolvePoseInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler);

        // Resolves the scalar register feeding InputPin, or emits a LoadConst of
        // the pin's DefaultValue when the pin is unconnected.
        static uint16 ResolveValueInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler);

        // Object register feeding InputPin, or Constants::kIndexNone when the pin is unconnected.
        static int32 ResolveObjectInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler);

        // ResolveValueInput reshaped by Easing, for the nodes that expose an alpha pin.
        static uint16 ResolveAlphaInput(CEdNodeGraphPin* InputPin, FAnimationGraphCompiler& Compiler,
            EAnimAlphaEasing Easing);
    };
}
