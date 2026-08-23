#include "AnimGraphNode_CachedPose.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    void CAnimGraphNode_SaveCachedPose::BuildNode()
    {
        PoseInputPin = CreateAnimPin("Pose", ENodePinDirection::Input, EAnimPinType::Pose);
    }

    void CAnimGraphNode_SaveCachedPose::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        // Publishes the register itself, so a reader aliases this evaluation instead of repeating it.
        Compiler.SetCachedPose(CacheName, ResolvePoseInput(PoseInputPin, Compiler));
    }

    FString CAnimGraphNode_SaveCachedPose::GetNodeTitleText() const
    {
        return CacheName.IsNone() ? FString("Save Cached Pose") : Format("Save {}", CacheName.c_str());
    }

    void CAnimGraphNode_UseCachedPose::BuildNode()
    {
        PosePin = CreateAnimPin("Pose", ENodePinDirection::Output, EAnimPinType::Pose);
    }

    void CAnimGraphNode_UseCachedPose::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        uint16 CachedRegister = 0;
        if (!Compiler.TryGetCachedPose(CacheName, CachedRegister))
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Missing Cached Pose";
            NodeError.Description = "Use Cached Pose names a cache no Save Cached Pose node writes; it will read the reference pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);

            Compiler.SetPinRegister(PosePin, Compiler.EmitRefPose());
            return;
        }

        Compiler.SetPinRegister(PosePin, CachedRegister);
    }

    FString CAnimGraphNode_UseCachedPose::GetNodeTitleText() const
    {
        return CacheName.IsNone() ? FString("Use Cached Pose") : FString(CacheName.c_str());
    }
}
