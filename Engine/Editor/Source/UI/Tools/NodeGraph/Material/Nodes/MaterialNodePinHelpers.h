#pragma once

#include "Core/Object/Cast.h"
#include "MaterialNodeExpression.h"
#include "UI/Tools/NodeGraph/Material/MaterialInput.h"

namespace Lumina
{
    /**
     * Adds a named input pin to an expression node.
     *
     * Inline in a header rather than copied into each MaterialNode_*.cpp: identical file-scope
     * copies compile fine one translation unit at a time, but collide the moment those files share
     * one, which is what a unity build makes them do.
     */
    inline CMaterialInput* MakeIn(
        CMaterialExpression* Self, const char* Name, EMaterialInputType Type = EMaterialInputType::Float)
    {
        CMaterialInput* P = Cast<CMaterialInput>(Self->CreatePin(CMaterialInput::StaticClass(), Name, ENodePinDirection::Input));
        P->SetPinName(Name);
        P->SetInputType(Type);
        return P;
    }
}
