#pragma once

#include "Core/Object/Cast.h"
#include "MaterialNodeExpression.h"
#include "UI/Tools/NodeGraph/Material/MaterialInput.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

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

    /**
     * Adds a NAMED output pin to an expression node.
     *
     * For nodes publishing several outputs, which therefore skip Super::BuildNode (that creates the
     * single unnamed Output) and bind each pin individually through ResolvedVar.
     *
     * Lives here for the same reason MakeIn does: a file-scope copy per node .cpp is fine until a
     * unity build puts two of them in one translation unit and they collide.
     */
    inline CMaterialOutput* MakeOut(CMaterialExpression* Self, const char* Name, EMaterialInputType Type)
    {
        CMaterialOutput* Pin = Cast<CMaterialOutput>(
            Self->CreatePin(CMaterialOutput::StaticClass(), Name, ENodePinDirection::Output));
        Pin->SetPinName(Name);
        Pin->SetShouldDrawEditor(true);
        Pin->SetHideDuringConnection(false);
        Pin->SetInputType(Type);
        Pin->SetComponentMask(EComponentMask::None);
        return Pin;
    }
}
