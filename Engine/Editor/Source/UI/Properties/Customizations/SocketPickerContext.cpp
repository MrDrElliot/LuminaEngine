#include "SocketPickerContext.h"

namespace Lumina::SocketPickerContext
{
    namespace
    {
        TVector<const FSocketPickerData*>& GetStack()
        {
            static TVector<const FSocketPickerData*> Stack;
            return Stack;
        }
    }

    void Push(const FSocketPickerData* Data)
    {
        GetStack().push_back(Data);
    }

    void Pop()
    {
        auto& Stack = GetStack();
        if (!Stack.empty())
        {
            Stack.pop_back();
        }
    }

    const FSocketPickerData* GetActive()
    {
        const auto& Stack = GetStack();
        return Stack.empty() ? nullptr : Stack.back();
    }
}
