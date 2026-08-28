using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// An RGBA color (0..1). Converts implicitly to FVector4, so it drops into every engine API that takes a color.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("FVector4")]
public readonly struct Color
{
    public readonly float R;
    public readonly float G;
    public readonly float B;
    public readonly float A;

    public Color(float R, float G, float B, float A = 1.0f)
    {
        this.R = R;
        this.G = G;
        this.B = B;
        this.A = A;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Color WithAlpha(float Alpha) => new(R, G, B, Alpha);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator FVector4(Color C) => new(C.R, C.G, C.B, C.A);

    public static Color White => new(1.0f, 1.0f, 1.0f);
    public static Color Black => new(0.0f, 0.0f, 0.0f);
    public static Color Gray => new(0.5f, 0.5f, 0.5f);
    public static Color Red => new(1.0f, 0.0f, 0.0f);
    public static Color Green => new(0.0f, 1.0f, 0.0f);
    public static Color Blue => new(0.2f, 0.4f, 1.0f);
    public static Color Yellow => new(1.0f, 1.0f, 0.0f);
    public static Color Orange => new(1.0f, 0.5f, 0.0f);
    public static Color Cyan => new(0.0f, 1.0f, 1.0f);
    public static Color Magenta => new(1.0f, 0.0f, 1.0f);
}
