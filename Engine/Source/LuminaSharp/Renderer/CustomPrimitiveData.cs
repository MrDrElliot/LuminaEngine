using System;

namespace Lumina;

// The generated mirror is a plain value struct, so its typed accessors are written here to match CustomPrimitiveData.h bit for bit.
public partial struct SCustomPrimitiveData
{
    public float AsFloat() => BitConverter.UInt32BitsToSingle(Data.Packed);

    public int AsInt() => unchecked((int)Data.Packed);

    public bool AsBool() => Data.Packed != 0u;

    public FVector4 AsColor() => new FVector4(Data.Packed & 0xFFu, (Data.Packed >> 8) & 0xFFu, (Data.Packed >> 16) & 0xFFu, (Data.Packed >> 24) & 0xFFu) / 255.0f;

    public void SetAsFloat(float X)
    {
        Type = ECustomPrimitiveDataType.Float;
        Data.Packed = BitConverter.SingleToUInt32Bits(X);
    }

    public void SetAsInt(int X)
    {
        Type = ECustomPrimitiveDataType.Int;
        Data.Packed = unchecked((uint)X);
    }

    public void SetAsBool(bool X)
    {
        Type = ECustomPrimitiveDataType.Bool;
        Data.Packed = X ? 1u : 0u;
    }

    public void SetAsColor(FVector4 Color)
    {
        Type = ECustomPrimitiveDataType.Color;
        Data.Packed = Byte(Color.X) | (Byte(Color.Y) << 8) | (Byte(Color.Z) << 16) | (Byte(Color.W) << 24);
    }

    // The native side truncates the scaled channel into a byte, so rounding here would disagree by one step.
    private static uint Byte(float Channel) => (uint)(Math.Clamp(Channel, 0.0f, 1.0f) * 255.0f);
}
