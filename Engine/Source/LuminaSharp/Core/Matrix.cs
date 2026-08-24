using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Lumina;

// Blittable mirror of the engine's FMatrix4 (TMat<float, 4, 4>). COLUMN-MAJOR storage: the 16 floats are
// laid out column-by-column (C0.X, C0.Y, C0.Z, C0.W, C1.X, ...), identical to the native type, so this can
// be passed across interop without conversion. Conventions match C++ (Core/Math/Matrix): LEFT-HANDED,
// +Z forward, ZERO-TO-ONE clip depth. Math is ported 1:1 from MatrixMath.h. Do not "fix" the conventions.
// M[c] is column c; M[c, r] is the element at column c, row r. A transform is applied as M * V.

[StructLayout(LayoutKind.Sequential)]
[LuminaSharp.NativeLayout("FMatrix4")]
public struct FMatrix4 : IEquatable<FMatrix4>
{
    public FVector4 C0;
    public FVector4 C1;
    public FVector4 C2;
    public FVector4 C3;

    public FMatrix4(FVector4 C0, FVector4 C1, FVector4 C2, FVector4 C3)
    {
        this.C0 = C0;
        this.C1 = C1;
        this.C2 = C2;
        this.C3 = C3;
    }

    // Diagonal matrix (pass 1 for identity).
    public FMatrix4(float Diagonal)
    {
        C0 = new FVector4(Diagonal, 0.0f, 0.0f, 0.0f);
        C1 = new FVector4(0.0f, Diagonal, 0.0f, 0.0f);
        C2 = new FVector4(0.0f, 0.0f, Diagonal, 0.0f);
        C3 = new FVector4(0.0f, 0.0f, 0.0f, Diagonal);
    }

    public static FMatrix4 Identity => new(1.0f);
    public static FMatrix4 Zero => new(0.0f);

    // Column accessor (M[c] is column c).
    public FVector4 this[int Column]
    {
        get
        {
            switch (Column)
            {
                case 0: return C0;
                case 1: return C1;
                case 2: return C2;
                default: return C3;
            }
        }
        set
        {
            switch (Column)
            {
                case 0: C0 = value; break;
                case 1: C1 = value; break;
                case 2: C2 = value; break;
                default: C3 = value; break;
            }
        }
    }

    // Element accessor (column, row).
    public float this[int Column, int Row]
    {
        get => this[Column][Row];
        set
        {
            FVector4 Col = this[Column];
            Col[Row] = value;
            this[Column] = Col;
        }
    }

    public FVector4 GetColumn(int Column) => this[Column];
    public FVector4 GetRow(int Row) => new(C0[Row], C1[Row], C2[Row], C3[Row]);

    // Translation component (the fourth column's xyz).
    public FVector3 GetTranslation() => C3.XYZ;

    // M (R x C) * column vector -> column vector: linear combination of columns.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static FVector4 operator *(FMatrix4 M, FVector4 V)
    {
        return M.C0 * V.X + M.C1 * V.Y + M.C2 * V.Z + M.C3 * V.W;
    }

    public static FMatrix4 operator *(FMatrix4 A, FMatrix4 B)
    {
        return new FMatrix4(A * B.C0, A * B.C1, A * B.C2, A * B.C3);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static FMatrix4 operator *(FMatrix4 M, float S) => new(M.C0 * S, M.C1 * S, M.C2 * S, M.C3 * S);
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static FMatrix4 operator *(float S, FMatrix4 M) => M * S;
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static FMatrix4 operator +(FMatrix4 A, FMatrix4 B) => new(A.C0 + B.C0, A.C1 + B.C1, A.C2 + B.C2, A.C3 + B.C3);
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static FMatrix4 operator -(FMatrix4 A, FMatrix4 B) => new(A.C0 - B.C0, A.C1 - B.C1, A.C2 - B.C2, A.C3 - B.C3);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static bool operator ==(FMatrix4 A, FMatrix4 B) => A.C0 == B.C0 && A.C1 == B.C1 && A.C2 == B.C2 && A.C3 == B.C3;
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static bool operator !=(FMatrix4 A, FMatrix4 B) => !(A == B);

    // Transforms a point (implicit w = 1, with perspective divide if needed).
    public FVector3 TransformPoint(FVector3 P)
    {
        FVector4 R = this * new FVector4(P.X, P.Y, P.Z, 1.0f);
        if (R.W != 0.0f && R.W != 1.0f)
        {
            float Inv = 1.0f / R.W;
            return new FVector3(R.X * Inv, R.Y * Inv, R.Z * Inv);
        }
        return new FVector3(R.X, R.Y, R.Z);
    }

    // Transforms a direction (implicit w = 0; ignores translation).
    public FVector3 TransformDirection(FVector3 V)
    {
        FVector4 R = this * new FVector4(V.X, V.Y, V.Z, 0.0f);
        return new FVector3(R.X, R.Y, R.Z);
    }

    public FMatrix4 Transpose()
    {
        return new FMatrix4(
            new FVector4(C0.X, C1.X, C2.X, C3.X),
            new FVector4(C0.Y, C1.Y, C2.Y, C3.Y),
            new FVector4(C0.Z, C1.Z, C2.Z, C3.Z),
            new FVector4(C0.W, C1.W, C2.W, C3.W));
    }

    public float Determinant()
    {
        float S0 = C2.Z * C3.W - C3.Z * C2.W;
        float S1 = C2.Y * C3.W - C3.Y * C2.W;
        float S2 = C2.Y * C3.Z - C3.Y * C2.Z;
        float S3 = C2.X * C3.W - C3.X * C2.W;
        float S4 = C2.X * C3.Z - C3.X * C2.Z;
        float S5 = C2.X * C3.Y - C3.X * C2.Y;

        float Cof0 = C1.Y * S0 - C1.Z * S1 + C1.W * S2;
        float Cof1 = C1.X * S0 - C1.Z * S3 + C1.W * S4;
        float Cof2 = C1.X * S1 - C1.Y * S3 + C1.W * S5;
        float Cof3 = C1.X * S2 - C1.Y * S4 + C1.Z * S5;

        return C0.X * Cof0 - C0.Y * Cof1 + C0.Z * Cof2 - C0.W * Cof3;
    }

    // Full general 4x4 inverse (cofactor/adjugate). Ported 1:1 from C++ Math::Inverse.
    public FMatrix4 Inverse()
    {
        float C00 = C2.Z * C3.W - C3.Z * C2.W;
        float C02 = C1.Z * C3.W - C3.Z * C1.W;
        float C03 = C1.Z * C2.W - C2.Z * C1.W;

        float C04 = C2.Y * C3.W - C3.Y * C2.W;
        float C06 = C1.Y * C3.W - C3.Y * C1.W;
        float C07 = C1.Y * C2.W - C2.Y * C1.W;

        float C08 = C2.Y * C3.Z - C3.Y * C2.Z;
        float C10 = C1.Y * C3.Z - C3.Y * C1.Z;
        float C11 = C1.Y * C2.Z - C2.Y * C1.Z;

        float C12 = C2.X * C3.W - C3.X * C2.W;
        float C14 = C1.X * C3.W - C3.X * C1.W;
        float C15 = C1.X * C2.W - C2.X * C1.W;

        float C16 = C2.X * C3.Z - C3.X * C2.Z;
        float C18 = C1.X * C3.Z - C3.X * C1.Z;
        float C19 = C1.X * C2.Z - C2.X * C1.Z;

        float C20 = C2.X * C3.Y - C3.X * C2.Y;
        float C22 = C1.X * C3.Y - C3.X * C1.Y;
        float C23 = C1.X * C2.Y - C2.X * C1.Y;

        FVector4 F0 = new(C00, C00, C02, C03);
        FVector4 F1 = new(C04, C04, C06, C07);
        FVector4 F2 = new(C08, C08, C10, C11);
        FVector4 F3 = new(C12, C12, C14, C15);
        FVector4 F4 = new(C16, C16, C18, C19);
        FVector4 F5 = new(C20, C20, C22, C23);

        FVector4 V0 = new(C1.X, C0.X, C0.X, C0.X);
        FVector4 V1 = new(C1.Y, C0.Y, C0.Y, C0.Y);
        FVector4 V2 = new(C1.Z, C0.Z, C0.Z, C0.Z);
        FVector4 V3 = new(C1.W, C0.W, C0.W, C0.W);

        FVector4 I0 = V1 * F0 - V2 * F1 + V3 * F2;
        FVector4 I1 = V0 * F0 - V2 * F3 + V3 * F4;
        FVector4 I2 = V0 * F1 - V1 * F3 + V3 * F5;
        FVector4 I3 = V0 * F2 - V1 * F4 + V2 * F5;

        FVector4 SignA = new(+1.0f, -1.0f, +1.0f, -1.0f);
        FVector4 SignB = new(-1.0f, +1.0f, -1.0f, +1.0f);

        FVector4 Inv0 = I0 * SignA;
        FVector4 Inv1 = I1 * SignB;
        FVector4 Inv2 = I2 * SignA;
        FVector4 Inv3 = I3 * SignB;

        FVector4 Row0 = new(Inv0.X, Inv1.X, Inv2.X, Inv3.X);
        FVector4 Dot0 = C0 * Row0;
        float Dot1 = (Dot0.X + Dot0.Y) + (Dot0.Z + Dot0.W);
        float OneOverDet = 1.0f / Dot1;

        return new FMatrix4(Inv0 * OneOverDet, Inv1 * OneOverDet, Inv2 * OneOverDet, Inv3 * OneOverDet);
    }

    // Builders.
    public static FMatrix4 Translation(FVector3 T)
    {
        FMatrix4 M = Identity;
        M.C3 = new FVector4(T.X, T.Y, T.Z, 1.0f);
        return M;
    }

    public static FMatrix4 Scaling(FVector3 S)
    {
        return new FMatrix4(
            new FVector4(S.X, 0.0f, 0.0f, 0.0f),
            new FVector4(0.0f, S.Y, 0.0f, 0.0f),
            new FVector4(0.0f, 0.0f, S.Z, 0.0f),
            new FVector4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    // Rotation about a (normalized) axis by an angle in radians.
    public static FMatrix4 RotationAxis(float AngleRadians, FVector3 Axis)
    {
        float Cos = MathF.Cos(AngleRadians);
        float Sin = MathF.Sin(AngleRadians);
        FVector3 A = Axis.Normalized();
        FVector3 T = A * (1.0f - Cos);

        return new FMatrix4(
            new FVector4(Cos + T.X * A.X, T.X * A.Y + Sin * A.Z, T.X * A.Z - Sin * A.Y, 0.0f),
            new FVector4(T.Y * A.X - Sin * A.Z, Cos + T.Y * A.Y, T.Y * A.Z + Sin * A.X, 0.0f),
            new FVector4(T.Z * A.X + Sin * A.Y, T.Z * A.Y - Sin * A.X, Cos + T.Z * A.Z, 0.0f),
            new FVector4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    // Rotation matrix from a quaternion (the rotation in the upper-left 3x3).
    public static FMatrix4 FromQuat(FQuat Q)
    {
        float XX = Q.X * Q.X, YY = Q.Y * Q.Y, ZZ = Q.Z * Q.Z;
        float XZ = Q.X * Q.Z, XY = Q.X * Q.Y, YZ = Q.Y * Q.Z;
        float WX = Q.W * Q.X, WY = Q.W * Q.Y, WZ = Q.W * Q.Z;

        return new FMatrix4(
            new FVector4(1.0f - 2.0f * (YY + ZZ), 2.0f * (XY + WZ), 2.0f * (XZ - WY), 0.0f),
            new FVector4(2.0f * (XY - WZ), 1.0f - 2.0f * (XX + ZZ), 2.0f * (YZ + WX), 0.0f),
            new FVector4(2.0f * (XZ + WY), 2.0f * (YZ - WX), 1.0f - 2.0f * (XX + YY), 0.0f),
            new FVector4(0.0f, 0.0f, 0.0f, 1.0f));
    }

    // Translation * Rotation * Scale, composed directly (matches FTransform::GetMatrix).
    public static FMatrix4 TRS(FVector3 Translation, FQuat Rotation, FVector3 Scale)
    {
        FMatrix4 R = FromQuat(Rotation);
        return new FMatrix4(
            R.C0 * Scale.X,
            R.C1 * Scale.Y,
            R.C2 * Scale.Z,
            new FVector4(Translation.X, Translation.Y, Translation.Z, 1.0f));
    }

    // Rotation matrix -> quaternion (branch-by-largest-component).
    public FQuat ToQuat()
    {
        float M00 = C0.X, M01 = C0.Y, M02 = C0.Z;
        float M10 = C1.X, M11 = C1.Y, M12 = C1.Z;
        float M20 = C2.X, M21 = C2.Y, M22 = C2.Z;

        float FourWSq = M00 + M11 + M22;
        float FourXSq = M00 - M11 - M22;
        float FourYSq = M11 - M00 - M22;
        float FourZSq = M22 - M00 - M11;

        int Biggest = 0;
        float FourBiggest = FourWSq;
        if (FourXSq > FourBiggest) { FourBiggest = FourXSq; Biggest = 1; }
        if (FourYSq > FourBiggest) { FourBiggest = FourYSq; Biggest = 2; }
        if (FourZSq > FourBiggest) { FourBiggest = FourZSq; Biggest = 3; }

        float BiggestVal = MathF.Sqrt(FourBiggest + 1.0f) * 0.5f;
        float Mult = 0.25f / BiggestVal;

        switch (Biggest)
        {
            case 0: return new FQuat((M12 - M21) * Mult, (M20 - M02) * Mult, (M01 - M10) * Mult, BiggestVal);
            case 1: return new FQuat(BiggestVal, (M01 + M10) * Mult, (M20 + M02) * Mult, (M12 - M21) * Mult);
            case 2: return new FQuat((M01 + M10) * Mult, BiggestVal, (M12 + M21) * Mult, (M20 - M02) * Mult);
            default: return new FQuat((M20 + M02) * Mult, (M12 + M21) * Mult, BiggestVal, (M01 - M10) * Mult);
        }
    }

    // Affine TRS decompose (no skew/perspective, which is all the engine builds). False on singular.
    public bool Decompose(out FVector3 Scale, out FQuat Rotation, out FVector3 Translation)
    {
        Scale = FVector3.One;
        Rotation = FQuat.Identity;
        Translation = FVector3.Zero;

        if (MathF.Abs(C3.W) < 1e-8f)
        {
            return false;
        }

        Translation = C3.XYZ;

        FVector3 R0 = C0.XYZ;
        FVector3 R1 = C1.XYZ;
        FVector3 R2 = C2.XYZ;

        Scale = new FVector3(R0.Length, R1.Length, R2.Length);

        if (Scale.X > 0.0f) { R0 /= Scale.X; }
        if (Scale.Y > 0.0f) { R1 /= Scale.Y; }
        if (Scale.Z > 0.0f) { R2 /= Scale.Z; }

        // Negative determinant -> one axis is mirrored.
        if (FVector3.Dot(FVector3.Cross(R1, R2), R0) < 0.0f)
        {
            Scale.X = -Scale.X;
            R0 = -R0;
        }

        Rotation = new FMatrix4(new FVector4(R0, 0.0f), new FVector4(R1, 0.0f), new FVector4(R2, 0.0f), new FVector4(0, 0, 0, 1)).ToQuat();
        return true;
    }

    // Projection / view.
    // Left-handed perspective, zero-to-one depth.
    public static FMatrix4 Perspective(float FovYRadians, float Aspect, float Near, float Far)
    {
        float TanHalf = MathF.Tan(FovYRadians * 0.5f);
        FMatrix4 M = Zero;
        M.C0.X = 1.0f / (Aspect * TanHalf);
        M.C1.Y = 1.0f / TanHalf;
        M.C2.Z = Far / (Far - Near);
        M.C2.W = 1.0f;
        M.C3.Z = -(Far * Near) / (Far - Near);
        return M;
    }

    public static FMatrix4 Ortho(float Left, float Right, float Bottom, float Top, float Near, float Far)
    {
        FMatrix4 M = Identity;
        M.C0.X = 2.0f / (Right - Left);
        M.C1.Y = 2.0f / (Top - Bottom);
        M.C2.Z = 1.0f / (Far - Near);
        M.C3.X = -(Right + Left) / (Right - Left);
        M.C3.Y = -(Top + Bottom) / (Top - Bottom);
        M.C3.Z = -Near / (Far - Near);
        return M;
    }

    // Left-handed look-at view matrix.
    public static FMatrix4 LookAt(FVector3 Eye, FVector3 Center, FVector3 Up)
    {
        FVector3 F = (Center - Eye).Normalized();
        FVector3 S = FVector3.Cross(Up, F).Normalized();
        FVector3 U = FVector3.Cross(F, S);

        FMatrix4 M = Identity;
        M.C0.X = S.X; M.C1.X = S.Y; M.C2.X = S.Z;
        M.C0.Y = U.X; M.C1.Y = U.Y; M.C2.Y = U.Z;
        M.C0.Z = F.X; M.C1.Z = F.Y; M.C2.Z = F.Z;
        M.C3.X = -FVector3.Dot(S, Eye);
        M.C3.Y = -FVector3.Dot(U, Eye);
        M.C3.Z = -FVector3.Dot(F, Eye);
        return M;
    }

    public bool Equals(FMatrix4 Other) => this == Other;
    public override bool Equals(object? Obj) => Obj is FMatrix4 Other && Equals(Other);
    public override int GetHashCode() => HashCode.Combine(C0, C1, C2, C3);
    public override string ToString() => $"[{C0}, {C1}, {C2}, {C3}]";
}
