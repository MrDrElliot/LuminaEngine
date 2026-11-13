#pragma once

#include "Core/Object/ObjectMacros.h"

#include <bit>

#include "CustomPrimitiveData.generated.h"

namespace Lumina
{
    REFLECT()
    enum class ECustomPrimitiveDataType : uint8
    {
        Float,
        Int,
        UInt,
        Color,
        Bool,
    };
    
    // One 32 bit slot that every type reads and writes whole, so a narrow write cannot leave stale bytes.
    REFLECT()
    struct RUNTIME_API SCustomPrimitiveDataUnion
    {
        GENERATED_BODY()

        // Reflected so prefab override diffs and editing tools see the value, not only its type.
        PROPERTY(Editable)
        uint32 Packed = 0;

        float      AsFloat() const { return std::bit_cast<float>(Packed); }
        int32      AsInt()   const { return std::bit_cast<int32>(Packed); }
        uint32     AsUInt()  const { return Packed; }
        bool       AsBool()  const { return Packed != 0u; }

        FU8Vector4 AsBytes() const
        {
            return FU8Vector4((uint8)(Packed & 0xFFu), (uint8)((Packed >> 8) & 0xFFu),
                              (uint8)((Packed >> 16) & 0xFFu), (uint8)((Packed >> 24) & 0xFFu));
        }

        static SCustomPrimitiveDataUnion FromFloat(float Value)  { return { std::bit_cast<uint32>(Value) }; }
        static SCustomPrimitiveDataUnion FromInt(int32 Value)    { return { std::bit_cast<uint32>(Value) }; }
        static SCustomPrimitiveDataUnion FromUInt(uint32 Value)  { return { Value }; }
        static SCustomPrimitiveDataUnion FromBool(bool Value)    { return { Value ? 1u : 0u }; }

        static SCustomPrimitiveDataUnion FromColor(FU8Vector4 Bytes)
        {
            return { (uint32)Bytes.x | ((uint32)Bytes.y << 8) | ((uint32)Bytes.z << 16) | ((uint32)Bytes.w << 24) };
        }
    };
    
    REFLECT()
    struct RUNTIME_API SCustomPrimitiveData
    {
        GENERATED_BODY()
        
        PROPERTY(Editable)
        ECustomPrimitiveDataType Type = ECustomPrimitiveDataType::Float;

        PROPERTY(Editable)
        SCustomPrimitiveDataUnion Data;
        
        bool Serialize(FArchive& Ar)
        {
            Ar << Type;

            // Every other type went over the wire as its own 4 bytes, which is what the packed word holds.
            if (Type == ECustomPrimitiveDataType::Color)
            {
                FU8Vector4 Bytes = Data.AsBytes();
                Ar << Bytes;
                Data = SCustomPrimitiveDataUnion::FromColor(Bytes);
            }
            else
            {
                Ar << Data.Packed;
            }

            return true;
        }

        FUNCTION()
        float AsFloat() const { return Data.AsFloat(); }

        FUNCTION()
        int32 AsInt() const { return Data.AsInt(); }

        FUNCTION()
        bool AsBool() const { return Data.AsBool(); }

        FUNCTION()
        FVector4 AsColor() const { return FVector4(Data.AsBytes()) / 255.0f; }

        FUNCTION()
        void SetAsFloat(float X)
        {
            Type = ECustomPrimitiveDataType::Float;
            Data = SCustomPrimitiveDataUnion::FromFloat(X);
        }

        FUNCTION()
        void SetAsInt(int32 X)
        {
            Type = ECustomPrimitiveDataType::Int;
            Data = SCustomPrimitiveDataUnion::FromInt(X);
        }

        FUNCTION()
        void SetAsBool(bool X)
        {
            Type = ECustomPrimitiveDataType::Bool;
            Data = SCustomPrimitiveDataUnion::FromBool(X);
        }

        FUNCTION()
        void SetAsColor(FVector4 Color)
        {
            Type = ECustomPrimitiveDataType::Color;
            Data = SCustomPrimitiveDataUnion::FromColor(FU8Vector4(Math::Clamp(Color, 0.0f, 1.0f) * 255.0f));
        }
    };
}
