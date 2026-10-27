#include "MaterialNode_Formula.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{
    namespace
    {
        constexpr FMaterialFormulaPin kUV       = { "UV", 2, 0.0f, "UV0" };
        constexpr FMaterialFormulaPin kCenter   = { "Center", 2, 0.5f };
        constexpr FMaterialFormulaPin kSoftness = { "Softness", 1, 0.01f };
        constexpr FMaterialFormulaPin kTime     = { "Time", 1, 0.0f, "GetTime()" };
        constexpr FMaterialFormulaPin kPosition = { "Position", 3, 0.0f, "WorldPosition" };
        constexpr FMaterialFormulaPin kNormal   = { "Normal", 3, 0.0f, "WorldNormal.xyz" };
        constexpr FMaterialFormulaPin kTangentNormal = { "Normal", 3, 0.0f, "float3(0.0, 0.0, 1.0)" };
        constexpr FMaterialFormulaPin kColor    = { "Color", 3, 1.0f };
        constexpr FMaterialFormulaPin kBlendPins[] = { { "Base", 0, 0.5f }, { "Blend", 0, 0.5f }, { "Opacity", 1, 1.0f } };
        constexpr FMaterialFormulaPin kWavePins[] = { kTime, { "Frequency", 1, 1.0f }, { "Amplitude", 1, 1.0f }, { "Phase", 1, 0.0f } };
        constexpr FMaterialFormulaPin kFractalPins[] = { kUV, { "Scale", 1, 8.0f }, { "Octaves", 1, 5.0f }, { "Lacunarity", 1, 2.0f }, { "Gain", 1, 0.5f } };
        constexpr FMaterialFormulaPin kEasePins[] = { { "X", 0, 0.5f }, { "Power", 1, 2.0f } };

        constexpr EMaterialInputType TypeForWidth(int32 Width)
        {
            return Width == 2 ? EMaterialInputType::Float2
                 : Width == 3 ? EMaterialInputType::Float3
                 : Width == 4 ? EMaterialInputType::Float4
                 : EMaterialInputType::Float;
        }
    }

// An integer hash of the four corners, since a sin hash of a reordered dot product disagrees between neighboring cells.
#define LUMINA_HASH_CORNERS_2D \
    "int2 $_C = int2(floor($_P));\n" \
    "uint4 $_S = uint4(uint($_C.x), uint($_C.x + 1), uint($_C.x), uint($_C.x + 1)) * 1597334673u " \
    "^ uint4(uint($_C.y), uint($_C.y), uint($_C.y + 1), uint($_C.y + 1)) * 3812015801u;\n" \
    "$_S = ($_S ^ ($_S >> 16u)) * 2246822519u;\n" \
    "$_S = $_S ^ ($_S >> 13u);\n"

// Smooth value noise at the float2 named $_P, written to the float lvalue Out.
#define LUMINA_VALUE_NOISE_2D(Out) \
    LUMINA_HASH_CORNERS_2D \
    "float4 $_H = float4($_S) * (1.0 / 4294967296.0);\n" \
    "float2 $_F = frac($_P);\n" \
    "float2 $_U = $_F * $_F * (3.0 - 2.0 * $_F);\n" \
    Out " = lerp(lerp($_H.x, $_H.y, $_U.x), lerp($_H.z, $_H.w, $_U.x), $_U.y);\n"

// The 2D fractals differ only in how each octave's noise $_N is shaped before it is summed.
#define LUMINA_FRACTAL_NOISE_2D(Shape) \
    "float2 $_P = {UV} * {Scale};\n" \
    "float $_Sum = 0.0;\n" \
    "float $_Amp = 1.0;\n" \
    "float $_Norm = 0.0;\n" \
    "int $_Count = clamp(int({Octaves}), 1, 10);\n" \
    "for (int $_I = 0; $_I < $_Count; ++$_I)\n" \
    "{\n" \
    "float $_N;\n" \
    LUMINA_VALUE_NOISE_2D("$_N") \
    "$_Sum += $_Amp * (" Shape ");\n" \
    "$_Norm += $_Amp;\n" \
    "$_Amp *= {Gain};\n" \
    "$_P = $_P * {Lacunarity} + float2(17.31, 9.17);\n" \
    "}\n" \
    "float $ = $_Sum / max($_Norm, 1e-6);"

// A shape's signed distance $_D, negative inside, made a mask with an edge Softness wide.
#define LUMINA_SHAPE_EDGE "\nfloat $ = saturate(0.5 - $_D / max({Softness}, 1e-5));"

    FString FormulaVectorType(int32 Width)
    {
        switch (Width)
        {
        case 2:  return "float2";
        case 3:  return "float3";
        case 4:  return "float4";
        default: return "float";
        }
    }

    FString ReadFormulaInput(const FString& Expr, int32 Width, int32 Target)
    {
        if (Width == Target)
        {
            return "(" + Expr + ")";
        }
        if (Width == 1)
        {
            return FormulaVectorType(Target) + "(" + Expr + ")";
        }
        if (Width > Target)
        {
            return "(" + Expr + ")." + FString("xyzw").substr(0, Target);
        }

        FString Padded = FormulaVectorType(Target) + "(" + Expr;
        for (int32 Component = Width; Component < Target; ++Component)
        {
            Padded += ", 0.0";
        }
        return Padded + ")";
    }

    FString ExpandFormula(const FMaterialFormula& Formula, const FString& Prefix, TSpan<const TPair<FString, int32>> Inputs, int32 SharedWidth)
    {
        // A placeholder is {Name} with nothing else inside the braces, so a block's braces pass through untouched.
        const std::string_view Code = Formula.Code;
        FString Expanded;
        for (size_t Cursor = 0; Cursor < Code.size(); ++Cursor)
        {
            if (Code[Cursor] == '$')
            {
                Expanded += Prefix;
                continue;
            }

            const size_t Close = Code[Cursor] == '{' ? Code.find('}', Cursor) : std::string_view::npos;
            const std::string_view Name = Close != std::string_view::npos ? Code.substr(Cursor + 1, Close - Cursor - 1) : std::string_view();
            if (Name == "T")
            {
                Expanded += FormulaVectorType(SharedWidth);
                Cursor = Close;
                continue;
            }

            bool bReplaced = false;
            for (size_t Index = 0; Index < Inputs.size() && Index < Formula.Inputs.size() && !Name.empty(); ++Index)
            {
                if (Name == Formula.Inputs[Index].Name)
                {
                    const int32 Target = Formula.Inputs[Index].Width == 0 ? SharedWidth : Formula.Inputs[Index].Width;
                    Expanded += ReadFormulaInput(Inputs[Index].first, Inputs[Index].second, Target);
                    Cursor    = Close;
                    bReplaced = true;
                    break;
                }
            }
            if (!bReplaced)
            {
                Expanded += Code[Cursor];
            }
        }
        return Expanded;
    }

    const FMaterialFormula& CMaterialExpression_Formula::GetFormula() const
    {
        static constexpr FMaterialFormula Empty = { "Formula", "Math", "", TSpan<const FMaterialFormulaPin>(), 1, "float $ = 0.0;" };
        return Empty;
    }

    void CMaterialExpression_Formula::BuildNode()
    {
        const FMaterialFormula& Formula = GetFormula();
        if (Formula.Outputs.empty())
        {
            Super::BuildNode();
            Output->SetInputType(TypeForWidth(Formula.Width));
        }
        for (const FMaterialFormulaPin& Pin : Formula.Outputs)
        {
            MakeOut(this, Pin.Name, TypeForWidth(Pin.Width));
        }

        // Loaded nodes rebuild their pins after their saved values arrive, so only missing values take defaults.
        const size_t Saved = Values.size();
        Values.resize(Formula.Inputs.size());
        for (size_t Index = 0; Index < Formula.Inputs.size(); ++Index)
        {
            const FMaterialFormulaPin& Pin = Formula.Inputs[Index];
            CMaterialInput* Input = MakeIn(this, Pin.Name, Pin.Fallback != nullptr ? TypeForWidth(Pin.Width) : EMaterialInputType::Float);
            Input->SetIndex((uint32)Index);
            Input->SetShouldDrawEditor(Pin.Fallback == nullptr);
            if (Index >= Saved)
            {
                Values[Index] = Pin.Default;
            }
        }
    }

    void CMaterialExpression_Formula::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.EmitFormula(this, GetFormula(), Values);
    }

    const FMaterialFormula& CMaterialExpression_InverseLerp::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "A", 0, 0.0f }, { "B", 0, 1.0f }, { "X", 0, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Inverse Lerp", "Math", "Where X sits between A and B, 0 at A and 1 at B.", Inputs, 0,
            "{T} $_D = {B} - {A};\n"
            "{T} $ = ({X} - {A}) * $_D / max($_D * $_D, {T}(1e-12));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_RemapClamped::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.5f }, { "InMin", 0, 0.0f }, { "InMax", 0, 1.0f }, { "OutMin", 0, 0.0f }, { "OutMax", 0, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Remap Clamped", "Math", "Remaps X from the In range to the Out range, holding it inside the Out range.", Inputs, 0,
            "{T} $_D = {InMax} - {InMin};\n"
            "{T} $ = lerp({OutMin}, {OutMax}, saturate(({X} - {InMin}) * $_D / max($_D * $_D, {T}(1e-12))));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SmoothMin::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "A", 0, 0.0f }, { "B", 0, 1.0f }, { "Smoothness", 1, 0.1f } };
        static constexpr FMaterialFormula Formula = { "Smooth Min", "Math", "The smaller of A and B, rounded off where they meet.", Inputs, 0,
            "{T} $_H = saturate(0.5 + 0.5 * ({B} - {A}) / max({Smoothness}, 1e-6));\n"
            "{T} $ = lerp({B}, {A}, $_H) - {Smoothness} * $_H * (1.0 - $_H);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SmoothMax::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "A", 0, 0.0f }, { "B", 0, 1.0f }, { "Smoothness", 1, 0.1f } };
        static constexpr FMaterialFormula Formula = { "Smooth Max", "Math", "The larger of A and B, rounded off where they meet.", Inputs, 0,
            "{T} $_H = saturate(0.5 - 0.5 * ({B} - {A}) / max({Smoothness}, 1e-6));\n"
            "{T} $ = lerp({B}, {A}, $_H) + {Smoothness} * $_H * (1.0 - $_H);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_ConstantBiasScale::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.0f }, { "Bias", 1, 1.0f }, { "Scale", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Constant Bias Scale", "Math", "(X + Bias) * Scale, which by default maps -1..1 to 0..1.", Inputs, 0,
            "{T} $ = ({X} + {Bias}) * {Scale};" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Snap::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.0f }, { "Step", 0, 0.25f } };
        static constexpr FMaterialFormula Formula = { "Snap", "Math", "Rounds X to the nearest multiple of Step.", Inputs, 0,
            "{T} $ = round({X} / max(abs({Step}), {T}(1e-6))) * {Step};" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Wrap::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.0f }, { "Min", 0, 0.0f }, { "Max", 0, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Wrap", "Math", "Wraps X around so it repeats inside Min..Max.", Inputs, 0,
            "{T} $_R = max({Max} - {Min}, {T}(1e-6));\n"
            "{T} $ = {Min} + ({X} - {Min}) - $_R * floor(({X} - {Min}) / $_R);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_PingPong::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.0f }, { "Length", 0, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Ping Pong", "Math", "Bounces X back and forth between 0 and Length.", Inputs, 0,
            "{T} $_L = max({Length}, {T}(1e-6));\n"
            "{T} $ = $_L - abs(frac({X} / (2.0 * $_L)) * 2.0 * $_L - $_L);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Lerp3::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "A", 0, 0.0f }, { "B", 0, 0.5f }, { "C", 0, 1.0f }, { "Alpha", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Lerp 3", "Math", "Blends A to B over the first half of Alpha and B to C over the second, such as a three color gradient.", Inputs, 0,
            "{T} $ = lerp(lerp({A}, {B}, saturate({Alpha} * 2.0)), {C}, saturate({Alpha} * 2.0 - 1.0));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_DistanceSquared::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kPosition, { "B", 3, 0.0f } };
        static constexpr FMaterialFormula Formula = { "Distance Squared", "Math", "The squared distance between two points, cheaper than Distance when only comparing.", Inputs, 1,
            "float3 $_D = {Position} - {B};\n"
            "float $ = dot($_D, $_D);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Bias::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.5f }, { "Bias", 1, 0.25f } };
        static constexpr FMaterialFormula Formula = { "Bias", "Curves", "Bends a 0..1 value toward 0 when Bias is under 0.5 and toward 1 above it.", Inputs, 0,
            "{T} $ = {X} / ((1.0 / max({Bias}, 1e-6) - 2.0) * (1.0 - {X}) + 1.0);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Gain::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.5f }, { "Gain", 1, 0.25f } };
        static constexpr FMaterialFormula Formula = { "Gain", "Curves", "An S curve on a 0..1 value, steeper in the middle when Gain is under 0.5.", Inputs, 0,
            "{T} $_S = step({T}(0.5), {X});\n"
            "{T} $_U = lerp(2.0 * {X}, 2.0 - 2.0 * {X}, $_S);\n"
            "{T} $_C = $_U / ((1.0 / max({Gain}, 1e-6) - 2.0) * (1.0 - $_U) + 1.0);\n"
            "{T} $ = lerp(0.5 * $_C, 1.0 - 0.5 * $_C, $_S);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_EaseIn::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Ease In", "Curves", "Starts slow and speeds up across 0..1.", kEasePins, 0,
            "{T} $ = pow(saturate({X}), {T}({Power}));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_EaseOut::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Ease Out", "Curves", "Starts fast and slows down across 0..1.", kEasePins, 0,
            "{T} $ = 1.0 - pow(1.0 - saturate({X}), {T}({Power}));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_EaseInOut::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Ease In Out", "Curves", "Slow at both ends of 0..1 and fast in the middle.", kEasePins, 0,
            "{T} $_X = saturate({X});\n"
            "{T} $_A = pow($_X, {T}({Power}));\n"
            "{T} $ = $_A / max($_A + pow(1.0 - $_X, {T}({Power})), {T}(1e-6));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SineWave::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Sine Wave", "Waves", "A smooth wave between -Amplitude and Amplitude, Frequency times a second. Phase is in cycles.", kWavePins, 1,
            "float $ = {Amplitude} * sin(6.2831853 * ({Time} * {Frequency} + {Phase}));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SquareWave::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Square Wave", "Waves", "Jumps between Amplitude and -Amplitude, Frequency times a second.", kWavePins, 1,
            "float $ = {Amplitude} * (1.0 - 2.0 * step(0.5, frac({Time} * {Frequency} + {Phase})));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_TriangleWave::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Triangle Wave", "Waves", "Ramps evenly up and down between -Amplitude and Amplitude.", kWavePins, 1,
            "float $ = {Amplitude} * (1.0 - 4.0 * abs(frac({Time} * {Frequency} + {Phase}) - 0.5));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SawtoothWave::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Sawtooth Wave", "Waves", "Ramps up from -Amplitude to Amplitude, then drops back.", kWavePins, 1,
            "float $ = {Amplitude} * (2.0 * frac({Time} * {Frequency} + {Phase}) - 1.0);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_PulseWave::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kTime, { "Frequency", 1, 1.0f }, { "Duty", 1, 0.5f }, { "Phase", 1, 0.0f } };
        static constexpr FMaterialFormula Formula = { "Pulse Wave", "Waves", "1 for the Duty share of each cycle and 0 for the rest, such as a blinking light.", Inputs, 1,
            "float $ = step(frac({Time} * {Frequency} + {Phase}), {Duty});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Spherize::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Strength", 1, 10.0f } };
        static constexpr FMaterialFormula Formula = { "Spherize", "UV", "Bulges the UV out from Center like a fisheye.", Inputs, 2,
            "float2 $_D = {UV} - {Center};\n"
            "float $_D2 = dot($_D, $_D);\n"
            "float2 $ = {UV} + $_D * ($_D2 * $_D2 * {Strength});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_RadialShear::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Strength", 1, 10.0f } };
        static constexpr FMaterialFormula Formula = { "Radial Shear", "UV", "Swirls the UV around Center, more the farther out it is.", Inputs, 2,
            "float2 $_D = {UV} - {Center};\n"
            "float2 $ = {UV} + float2($_D.y, -$_D.x) * (dot($_D, $_D) * {Strength});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Pixelate::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Resolution", 2, 32.0f } };
        static constexpr FMaterialFormula Formula = { "Pixelate", "UV", "Snaps the UV to the centers of a Resolution sized grid of blocks.", Inputs, 2,
            "float2 $_R = max({Resolution}, float2(1.0));\n"
            "float2 $ = (floor({UV} * $_R) + 0.5) / $_R;" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_MirrorUV::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV };
        static constexpr FMaterialFormula Formula = { "Mirror UV", "UV", "Repeats the UV mirrored on every other tile, so edges always meet.", Inputs, 2,
            "float2 $ = 1.0 - abs(frac({UV} * 0.5) * 2.0 - 1.0);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_FlipUV::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "FlipU", 1, 1.0f }, { "FlipV", 1, 0.0f } };
        static constexpr FMaterialFormula Formula = { "Flip UV", "UV", "Flips U and V on their own, each where its value is 1.", Inputs, 2,
            "float2 $ = lerp({UV}, 1.0 - {UV}, float2(step(0.5, {FlipU}), step(0.5, {FlipV})));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_ScaleUV::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Scale", 2, 2.0f }, kCenter };
        static constexpr FMaterialFormula Formula = { "Scale UV", "UV", "Scales the UV around Center rather than the corner.", Inputs, 2,
            "float2 $ = ({UV} - {Center}) * {Scale} + {Center};" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BumpOffset::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Height", 1, 0.5f }, { "Scale", 1, 0.05f }, { "Reference", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Bump Offset", "UV", "Cheap parallax that shifts the UV by Height toward the viewer.", Inputs, 2,
            "float3 $_V = normalize(GetCameraPosition() - WorldPosition);\n"
            "float3 $_B = cross(WorldNormal.xyz, WorldTangent.xyz) * WorldTangent.w;\n"
            "float2 $ = {UV} + float2(dot($_V, WorldTangent.xyz), dot($_V, $_B)) * (({Height} - {Reference}) * {Scale});",
            {}, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_WorldAlignedUV::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kPosition, kNormal, { "Scale", 1, 1.0f } };
        static constexpr FMaterialFormula Formula = { "World Aligned UV", "UV", "A UV from world position projected along the axis the surface faces most, for textures that ignore the mesh.", Inputs, 2,
            "float3 $_N = abs({Normal});\n"
            "float3 $_P = {Position} * {Scale};\n"
            "float2 $ = $_N.y >= max($_N.x, $_N.z) ? $_P.xz : ($_N.x >= $_N.z ? $_P.zy : $_P.xy);",
            {}, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_GridCells::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Cells", 2, 4.0f } };
        static constexpr FMaterialFormulaPin Outputs[] = { { "CellUV", 2 }, { "CellID", 2 }, { "Random", 1 } };
        static constexpr FMaterialFormula Formula = { "Grid Cells", "UV", "Splits the UV into a grid, giving each cell its own 0..1 UV, its index and a random value.", Inputs, 2,
            "float2 $_S = {UV} * {Cells};\n"
            "float2 $_CellUV = frac($_S);\n"
            "float2 $_CellID = floor($_S);\n"
            "float $_Random = frac(sin(dot($_CellID, float2(12.9898, 78.233))) * 43758.5453);",
            Outputs };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_LensDistortion::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Strength", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Lens Distortion", "UV", "Barrel distortion for positive Strength and pincushion for negative.", Inputs, 2,
            "float2 $_D = {UV} - {Center};\n"
            "float2 $ = {Center} + $_D * (1.0 + {Strength} * dot($_D, $_D));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_WavyUV::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Amplitude", 1, 0.02f }, { "Frequency", 1, 10.0f }, { "Speed", 1, 1.0f }, kTime };
        static constexpr FMaterialFormula Formula = { "Wavy UV", "UV", "Ripples the UV with moving sine waves, such as heat haze or water.", Inputs, 2,
            "float $_T = {Time} * {Speed};\n"
            "float2 $ = {UV} + {Amplitude} * float2(sin({UV}.y * {Frequency} + $_T), cos({UV}.x * {Frequency} + $_T));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_DistortUV::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Offset", 2, 0.5f }, { "Strength", 1, 0.1f } };
        static constexpr FMaterialFormula Formula = { "Distort UV", "UV", "Pushes the UV by a 0..1 Offset such as noise or a flow map, where 0.5 means no push.", Inputs, 2,
            "float2 $ = {UV} + ({Offset} * 2.0 - 1.0) * {Strength};" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Circle::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Size", 2, 0.5f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Circle", "Shapes", "1 inside a circle or ellipse Size across, 0 outside.", Inputs, 1,
            "float2 $_R = max({Size} * 0.5, float2(1e-5));\n"
            "float $_D = (length(({UV} - {Center}) / $_R) - 1.0) * min($_R.x, $_R.y);"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Rectangle::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Size", 2, 0.5f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Rectangle", "Shapes", "1 inside a rectangle Size across, 0 outside.", Inputs, 1,
            "float2 $_Q = abs({UV} - {Center}) - {Size} * 0.5;\n"
            "float $_D = length(max($_Q, float2(0.0))) + min(max($_Q.x, $_Q.y), 0.0);"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_RoundedRectangle::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Size", 2, 0.5f }, { "Radius", 1, 0.1f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Rounded Rectangle", "Shapes", "1 inside a rectangle with corners rounded by Radius, 0 outside.", Inputs, 1,
            "float $_R = min({Radius}, min({Size}.x, {Size}.y) * 0.5);\n"
            "float2 $_Q = abs({UV} - {Center}) - {Size} * 0.5 + $_R;\n"
            "float $_D = length(max($_Q, float2(0.0))) + min(max($_Q.x, $_Q.y), 0.0) - $_R;"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Polygon::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Sides", 1, 6.0f }, { "Radius", 1, 0.4f }, { "Rotation", 1, 0.0f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Polygon", "Shapes", "1 inside a regular polygon with Sides corners, Radius to each corner. Rotation is in turns.", Inputs, 1,
            "float2 $_P = {UV} - {Center};\n"
            "float $_S = 6.2831853 / max(floor({Sides}), 3.0);\n"
            "float $_A = atan2($_P.x, -$_P.y) + 6.2831853 * {Rotation};\n"
            "float $_D = cos(floor(0.5 + $_A / $_S) * $_S - $_A) * length($_P) - {Radius} * cos(0.5 * $_S);"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Ring::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Radius", 1, 0.35f }, { "Thickness", 1, 0.05f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Ring", "Shapes", "1 on a circular band Thickness wide at Radius, 0 elsewhere.", Inputs, 1,
            "float $_D = abs(length({UV} - {Center}) - {Radius}) - {Thickness} * 0.5;"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Star::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Points", 1, 5.0f }, { "Radius", 1, 0.4f }, { "Inner", 1, 0.45f }, { "Rotation", 1, 0.0f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Star", "Shapes", "1 inside a star. Inner is the share of Radius the dips reach, and Rotation is in turns.", Inputs, 1,
            "float2 $_P = {UV} - {Center};\n"
            "float $_Half = 3.1415927 / max(floor({Points}), 2.0);\n"
            "float $_B = abs(fmod(atan2($_P.x, -$_P.y) + 6.2831853 * ({Rotation} + 1.0), 2.0 * $_Half) - $_Half);\n"
            "float2 $_Inner = float2({Radius} * {Inner}, 0.0);\n"
            "float2 $_E = {Radius} * float2(cos($_Half), sin($_Half)) - $_Inner;\n"
            "float2 $_W = length($_P) * float2(cos($_B), sin($_B)) - $_Inner;\n"
            "float $_D = length($_W - $_E * saturate(dot($_W, $_E) / max(dot($_E, $_E), 1e-8)));\n"
            "$_D *= ($_E.x * $_W.y - $_E.y * $_W.x) > 0.0 ? -1.0 : 1.0;"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_LineSegment::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Start", 2, 0.2f }, { "End", 2, 0.8f }, { "Thickness", 1, 0.02f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Line Segment", "Shapes", "1 on a line Thickness wide from Start to End, 0 elsewhere.", Inputs, 1,
            "float2 $_PA = {UV} - {Start};\n"
            "float2 $_BA = {End} - {Start};\n"
            "float $_H = saturate(dot($_PA, $_BA) / max(dot($_BA, $_BA), 1e-8));\n"
            "float $_D = length($_PA - $_BA * $_H) - {Thickness} * 0.5;"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_PlusShape::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Size", 1, 0.5f }, { "Thickness", 1, 0.1f }, kSoftness };
        static constexpr FMaterialFormula Formula = { "Plus Shape", "Shapes", "1 inside a plus sign Size across with arms Thickness wide.", Inputs, 1,
            "float2 $_P = abs({UV} - {Center});\n"
            "float2 $_Q1 = $_P - float2({Size}, {Thickness}) * 0.5;\n"
            "float2 $_Q2 = $_P - float2({Thickness}, {Size}) * 0.5;\n"
            "float $_D = min(length(max($_Q1, float2(0.0))) + min(max($_Q1.x, $_Q1.y), 0.0), length(max($_Q2, float2(0.0))) + min(max($_Q2.x, $_Q2.y), 0.0));"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_LinearGradient::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Angle", 1, 0.0f } };
        static constexpr FMaterialFormula Formula = { "Linear Gradient", "Shapes", "0 to 1 across the UV square along Angle, in turns, where 0 runs left to right.", Inputs, 1,
            "float2 $_Dir = float2(cos(6.2831853 * {Angle}), sin(6.2831853 * {Angle}));\n"
            "float $ = saturate(dot({UV} - 0.5, $_Dir) + 0.5);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_RadialGradient::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Radius", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Radial Gradient", "Shapes", "1 at Center fading to 0 at Radius.", Inputs, 1,
            "float $ = saturate(1.0 - length({UV} - {Center}) / max({Radius}, 1e-5));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_AngularGradient::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Rotation", 1, 0.0f } };
        static constexpr FMaterialFormula Formula = { "Angular Gradient", "Shapes", "0 to 1 once around Center, such as a radar sweep or a pie timer.", Inputs, 1,
            "float2 $_P = {UV} - {Center};\n"
            "float $ = frac(atan2($_P.y, $_P.x) / 6.2831853 + 0.5 + {Rotation});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_DiamondGradient::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Radius", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Diamond Gradient", "Shapes", "1 at Center fading to 0 in a diamond Radius out.", Inputs, 1,
            "float2 $_P = abs({UV} - {Center});\n"
            "float $ = saturate(1.0 - ($_P.x + $_P.y) / max({Radius}, 1e-5));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Stripes::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Frequency", 1, 10.0f }, { "Angle", 1, 0.125f }, { "Width", 1, 0.5f }, { "Softness", 1, 0.05f } };
        static constexpr FMaterialFormula Formula = { "Stripes", "Shapes", "Parallel stripes, Frequency across the UV. Width is each stripe's share of its period and Angle is in turns.", Inputs, 1,
            "float2 $_Dir = float2(cos(6.2831853 * {Angle}), sin(6.2831853 * {Angle}));\n"
            "float $_D = abs(frac(dot({UV}, $_Dir) * {Frequency}) - 0.5) - {Width} * 0.5;"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_GridLines::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Cells", 2, 8.0f }, { "Thickness", 1, 0.05f }, { "Softness", 1, 0.02f } };
        static constexpr FMaterialFormula Formula = { "Grid Lines", "Shapes", "1 on the lines of a grid, Thickness as a share of a cell.", Inputs, 1,
            "float2 $_G = abs(frac({UV} * {Cells}) - 0.5);\n"
            "float $_D = 0.5 - max($_G.x, $_G.y) - {Thickness} * 0.5;"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Bricks::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Columns", 1, 4.0f }, { "Rows", 1, 8.0f }, { "Mortar", 1, 0.01f }, { "Offset", 1, 0.5f }, { "Softness", 1, 0.002f } };
        static constexpr FMaterialFormulaPin Outputs[] = { { "Mask", 1 }, { "BrickUV", 2 }, { "BrickID", 2 }, { "Random", 1 } };
        static constexpr FMaterialFormula Formula = { "Bricks", "Shapes", "A brick wall, 1 on the bricks and 0 on the mortar, with every other row shifted by Offset.", Inputs, 1,
            "float2 $_Count = float2({Columns}, {Rows});\n"
            "float2 $_S = {UV} * $_Count;\n"
            "$_S.x += {Offset} * frac(floor($_S.y) * 0.5) * 2.0;\n"
            "float2 $_BrickID = floor($_S);\n"
            "float2 $_BrickUV = frac($_S);\n"
            "float2 $_E = min($_BrickUV, 1.0 - $_BrickUV) / max($_Count, float2(1e-5));\n"
            "float $_Mask = saturate(0.5 + (min($_E.x, $_E.y) - {Mortar} * 0.5) / max({Softness}, 1e-5));\n"
            "float $_Random = frac(sin(dot($_BrickID, float2(12.9898, 78.233))) * 43758.5453);",
            Outputs };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_HexGrid::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Scale", 1, 8.0f }, { "Thickness", 1, 0.05f }, kSoftness };
        static constexpr FMaterialFormulaPin Outputs[] = { { "Lines", 1 }, { "Distance", 1 }, { "CellID", 2 }, { "Random", 1 } };
        static constexpr FMaterialFormula Formula = { "Hex Grid", "Shapes", "A honeycomb, with its lines, the distance to the nearest edge, and each cell's index and random value.", Inputs, 1,
            "float2 $_P = {UV} * {Scale};\n"
            "float2 $_R = float2(1.0, 1.7320508);\n"
            "float2 $_A = $_P - $_R * floor($_P / $_R) - 0.5 * $_R;\n"
            "float2 $_B = ($_P - 0.5 * $_R) - $_R * floor(($_P - 0.5 * $_R) / $_R) - 0.5 * $_R;\n"
            "float2 $_G = dot($_A, $_A) < dot($_B, $_B) ? $_A : $_B;\n"
            "float2 $_Abs = abs($_G);\n"
            "float $_Distance = 0.5 - max(dot($_Abs, float2(0.5, 0.8660254)), $_Abs.x);\n"
            "float2 $_CellID = $_P - $_G;\n"
            "float $_Random = frac(sin(dot(floor($_CellID * 2.0 + 0.5), float2(12.9898, 78.233))) * 43758.5453);\n"
            "float $_Lines = saturate(0.5 - ($_Distance - {Thickness} * 0.5) / max({Softness}, 1e-5));",
            Outputs };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Dots::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Cells", 2, 8.0f }, { "Radius", 1, 0.3f }, { "Softness", 1, 0.02f } };
        static constexpr FMaterialFormula Formula = { "Dots", "Shapes", "A polka dot pattern, one dot per grid cell, Radius as a share of a cell.", Inputs, 1,
            "float $_D = length(frac({UV} * {Cells}) - 0.5) - {Radius};"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Spiral::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, kCenter, { "Arms", 1, 3.0f }, { "Twist", 1, 10.0f }, { "Width", 1, 0.5f }, { "Softness", 1, 0.05f } };
        static constexpr FMaterialFormula Formula = { "Spiral", "Shapes", "Spiral arms winding out from Center. Width is each arm's share of the gap.", Inputs, 1,
            "float2 $_P = {UV} - {Center};\n"
            "float $_X = frac(atan2($_P.y, $_P.x) / 6.2831853 * floor({Arms}) + length($_P) * {Twist});\n"
            "float $_D = abs($_X - 0.5) - {Width} * 0.5;"
            LUMINA_SHAPE_EDGE };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Vignette::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Intensity", 1, 1.0f }, { "Power", 1, 2.0f } };
        static constexpr FMaterialFormula Formula = { "Vignette", "Shapes", "1 in the middle of the UV square darkening toward the corners.", Inputs, 1,
            "float $ = saturate(1.0 - pow(length({UV} - 0.5) * 1.4142136, max({Power}, 1e-3)) * {Intensity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_FBMNoise::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "FBM Noise", "Noise", "Layers of smooth noise at rising detail, such as clouds, dirt or terrain. 0..1.", kFractalPins, 1,
            LUMINA_FRACTAL_NOISE_2D("$_N") };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_RidgedNoise::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Ridged Noise", "Noise", "Fractal noise with sharp crests, such as mountain ridges or lightning veins. 0..1.", kFractalPins, 1,
            LUMINA_FRACTAL_NOISE_2D("(1.0 - abs($_N * 2.0 - 1.0)) * (1.0 - abs($_N * 2.0 - 1.0))") };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_TurbulenceNoise::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Turbulence Noise", "Noise", "Fractal noise with sharp creases, such as smoke, marble or fire. 0..1.", kFractalPins, 1,
            LUMINA_FRACTAL_NOISE_2D("abs($_N * 2.0 - 1.0)") };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_FBMNoise3D::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kPosition, { "Scale", 1, 0.5f }, { "Octaves", 1, 4.0f }, { "Lacunarity", 1, 2.0f }, { "Gain", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "FBM Noise 3D", "Noise", "Fractal noise through 3D space, so world position noise has no UV seams. One octave is plain smooth noise. 0..1.", Inputs, 1,
            "float3 $_P = {Position} * {Scale};\n"
            "float $_Sum = 0.0;\n"
            "float $_Amp = 1.0;\n"
            "float $_Norm = 0.0;\n"
            "int $_Count = clamp(int({Octaves}), 1, 8);\n"
            "for (int $_Octave = 0; $_Octave < $_Count; ++$_Octave)\n"
            "{\n"
            "float3 $_I = floor($_P);\n"
            "float3 $_U = frac($_P) * frac($_P) * (3.0 - 2.0 * frac($_P));\n"
            "float $_N = 0.0;\n"
            "for (int $_K = 0; $_K < 8; ++$_K)\n"
            "{\n"
            "float3 $_O = float3(float($_K & 1), float(($_K >> 1) & 1), float(($_K >> 2) & 1));\n"
            "float3 $_W = lerp(1.0 - $_U, $_U, $_O);\n"
            "uint3 $_Q = uint3(int3($_I + $_O));\n"
            "uint $_S = $_Q.x * 1597334673u ^ $_Q.y * 3812015801u ^ $_Q.z * 2798796415u;\n"
            "$_S = ($_S ^ ($_S >> 16u)) * 2246822519u;\n"
            "$_N += $_W.x * $_W.y * $_W.z * float($_S ^ ($_S >> 13u)) * (1.0 / 4294967296.0);\n"
            "}\n"
            "$_Sum += $_Amp * $_N;\n"
            "$_Norm += $_Amp;\n"
            "$_Amp *= {Gain};\n"
            "$_P = $_P * {Lacunarity} + float3(17.31, 9.17, 4.73);\n"
            "}\n"
            "float $ = $_Sum / max($_Norm, 1e-6);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SimplexNoise::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Scale", 1, 8.0f } };
        static constexpr FMaterialFormula Formula = { "Simplex Noise", "Noise", "Smooth gradient noise on a triangle grid, with fewer square artifacts than Perlin. 0..1.", Inputs, 1,
            "float2 $_P = {UV} * {Scale};\n"
            "float2 $_I = floor($_P + ($_P.x + $_P.y) * 0.36602540);\n"
            "float2 $_A = $_P - $_I + ($_I.x + $_I.y) * 0.21132487;\n"
            "float2 $_O = $_A.x > $_A.y ? float2(1.0, 0.0) : float2(0.0, 1.0);\n"
            "float2 $_B = $_A - $_O + 0.21132487;\n"
            "float2 $_C = $_A - 1.0 + 2.0 * 0.21132487;\n"
            "float3 $_H = max(0.5 - float3(dot($_A, $_A), dot($_B, $_B), dot($_C, $_C)), float3(0.0));\n"
            "float3 $_G = float3(0.0);\n"
            "for (int $_K = 0; $_K < 3; ++$_K)\n"
            "{\n"
            "uint2 $_Cell = uint2(int2($_I + ($_K == 0 ? float2(0.0) : ($_K == 1 ? $_O : float2(1.0)))));\n"
            "uint2 $_S = $_Cell.x * uint2(1597334673u, 2798796415u) ^ $_Cell.y * uint2(3812015801u, 1597334673u);\n"
            "$_S = ($_S ^ ($_S >> 16u)) * 2246822519u;\n"
            "float2 $_Grad = float2($_S ^ ($_S >> 13u)) * (2.0 / 4294967296.0) - 1.0;\n"
            "$_G[$_K] = dot($_K == 0 ? $_A : ($_K == 1 ? $_B : $_C), $_Grad);\n"
            "}\n"
            "float $ = 0.5 + 0.5 * dot($_H * $_H * $_H * $_H * $_G, float3(70.0));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_VoronoiCells::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Scale", 1, 5.0f }, { "Randomness", 1, 1.0f } };
        static constexpr FMaterialFormulaPin Outputs[] = { { "Distance", 1 }, { "Edge", 1 }, { "CellID", 2 }, { "Random", 1 } };
        static constexpr FMaterialFormula Formula = { "Voronoi Cells", "Noise", "Cellular noise with the distance to the nearest point, the width to the next edge, and each cell's index and random value.", Inputs, 1,
            "float2 $_P = {UV} * {Scale};\n"
            "float2 $_I = floor($_P);\n"
            "float2 $_F = frac($_P);\n"
            "float $_F1 = 8.0;\n"
            "float $_F2 = 8.0;\n"
            "float2 $_Cell = float2(0.0);\n"
            "for (int $_Y = -1; $_Y <= 1; ++$_Y)\n"
            "for (int $_X = -1; $_X <= 1; ++$_X)\n"
            "{\n"
            "float2 $_G = float2(float($_X), float($_Y));\n"
            "float2 $_O = frac(sin(float2(dot($_I + $_G, float2(127.1, 311.7)), dot($_I + $_G, float2(269.5, 183.3)))) * 43758.5453) * {Randomness};\n"
            "float $_D = length($_G + $_O - $_F);\n"
            "if ($_D < $_F1) { $_F2 = $_F1; $_F1 = $_D; $_Cell = $_I + $_G; }\n"
            "else if ($_D < $_F2) { $_F2 = $_D; }\n"
            "}\n"
            "float $_Distance = $_F1;\n"
            "float $_Edge = $_F2 - $_F1;\n"
            "float2 $_CellID = $_Cell;\n"
            "float $_Random = frac(sin(dot($_Cell, float2(12.9898, 78.233))) * 43758.5453);",
            Outputs };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_DomainWarp::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kUV, { "Scale", 1, 4.0f }, { "Strength", 1, 0.1f } };
        static constexpr FMaterialFormula Formula = { "Domain Warp", "Noise", "Bends the UV with smooth noise, so anything sampled through it looks organic.", Inputs, 2,
            "float2 $_W = float2(0.0);\n"
            "for (int $_K = 0; $_K < 2; ++$_K)\n"
            "{\n"
            "float2 $_P = {UV} * {Scale} + float2(5.2, 1.3) * float($_K);\n"
            LUMINA_VALUE_NOISE_2D("$_W[$_K]")
            "}\n"
            "float2 $ = {UV} + {Strength} * ($_W * 2.0 - 1.0);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_RandomRange::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Seed", 2, 0.0f, "UV0" }, { "Min", 0, 0.0f }, { "Max", 0, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Random Range", "Noise", "A random value between Min and Max, the same every frame for the same Seed.", Inputs, 0,
            "{T} $ = lerp({Min}, {Max}, frac(sin(dot({Seed}, float2(12.9898, 78.233))) * 43758.5453));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_ScreenDither::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Screen Dither", "Noise", "A fine per pixel pattern in 0..1 for dithering gradients and fades.", TSpan<const FMaterialFormulaPin>(), 1,
            "float $ = frac(52.9829189 * frac(dot(Input.Position.xy, float2(0.06711056, 0.00583715))));",
            {}, EMaterialFormulaLimit::PixelOnly };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_HueShift::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Shift", 1, 0.25f } };
        static constexpr FMaterialFormula Formula = { "Hue Shift", "Color", "Rotates the hue of Color by Shift, in turns around the color wheel.", Inputs, 3,
            "float3 $_K = float3(0.57735027, 0.57735027, 0.57735027);\n"
            "float $_C = cos(6.2831853 * {Shift});\n"
            "float3 $ = {Color} * $_C + cross($_K, {Color}) * sin(6.2831853 * {Shift}) + $_K * dot($_K, {Color}) * (1.0 - $_C);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Saturation::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Amount", 1, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Saturation", "Color", "0 is grayscale, 1 leaves Color alone and above 1 makes it more vivid.", Inputs, 3,
            "float3 $ = lerp(float3(dot({Color}, float3(0.2126, 0.7152, 0.0722))), {Color}, {Amount});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Vibrance::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Amount", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Vibrance", "Color", "Boosts saturation mostly on dull colors, leaving vivid ones and skin tones alone.", Inputs, 3,
            "float3 $_C = {Color};\n"
            "float $_S = max($_C.r, max($_C.g, $_C.b)) - min($_C.r, min($_C.g, $_C.b));\n"
            "float3 $ = lerp(float3(dot($_C, float3(0.2126, 0.7152, 0.0722))), $_C, 1.0 + {Amount} * (1.0 - $_S));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Exposure::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Stops", 1, 0.0f } };
        static constexpr FMaterialFormula Formula = { "Exposure", "Color", "Brightens Color by photographic stops, where each stop doubles it.", Inputs, 3,
            "float3 $ = {Color} * exp2({Stops});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Levels::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Color", 3, 0.5f }, { "InBlack", 1, 0.0f }, { "InWhite", 1, 1.0f }, { "Gamma", 1, 1.0f }, { "OutBlack", 1, 0.0f }, { "OutWhite", 1, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Levels", "Color", "Photo editor levels, setting the input black and white points, the midtones and the output range.", Inputs, 3,
            "float3 $_N = saturate(({Color} - {InBlack}) / max({InWhite} - {InBlack}, 1e-5));\n"
            "float3 $ = lerp(float3({OutBlack}), float3({OutWhite}), pow($_N, float3(1.0 / max({Gamma}, 1e-5))));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_WhiteBalance::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Temperature", 1, 0.0f }, { "Tint", 1, 0.0f } };
        static constexpr FMaterialFormula Formula = { "White Balance", "Color", "Warms or cools Color with Temperature and pushes it green or magenta with Tint, both -1..1.", Inputs, 3,
            "float3 $_W = float3(1.0 + 0.3 * {Temperature}, 1.0, 1.0 - 0.3 * {Temperature});\n"
            "float3 $_T = float3(1.0 + 0.15 * {Tint}, 1.0 - 0.3 * {Tint}, 1.0 + 0.15 * {Tint});\n"
            "float3 $ = {Color} * $_W * $_T;" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_CosinePalette::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Value", 1, 0.0f }, { "Bias", 3, 0.5f }, { "Amplitude", 3, 0.5f }, { "Frequency", 3, 1.0f },
                                                          { "Phase", 3, 0.0f, "float3(0.0, 0.33, 0.67)" } };
        static constexpr FMaterialFormula Formula = { "Cosine Palette", "Color", "A smooth color ramp from four colors, Bias + Amplitude * cos(2 pi (Frequency * Value + Phase)). The default is a rainbow.", Inputs, 3,
            "float3 $ = {Bias} + {Amplitude} * cos(6.2831853 * ({Frequency} * {Value} + {Phase}));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Blackbody::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Temperature", 1, 6500.0f } };
        static constexpr FMaterialFormula Formula = { "Blackbody", "Color", "The color of light glowing at Temperature kelvin, red near 1000 and blue past 10000.", Inputs, 3,
            "float $_T = clamp({Temperature}, 1000.0, 40000.0) / 100.0;\n"
            "float $_R = $_T <= 66.0 ? 1.0 : saturate(1.29293618 * pow(max($_T - 60.0, 1e-3), -0.1332047592));\n"
            "float $_G = $_T <= 66.0 ? saturate(0.39008157 * log($_T) - 0.63184144) : saturate(1.12989086 * pow(max($_T - 60.0, 1e-3), -0.0755148492));\n"
            "float $_B = $_T >= 66.0 ? 1.0 : ($_T <= 19.0 ? 0.0 : saturate(0.54320679 * log(max($_T - 10.0, 1e-3)) - 1.19625409));\n"
            "float3 $ = pow(float3($_R, $_G, $_B), float3(2.2));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_ChannelMixer::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Red", 3, 0.0f, "float3(1.0, 0.0, 0.0)" }, { "Green", 3, 0.0f, "float3(0.0, 1.0, 0.0)" },
                                                          { "Blue", 3, 0.0f, "float3(0.0, 0.0, 1.0)" } };
        static constexpr FMaterialFormula Formula = { "Channel Mixer", "Color", "Builds each output channel from a weighted mix of the input's red, green and blue.", Inputs, 3,
            "float3 $ = float3(dot({Color}, {Red}), dot({Color}, {Green}), dot({Color}, {Blue}));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_ReplaceColor::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "From", 3, 0.0f }, { "To", 3, 1.0f }, { "Range", 1, 0.1f }, { "Fuzziness", 1, 0.1f } };
        static constexpr FMaterialFormula Formula = { "Replace Color", "Color", "Swaps colors near From for To, within Range and fading over Fuzziness.", Inputs, 3,
            "float3 $ = lerp({To}, {Color}, saturate((distance({Color}, {From}) - {Range}) / max({Fuzziness}, 1e-5)));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_ColorMask::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Target", 3, 1.0f }, { "Range", 1, 0.1f }, { "Fuzziness", 1, 0.1f } };
        static constexpr FMaterialFormula Formula = { "Color Mask", "Color", "1 where Color is near Target, fading to 0 over Fuzziness past Range.", Inputs, 1,
            "float $ = 1.0 - saturate((distance({Color}, {Target}) - {Range}) / max({Fuzziness}, 1e-5));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_CheapContrast::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Color", 0, 0.5f }, { "Contrast", 1, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Cheap Contrast", "Color", "Pushes values away from the middle and clamps them, a fast way to sharpen a mask.", Inputs, 0,
            "{T} $ = saturate(lerp({T}(-{Contrast}), {T}(1.0 + {Contrast}), {Color}));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Sepia::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kColor, { "Amount", 1, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Sepia", "Color", "Old photo brown tones, blended in by Amount.", Inputs, 3,
            "float3 $_S = float3(dot({Color}, float3(0.393, 0.769, 0.189)), dot({Color}, float3(0.349, 0.686, 0.168)), dot({Color}, float3(0.272, 0.534, 0.131)));\n"
            "float3 $ = lerp({Color}, $_S, saturate({Amount}));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_HueToRGB::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Hue", 1, 0.0f } };
        static constexpr FMaterialFormula Formula = { "Hue To RGB", "Color", "The fully saturated color at Hue, 0..1 once around the rainbow.", Inputs, 3,
            "float3 $ = saturate(abs(frac({Hue} + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendMultiply::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Multiply", "Blend", "Darkens Base by Blend, like stacking slides.", kBlendPins, 0,
            "{T} $ = lerp({Base}, {Base} * {Blend}, {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendScreen::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Screen", "Blend", "Lightens Base by Blend, like projecting two lights onto one screen.", kBlendPins, 0,
            "{T} $ = lerp({Base}, 1.0 - (1.0 - {Base}) * (1.0 - {Blend}), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendOverlay::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Overlay", "Blend", "Multiplies the darks and screens the lights of Base, adding contrast.", kBlendPins, 0,
            "{T} $ = lerp({Base}, lerp(2.0 * {Base} * {Blend}, 1.0 - 2.0 * (1.0 - {Base}) * (1.0 - {Blend}), step({T}(0.5), {Base})), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendSoftLight::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Soft Light", "Blend", "A gentle overlay, like shining a diffuse light on Base.", kBlendPins, 0,
            "{T} $ = lerp({Base}, (1.0 - 2.0 * {Blend}) * {Base} * {Base} + 2.0 * {Blend} * {Base}, {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendHardLight::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Hard Light", "Blend", "Overlay keyed on Blend instead of Base, like a harsh spotlight.", kBlendPins, 0,
            "{T} $ = lerp({Base}, lerp(2.0 * {Base} * {Blend}, 1.0 - 2.0 * (1.0 - {Base}) * (1.0 - {Blend}), step({T}(0.5), {Blend})), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendColorDodge::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Color Dodge", "Blend", "Brightens Base sharply toward Blend, blowing out highlights.", kBlendPins, 0,
            "{T} $ = lerp({Base}, min({Base} / max(1.0 - {Blend}, {T}(1e-5)), {T}(1.0)), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendColorBurn::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Color Burn", "Blend", "Darkens Base sharply toward Blend, deepening shadows.", kBlendPins, 0,
            "{T} $ = lerp({Base}, 1.0 - min((1.0 - {Base}) / max({Blend}, {T}(1e-5)), {T}(1.0)), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendLinearDodge::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Linear Dodge", "Blend", "Adds Blend to Base.", kBlendPins, 0,
            "{T} $ = lerp({Base}, {Base} + {Blend}, {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendLinearBurn::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Linear Burn", "Blend", "Adds Base and Blend and subtracts 1, darkening more than Multiply.", kBlendPins, 0,
            "{T} $ = lerp({Base}, max({Base} + {Blend} - 1.0, {T}(0.0)), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendLinearLight::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Linear Light", "Blend", "Brightens Base where Blend is over 0.5 and darkens it where under.", kBlendPins, 0,
            "{T} $ = lerp({Base}, {Base} + 2.0 * {Blend} - 1.0, {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendVividLight::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Vivid Light", "Blend", "Color Burn where Blend is under 0.5 and Color Dodge where over.", kBlendPins, 0,
            "{T} $_Burn = 1.0 - min((1.0 - {Base}) / max(2.0 * {Blend}, {T}(1e-5)), {T}(1.0));\n"
            "{T} $_Dodge = min({Base} / max(2.0 * (1.0 - {Blend}), {T}(1e-5)), {T}(1.0));\n"
            "{T} $ = lerp({Base}, lerp($_Burn, $_Dodge, step({T}(0.5), {Blend})), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendPinLight::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Pin Light", "Blend", "Darken where Blend is under 0.5 and Lighten where over.", kBlendPins, 0,
            "{T} $ = lerp({Base}, lerp(min({Base}, 2.0 * {Blend}), max({Base}, 2.0 * {Blend} - 1.0), step({T}(0.5), {Blend})), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendHardMix::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Hard Mix", "Blend", "Each channel snaps to 0 or 1 by whether Base and Blend sum past 1.", kBlendPins, 0,
            "{T} $ = lerp({Base}, step({T}(1.0), {Base} + {Blend}), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendDifference::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Difference", "Blend", "The absolute difference of Base and Blend.", kBlendPins, 0,
            "{T} $ = lerp({Base}, abs({Base} - {Blend}), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendExclusion::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Exclusion", "Blend", "Like Difference with less contrast.", kBlendPins, 0,
            "{T} $ = lerp({Base}, {Base} + {Blend} - 2.0 * {Base} * {Blend}, {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendSubtract::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Subtract", "Blend", "Takes Blend away from Base, stopping at 0.", kBlendPins, 0,
            "{T} $ = lerp({Base}, max({Base} - {Blend}, {T}(0.0)), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendDivide::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Divide", "Blend", "Divides Base by Blend, which brightens and can remove a color cast.", kBlendPins, 0,
            "{T} $ = lerp({Base}, {Base} / max({Blend}, {T}(1e-5)), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendDarken::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Darken", "Blend", "Keeps the darker of Base and Blend per channel.", kBlendPins, 0,
            "{T} $ = lerp({Base}, min({Base}, {Blend}), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_BlendLighten::GetFormula() const
    {
        static constexpr FMaterialFormula Formula = { "Blend Lighten", "Blend", "Keeps the lighter of Base and Blend per channel.", kBlendPins, 0,
            "{T} $ = lerp({Base}, max({Base}, {Blend}), {Opacity});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SphereMask::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kPosition, { "Center", 3, 0.0f }, { "Radius", 1, 100.0f }, { "Hardness", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Sphere Mask", "Utility", "1 inside a sphere around Center, fading to 0 at Radius. Hardness 1 is a sharp edge.", Inputs, 1,
            "float $ = saturate((1.0 - distance({Position}, {Center}) / max({Radius}, 1e-5)) / max(1.0 - {Hardness}, 1e-5));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_CameraDistance::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kPosition };
        static constexpr FMaterialFormula Formula = { "Camera Distance", "Scene", "How far Position is from the camera.", Inputs, 1,
            "float $ = distance({Position}, GetCameraPosition());",
            {}, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_DistanceFade::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kPosition, { "Start", 1, 500.0f }, { "End", 1, 1000.0f } };
        static constexpr FMaterialFormula Formula = { "Distance Fade", "Scene", "0 closer to the camera than Start, rising to 1 at End, for fading detail out with distance.", Inputs, 1,
            "float $ = saturate((distance({Position}, GetCameraPosition()) - {Start}) / max({End} - {Start}, 1e-5));",
            {}, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_CameraVectors::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Outputs[] = { { "Position", 3 }, { "Forward", 3 }, { "Right", 3 }, { "Up", 3 } };
        static constexpr FMaterialFormula Formula = { "Camera Vectors", "Scene", "The camera's world position and the directions it faces.", TSpan<const FMaterialFormulaPin>(), 3,
            "float3 $_Position = GetCameraPosition();\n"
            "float3 $_Forward = GetCameraForward();\n"
            "float3 $_Right = GetCameraRight();\n"
            "float3 $_Up = GetCameraUp();",
            Outputs, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SlopeMask::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kNormal, { "Direction", 3, 0.0f, "float3(0.0, 1.0, 0.0)" }, { "Threshold", 1, 0.7f }, { "Sharpness", 1, 10.0f } };
        static constexpr FMaterialFormula Formula = { "Slope Mask", "Utility", "1 where the surface faces Direction, such as snow or moss on top, by default straight up.", Inputs, 1,
            "float $ = saturate((dot(normalize({Normal}), normalize({Direction})) - {Threshold}) * {Sharpness} + 0.5);",
            {}, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_HeightLerp::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "A", 0, 0.0f }, { "B", 0, 1.0f }, { "Height", 1, 0.5f }, { "Alpha", 1, 0.5f }, { "Contrast", 1, 0.1f } };
        static constexpr FMaterialFormula Formula = { "Height Lerp", "Utility", "Blends A to B by Alpha, filling the low parts of Height first, such as water pooling in cracks.", Inputs, 0,
            "float $_T = saturate(({Height} - (1.0 - {Alpha})) / max({Contrast}, 1e-5) + 0.5);\n"
            "{T} $ = lerp({A}, {B}, $_T);" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_NormalStrength::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kTangentNormal, { "Strength", 1, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Normal Strength", "Shading", "Scales how strongly a tangent space normal map bends the surface.", Inputs, 3,
            "float3 $ = normalize(float3({Normal}.xy * {Strength}, lerp(1.0, {Normal}.z, saturate({Strength}))));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_FlattenNormal::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kTangentNormal, { "Flatness", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Flatten Normal", "Shading", "Blends a tangent space normal toward flat, 1 being fully flat.", Inputs, 3,
            "float3 $ = normalize(lerp({Normal}, float3(0.0, 0.0, 1.0), saturate({Flatness})));" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_TangentToWorld::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Vector", 3, 0.0f, "float3(0.0, 0.0, 1.0)" } };
        static constexpr FMaterialFormula Formula = { "Tangent To World", "Vector", "Turns a tangent space direction, such as a normal map sample, into world space.", Inputs, 3,
            "float3 $_B = cross(WorldNormal.xyz, WorldTangent.xyz) * WorldTangent.w;\n"
            "float3 $ = normalize({Vector}.x * WorldTangent.xyz + {Vector}.y * $_B + {Vector}.z * WorldNormal.xyz);",
            {}, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_WorldToTangent::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Vector", 3, 0.0f, "WorldNormal.xyz" } };
        static constexpr FMaterialFormula Formula = { "World To Tangent", "Vector", "Turns a world space direction into the surface's tangent space.", Inputs, 3,
            "float3 $_B = cross(WorldNormal.xyz, WorldTangent.xyz) * WorldTangent.w;\n"
            "float3 $ = float3(dot({Vector}, WorldTangent.xyz), dot({Vector}, $_B), dot({Vector}, WorldNormal.xyz));",
            {}, EMaterialFormulaLimit::NoUI };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_SoftThreshold::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.5f }, { "Threshold", 1, 0.5f }, { "Softness", 1, 0.05f } };
        static constexpr FMaterialFormula Formula = { "Soft Threshold", "Utility", "0 below Threshold and 1 above, with a smooth step Softness wide on each side.", Inputs, 0,
            "{T} $ = smoothstep({T}({Threshold} - {Softness}), {T}({Threshold} + {Softness} + 1e-5), {X});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_InRange::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "X", 0, 0.5f }, { "Min", 0, 0.0f }, { "Max", 0, 1.0f } };
        static constexpr FMaterialFormula Formula = { "In Range", "Conditional", "1 where X is between Min and Max, 0 elsewhere.", Inputs, 0,
            "{T} $ = step({Min}, {X}) * step({X}, {Max});" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Switch::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Index", 1, 0.0f }, { "A", 0, 0.0f }, { "B", 0, 0.33f }, { "C", 0, 0.66f }, { "D", 0, 1.0f } };
        static constexpr FMaterialFormula Formula = { "Switch", "Conditional", "Picks A, B, C or D by Index 0 to 3, while the shader runs.", Inputs, 0,
            "{T} $ = {A};\n"
            "if ({Index} >= 0.5) $ = {B};\n"
            "if ({Index} >= 1.5) $ = {C};\n"
            "if ({Index} >= 2.5) $ = {D};" };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_DitherFade::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { { "Opacity", 1, 0.5f } };
        static constexpr FMaterialFormula Formula = { "Dither Fade", "Utility", "Turns Opacity into a dithered 0 or 1 per pixel, for fading masked materials without sorting.", Inputs, 1,
            "float $ = step(frac(52.9829189 * frac(dot(Input.Position.xy, float2(0.06711056, 0.00583715)))), {Opacity});",
            {}, EMaterialFormulaLimit::PixelOnly };
        return Formula;
    }

    const FMaterialFormula& CMaterialExpression_Wobble::GetFormula() const
    {
        static constexpr FMaterialFormulaPin Inputs[] = { kPosition, { "Direction", 3, 0.0f, "float3(0.0, 1.0, 0.0)" }, { "Amplitude", 1, 0.1f },
                                                          { "Frequency", 1, 1.0f }, { "Speed", 1, 1.0f }, kTime };
        static constexpr FMaterialFormula Formula = { "Wobble", "Vertex", "A World Position Offset that sways the mesh along Direction in a wave through space, such as a flag or jelly.", Inputs, 3,
            "float3 $ = normalize({Direction}) * ({Amplitude} * sin(dot({Position}, float3(1.0, 1.0, 1.0)) * {Frequency} + {Time} * {Speed}));" };
        return Formula;
    }

#undef LUMINA_SHAPE_EDGE
#undef LUMINA_FRACTAL_NOISE_2D
#undef LUMINA_VALUE_NOISE_2D
}
