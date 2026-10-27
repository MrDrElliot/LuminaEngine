#include "SlangLanguage.h"

namespace Lumina
{
    namespace SlangLanguageData
    {
        constexpr const char* Keywords[] =
        {
            "break", "case", "cbuffer", "centroid", "class", "column_major", "const", "continue", "default", "discard",
            "do", "else", "export", "extern", "false", "for", "groupshared", "if", "in", "inline", "inout", "interface",
            "linear", "namespace", "nointerpolation", "noperspective", "out", "packoffset", "precise", "register",
            "return", "row_major", "sample", "shared", "static", "struct", "switch", "true", "typedef", "uniform",
            "volatile", "while", "__generic", "__init", "__subscript", "__exported", "associatedtype", "extension",
            "property", "typealias", "func", "let", "var", "enum", "is", "as", "where", "This", "no_diff",
            "differentiable", "bwd_diff", "fwd_diff", "module", "import", "implementing", "public", "internal",
            "private", "override", "each", "expand", "none", "dynamic_uniform", "reinterpret", "spirv_asm",
        };

        constexpr const char* Types[] =
        {
            "void", "bool", "int", "uint", "float", "half", "double",
            "bool2", "bool3", "bool4", "int2", "int3", "int4", "uint2", "uint3", "uint4",
            "float2", "float3", "float4", "half2", "half3", "half4", "double2", "double3", "double4",
            "float2x2", "float3x3", "float4x4", "float3x4", "float4x3", "half3x3", "half4x4",
            "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
            "float16_t", "float32_t", "float64_t", "matrix", "vector",
            "Texture1D", "Texture2D", "Texture3D", "TextureCube", "Texture2DArray", "RWTexture2D", "RWTexture3D",
            "Buffer", "RWBuffer", "StructuredBuffer", "RWStructuredBuffer", "ByteAddressBuffer", "RWByteAddressBuffer",
            "ConstantBuffer", "ParameterBlock", "SamplerState", "SamplerComparisonState", "Ptr", "Optional",
        };

        constexpr FCodeSymbol Intrinsics[] =
        {
            { "abs", "abs(x)", "Absolute value, per component." },
            { "acos", "acos(x)", "Arc cosine in radians." },
            { "all", "all(x)", "True when every component is nonzero." },
            { "any", "any(x)", "True when any component is nonzero." },
            { "asin", "asin(x)", "Arc sine in radians." },
            { "atan", "atan(x)", "Arc tangent in radians." },
            { "atan2", "atan2(y, x)", "Arc tangent of y over x, using both signs to pick the quadrant." },
            { "ceil", "ceil(x)", "Smallest integer not below x." },
            { "clamp", "clamp(x, min, max)", "Limits x to the range from min to max." },
            { "cos", "cos(x)", "Cosine of an angle in radians." },
            { "cosh", "cosh(x)", "Hyperbolic cosine." },
            { "cross", "cross(a, b)", "Cross product of two float3 vectors." },
            { "ddx", "ddx(x)", "Screen space derivative along x, from the 2x2 pixel quad." },
            { "ddy", "ddy(x)", "Screen space derivative along y, from the 2x2 pixel quad." },
            { "degrees", "degrees(x)", "Converts radians to degrees." },
            { "determinant", "determinant(m)", "Determinant of a square matrix." },
            { "distance", "distance(a, b)", "Distance between two points." },
            { "dot", "dot(a, b)", "Dot product." },
            { "exp", "exp(x)", "e raised to x." },
            { "exp2", "exp2(x)", "2 raised to x." },
            { "faceforward", "faceforward(n, i, ng)", "Flips n to face away from the incident vector i." },
            { "floor", "floor(x)", "Largest integer not above x." },
            { "fmod", "fmod(x, y)", "Floating point remainder of x divided by y, with the sign of x." },
            { "frac", "frac(x)", "Fractional part, x minus floor(x)." },
            { "frexp", "frexp(x, exp)", "Splits x into a mantissa and an exponent." },
            { "fwidth", "fwidth(x)", "abs(ddx(x)) + abs(ddy(x)), the change across one pixel." },
            { "isfinite", "isfinite(x)", "True for a finite value." },
            { "isinf", "isinf(x)", "True for an infinite value." },
            { "isnan", "isnan(x)", "True for NaN." },
            { "ldexp", "ldexp(x, exp)", "x times 2 raised to exp." },
            { "length", "length(v)", "Length of a vector." },
            { "lerp", "lerp(a, b, t)", "Linear interpolation, a + t * (b - a)." },
            { "log", "log(x)", "Natural logarithm." },
            { "log2", "log2(x)", "Base 2 logarithm." },
            { "log10", "log10(x)", "Base 10 logarithm." },
            { "mad", "mad(a, b, c)", "a * b + c in a single instruction." },
            { "max", "max(a, b)", "Larger of two values, per component." },
            { "min", "min(a, b)", "Smaller of two values, per component." },
            { "modf", "modf(x, ip)", "Splits x into fractional and integer parts." },
            { "mul", "mul(a, b)", "Matrix and vector multiplication." },
            { "normalize", "normalize(v)", "Vector scaled to unit length." },
            { "pow", "pow(x, y)", "x raised to y. x must not be negative." },
            { "radians", "radians(x)", "Converts degrees to radians." },
            { "rcp", "rcp(x)", "Fast approximate reciprocal." },
            { "reflect", "reflect(i, n)", "Reflects the incident vector i about the normal n." },
            { "refract", "refract(i, n, eta)", "Refracts i through a surface with normal n and index ratio eta." },
            { "round", "round(x)", "Nearest integer." },
            { "rsqrt", "rsqrt(x)", "Reciprocal square root." },
            { "saturate", "saturate(x)", "Clamps to the range 0 to 1." },
            { "sign", "sign(x)", "-1, 0 or 1 by the sign of x." },
            { "sin", "sin(x)", "Sine of an angle in radians." },
            { "sincos", "sincos(x, s, c)", "Sine and cosine of x at once." },
            { "sinh", "sinh(x)", "Hyperbolic sine." },
            { "smoothstep", "smoothstep(min, max, x)", "Smooth Hermite step from 0 at min to 1 at max." },
            { "sqrt", "sqrt(x)", "Square root." },
            { "step", "step(edge, x)", "0 when x is below edge, otherwise 1." },
            { "tan", "tan(x)", "Tangent of an angle in radians." },
            { "tanh", "tanh(x)", "Hyperbolic tangent." },
            { "transpose", "transpose(m)", "Transposed matrix." },
            { "trunc", "trunc(x)", "Integer part, rounding toward zero." },
            { "clip", "clip(x)", "Discards the pixel when any component of x is negative." },
            { "countbits", "countbits(x)", "Number of set bits." },
            { "firstbithigh", "firstbithigh(x)", "Index of the highest set bit." },
            { "firstbitlow", "firstbitlow(x)", "Index of the lowest set bit." },
            { "reversebits", "reversebits(x)", "Bits in reverse order." },
            { "asfloat", "asfloat(x)", "Reinterprets bits as float." },
            { "asint", "asint(x)", "Reinterprets bits as int." },
            { "asuint", "asuint(x)", "Reinterprets bits as uint." },
            { "f16tof32", "f16tof32(x)", "Unpacks a half in the low 16 bits to float." },
            { "f32tof16", "f32tof16(x)", "Packs a float into the low 16 bits as half." },
            { "select", "select(condition, a, b)", "a where condition is true, b elsewhere, per component." },
            { "InterlockedAdd", "InterlockedAdd(dest, value, original)", "Atomically adds value to dest." },
            { "InterlockedOr", "InterlockedOr(dest, value, original)", "Atomically ors value into dest." },
            { "GroupMemoryBarrierWithGroupSync", "GroupMemoryBarrierWithGroupSync()", "Waits for every thread of the group and for group shared writes." },
            { "AllMemoryBarrierWithGroupSync", "AllMemoryBarrierWithGroupSync()", "Waits for every thread of the group and for all memory writes." },
            { "WaveActiveSum", "WaveActiveSum(x)", "Sum of x across the active lanes." },
            { "WaveActiveMax", "WaveActiveMax(x)", "Largest x across the active lanes." },
            { "WaveActiveMin", "WaveActiveMin(x)", "Smallest x across the active lanes." },
            { "WaveActiveCountBits", "WaveActiveCountBits(b)", "Number of active lanes where b is true." },
            { "WavePrefixCountBits", "WavePrefixCountBits(b)", "Lanes below this one where b is true." },
            { "WaveReadLaneFirst", "WaveReadLaneFirst(x)", "x from the first active lane." },
            { "WaveIsFirstLane", "WaveIsFirstLane()", "True on the first active lane." },
            { "WaveGetLaneIndex", "WaveGetLaneIndex()", "Index of this lane in the wave." },
            { "SampleTexture2D", "SampleTexture2D(uint TextureIndex, uint SamplerIndex, float2 UV)", "Samples a bindless 2D texture." },
            { "SampleTexture2DGrad", "SampleTexture2DGrad(uint TextureIndex, uint SamplerIndex, float2 UV, float2 Ddx, float2 Ddy)", "Samples a bindless 2D texture with explicit derivatives." },
            { "SampleTexture2DLevel", "SampleTexture2DLevel(uint TextureIndex, uint SamplerIndex, float2 UV, float Lod)", "Samples one mip level of a bindless 2D texture." },
            { "SampleTexture2DArray", "SampleTexture2DArray(uint TextureIndex, uint SamplerIndex, float2 UV, uint Slice)", "Samples one slice of a bindless texture array." },
            { "SampleTextureCube", "SampleTextureCube(uint TextureIndex, uint SamplerIndex, float3 Dir)", "Samples a bindless cube map along a direction." },
            { "GetMaterialTexture", "GetMaterialTexture(uint MatIndex, uint Index)", "Bindless index of the material's texture parameter at Index." },
            { "GetMaterialScalar", "GetMaterialScalar(uint MatIndex, uint Index)", "Value of the material's scalar parameter at Index." },
            { "GetMaterialVec4", "GetMaterialVec4(uint MatIndex, uint Index)", "Value of the material's vector parameter at Index." },
            { "GetMaterialMipBias", "GetMaterialMipBias()", "Mip bias the material samples with." },
            { "GetCameraPosition", "GetCameraPosition()", "World space camera position." },
            { "GetCameraForward", "GetCameraForward()", "World space camera forward direction." },
            { "GetTime", "GetTime()", "Seconds since the world started." },
            { "Scene", "Scene()", "The per-frame scene constants." },
        };

        constexpr FCodeSymbol PixelLocals[] =
        {
            { "MaterialIndex", "uint MaterialIndex", "Index of this material, passed to GetMaterialTexture and GetMaterialScalar." },
            { "EntityID", "uint EntityID", "Id of the entity being drawn." },
            { "UV0", "float2 UV0", "First texture coordinate set." },
            { "UV1", "float2 UV1", "Second texture coordinate set." },
            { "UV0_DDX", "float2 UV0_DDX", "ddx(UV0), for SampleTexture2DGrad." },
            { "UV0_DDY", "float2 UV0_DDY", "ddy(UV0), for SampleTexture2DGrad." },
            { "UV1_DDX", "float2 UV1_DDX", "ddx(UV1)." },
            { "UV1_DDY", "float2 UV1_DDY", "ddy(UV1)." },
            { "WorldPosition", "float3 WorldPosition", "World space position of the pixel." },
            { "WorldPositionExcludingOffsets", "float3 WorldPositionExcludingOffsets", "World space position before world position offset." },
            { "WorldPosition_DDX", "float3 WorldPosition_DDX", "ddx(WorldPosition)." },
            { "WorldPosition_DDY", "float3 WorldPosition_DDY", "ddy(WorldPosition)." },
            { "WorldNormal", "float3 WorldNormal", "Normalized world space vertex normal." },
            { "WorldTangent", "float4 WorldTangent", "World space tangent, with the bitangent sign in w." },
            { "ViewPosition", "float3 ViewPosition", "View space position of the pixel." },
            { "VertexColor", "float4 VertexColor", "Interpolated vertex color." },
            { "VertexColor_DDX", "float4 VertexColor_DDX", "ddx(VertexColor)." },
            { "VertexColor_DDY", "float4 VertexColor_DDY", "ddy(VertexColor)." },
        };
    }

    FSlangLanguage::FSlangLanguage()
    {
        AddKeywords(SlangLanguageData::Keywords);
        AddTypes(SlangLanguageData::Types);
        AddFunctions(SlangLanguageData::Intrinsics);
        AddKnownIdentifiers(SlangLanguageData::PixelLocals);
        AddSnippet("if", "if (${1:condition})\n{\n\t$0\n}", "if block");
        AddSnippet("for", "for (int i = 0; i < ${1:Count}; ++i)\n{\n\t$0\n}", "for loop");
        AddSnippet("sample", "SampleTexture2D(GetMaterialTexture(MaterialIndex, ${1:0}), ${2:0}, ${3:UV0})$0", "sample a material texture");
    }
}
