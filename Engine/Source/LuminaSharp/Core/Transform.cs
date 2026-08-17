using System.Runtime.InteropServices;

namespace Lumina;

// Native VTransform is NoCSharp: its three VFloat4 make 48 bytes, which the pad fields below reproduce.
[StructLayout(LayoutKind.Sequential)]
[LuminaSharp.NativeLayout("FTransform")]
public struct FTransform
{
    public FVector3 Location;   // @0
    private float Pad0;         // @12 (VFloat4 Location's unused w lane)
    public FQuat Rotation;      // @16
    public FVector3 Scale;      // @32
    private float Pad1;         // @44 (VFloat4 Scale's unused w lane)

    public FTransform(FVector3 Location, FQuat Rotation, FVector3 Scale)
    {
        this.Location = Location;
        Pad0 = 0.0f;
        this.Rotation = Rotation;
        this.Scale = Scale;
        Pad1 = 0.0f;
    }

    public static FTransform Identity => new(FVector3.Zero, FQuat.Identity, FVector3.One);
}
