#include <gtest/gtest.h>

#include "Containers/Format.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Lumina.h"
#include "Renderer/MaterialTypes.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

using namespace Lumina;

namespace
{
    FName IndexedName(const char* Prefix, uint32 Index)
    {
        return FName(Format("{}{}", Prefix, Index));
    }

    TVector<FMaterialParameter> ExportedParameters(const FMaterialCompiler& Compiler)
    {
        TVector<FMaterialParameter> Params;
        FMaterialUniforms Uniforms{};
        Compiler.GetParameters(Params, Uniforms);
        return Params;
    }
}

TEST(MaterialParameterBudget, TheLastScalarThatFitsIsAccepted)
{
    FMaterialCompiler Compiler;
    for (uint32 i = 0; i < MAX_SCALARS; ++i)
    {
        Compiler.DefineFloatParameter(Format("Node{}", i), IndexedName("Scalar", i), (float)i);
    }

    EXPECT_FALSE(Compiler.HasErrors());
    EXPECT_EQ(ExportedParameters(Compiler).size(), (size_t)MAX_SCALARS);
}

TEST(MaterialParameterBudget, AScalarPastTheBudgetIsRefusedRatherThanClamped)
{
    FMaterialCompiler Compiler;
    for (uint32 i = 0; i < MAX_SCALARS + 4; ++i)
    {
        Compiler.DefineFloatParameter(Format("Node{}", i), IndexedName("Scalar", i), (float)i);
    }

    EXPECT_TRUE(Compiler.HasErrors());
    EXPECT_EQ(Compiler.GetErrors().size(), (size_t)4);

    const TVector<FMaterialParameter> Params = ExportedParameters(Compiler);
    EXPECT_EQ(Params.size(), (size_t)MAX_SCALARS);
    for (const FMaterialParameter& Param : Params)
    {
        EXPECT_LT(Param.Index, MAX_SCALARS);
    }
}

TEST(MaterialParameterBudget, ARefusedScalarStillDeclaresItsVariable)
{
    FMaterialCompiler Compiler;
    for (uint32 i = 0; i < MAX_SCALARS; ++i)
    {
        Compiler.DefineFloatParameter(Format("Node{}", i), IndexedName("Scalar", i), (float)i);
    }

    const uint32 LinesBefore = Compiler.GetStats().PixelInstructions;
    Compiler.DefineFloatParameter("OverBudgetNode", "OverBudget", 0.5f);

    // Downstream nodes read it by name, so a refused slot must still leave a declared local behind.
    EXPECT_GT(Compiler.GetStats().PixelInstructions, LinesBefore);
}

TEST(MaterialParameterBudget, AVectorPastTheBudgetIsRefusedRatherThanClamped)
{
    FMaterialCompiler Compiler;
    float Value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (uint32 i = 0; i < MAX_VECTORS + 4; ++i)
    {
        Compiler.DefineFloat4Parameter(Format("Node{}", i), IndexedName("Vector", i), Value);
    }

    EXPECT_TRUE(Compiler.HasErrors());

    const TVector<FMaterialParameter> Params = ExportedParameters(Compiler);
    EXPECT_EQ(Params.size(), (size_t)MAX_VECTORS);
    for (const FMaterialParameter& Param : Params)
    {
        EXPECT_LT(Param.Index, MAX_VECTORS);
    }
}

TEST(MaterialParameterBudget, ATexturePastTheBudgetBindsNoSlot)
{
    FMaterialCompiler Compiler;
    for (uint32 i = 0; i < MAX_TEXTURES; ++i)
    {
        EXPECT_EQ(Compiler.BindTextureParameter(IndexedName("Texture", i), nullptr), (int32)i);
    }

    EXPECT_FALSE(Compiler.HasErrors());
    EXPECT_EQ(Compiler.BindTextureParameter("OverBudget", nullptr), INDEX_NONE);
    EXPECT_TRUE(Compiler.HasErrors());

    const TVector<FMaterialParameter> Params = ExportedParameters(Compiler);
    EXPECT_EQ(Params.size(), (size_t)MAX_TEXTURES);
    for (const FMaterialParameter& Param : Params)
    {
        EXPECT_LT(Param.Index, MAX_TEXTURES);
    }
}

TEST(MaterialParameterBudget, ARepeatedParameterNameReusesItsSlot)
{
    FMaterialCompiler Compiler;
    for (uint32 i = 0; i < MAX_SCALARS * 2; ++i)
    {
        Compiler.DefineFloatParameter(Format("Node{}", i), "Shared", 1.0f);
    }

    EXPECT_FALSE(Compiler.HasErrors());
    EXPECT_EQ(ExportedParameters(Compiler).size(), (size_t)1);
}
