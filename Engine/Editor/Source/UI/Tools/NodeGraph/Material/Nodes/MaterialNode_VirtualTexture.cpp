#include "MaterialNode_VirtualTexture.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{
    void CMaterialExpression_VirtualTextureSample::BuildNode()
    {
        Position = MakeIn(this, "Position", EMaterialInputType::Float3);

        RGBAOut  = MakeOut(this, "RGBA",  EMaterialInputType::Float4);
        RGBOut   = MakeOut(this, "RGB",   EMaterialInputType::Float3);
        AlphaOut = MakeOut(this, "A",     EMaterialInputType::Float);
        ValidOut = MakeOut(this, "Valid", EMaterialInputType::Float);
    }

    void CMaterialExpression_VirtualTextureSample::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.VirtualTextureSample(this, Texture.Get(), Position, bSampleAtObjectOrigin, RGBAOut, RGBOut, AlphaOut, ValidOut);
    }
}
