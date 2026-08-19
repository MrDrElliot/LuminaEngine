#include "BonePickerContext.h"

#include "Containers/Vector.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina::BonePickerContext
{
    namespace
    {
        TVector<const FSkeletonResource*>& GetStack()
        {
            static TVector<const FSkeletonResource*> Stack;
            return Stack;
        }
    }

    void PushSkeleton(const FSkeletonResource* Skeleton)
    {
        GetStack().push_back(Skeleton);
    }

    void PopSkeleton()
    {
        auto& Stack = GetStack();
        if (!Stack.empty())
        {
            Stack.pop_back();
        }
    }

    const FSkeletonResource* GetActiveSkeleton()
    {
        const auto& Stack = GetStack();
        return Stack.empty() ? nullptr : Stack.back();
    }
}
