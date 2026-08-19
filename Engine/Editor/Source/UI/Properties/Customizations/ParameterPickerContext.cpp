#include "ParameterPickerContext.h"

#include "Containers/Vector.h"

namespace Lumina::ParameterPickerContext
{
    namespace
    {
        TVector<CAnimationGraph*>& GetStack()
        {
            static TVector<CAnimationGraph*> Stack;
            return Stack;
        }
    }

    void PushGraph(CAnimationGraph* Graph)
    {
        GetStack().push_back(Graph);
    }

    void PopGraph()
    {
        auto& Stack = GetStack();
        if (!Stack.empty())
        {
            Stack.pop_back();
        }
    }

    CAnimationGraph* GetActiveGraph()
    {
        const auto& Stack = GetStack();
        return Stack.empty() ? nullptr : Stack.back();
    }
}
