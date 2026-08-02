#pragma once

#include "Core/Object/ObjectMacros.h"
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
    
    union ECustomPrimitiveDataUnion
    {
        uint32      Packed = 0;
        float       Float;
        int32       Int;
        uint32      UInt;
        bool        Bool;
        FU8Vector4 Bytes;

        static ECustomPrimitiveDataUnion FromFloat(float Value)
        {
            ECustomPrimitiveDataUnion D;
            D.Float = Value;
            return D;
        }

        static ECustomPrimitiveDataUnion FromInt(int32 Value)
        {
            ECustomPrimitiveDataUnion D;
            D.Int = Value;
            return D;
        }

        static ECustomPrimitiveDataUnion FromUInt(uint32 Value)
        {
            ECustomPrimitiveDataUnion D;
            D.UInt = Value;
            return D;
        }

        static ECustomPrimitiveDataUnion FromBool(bool Value)
        {
            ECustomPrimitiveDataUnion D;
            D.UInt = Value ? 1u : 0u;
            return D;
        }

        static ECustomPrimitiveDataUnion FromColor(FU8Vector4 Bytes)
        {
            ECustomPrimitiveDataUnion D;
            D.Bytes = Bytes;
            return D;
        }
    }; 
    
    REFLECT()
    struct RUNTIME_API SCustomPrimitiveData
    {
        GENERATED_BODY()
        
        PROPERTY(Script, Editable)
        ECustomPrimitiveDataType Type = ECustomPrimitiveDataType::Float;

        ECustomPrimitiveDataUnion Data;
        
        bool Serialize(FArchive& Ar)
        {
            Ar << Type;

            switch (Type)
            {
                case ECustomPrimitiveDataType::Float:   Ar << Data.Float; break;
                case ECustomPrimitiveDataType::Int:     Ar << Data.Int;   break;
                case ECustomPrimitiveDataType::UInt:    Ar << Data.UInt;  break;
                case ECustomPrimitiveDataType::Bool:    Ar << Data.UInt;  break;
                case ECustomPrimitiveDataType::Color:   Ar << Data.Bytes; break;
            }

            return true;
        }
        
        FUNCTION(Script)
        float AsFloat() const { return Data.Float; }
        
        FUNCTION(Script)
        int32 AsInt() const { return Data.Int; }
        
        FUNCTION(Script)
        bool AsBool() const { return Data.Bool; }
        
        FUNCTION(Script)
        FVector4 AsColor() const { return FVector4(Data.Bytes) / 255.0f; }
        
        FUNCTION(Script)
        void SetAsFloat(float X)
        {
            Type = ECustomPrimitiveDataType::Float;
            Data.Float = X;
        }
        
        FUNCTION(Script)
        void SetAsInt(int32 X)
        {
            Type = ECustomPrimitiveDataType::Int;
            Data.Int = X;
        }
        
        FUNCTION(Script)
        void SetAsBool(bool X)
        {
            Type = ECustomPrimitiveDataType::Bool;
            Data.Bool = X;
        }
        
        FUNCTION(Script)
        void SetAsColor(FVector4 Color)
        {
            Type = ECustomPrimitiveDataType::Color;
            Data.Bytes = FU8Vector4(Math::Clamp(Color, 0.0f, 1.0f) * 255.0f);
        }
    };
}
