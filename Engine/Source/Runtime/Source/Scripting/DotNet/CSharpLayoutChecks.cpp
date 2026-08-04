#include "RuntimePCH.h"

#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Math/Matrix/Matrix.h"
#include "Core/Math/Transform.h"
#include "Containers/String.h"
#include "Containers/Array.h"
#include "Containers/ContainerOps.h"
#include "Core/Assertions/Assert.h"
#include "Input/InputAction.h"
#include "Scripting/DotNet/LayoutRegistry.h"

namespace Lumina
{
    // The hand-written LuminaSharp math value types (Math.cs / Matrix.cs) are [StructLayout(Sequential)]
    // mirrors of these. They're ManualStub, so the Reflector doesn't emit them or an auto size assert;
    // these guards catch a native size drift at compile time. (Reflected blittable value mirrors, e.g.
    // SPerceptionEvent / SCollisionEvent, get their size assert auto-emitted into the generated bindings.)
    static_assert(sizeof(FVector2) == 8,  "LuminaSharp FVector2 mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FVector3) == 12, "LuminaSharp FVector3 mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FVector4) == 16, "LuminaSharp FVector4 mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FQuat)    == 16, "LuminaSharp FQuat mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FMatrix4) == 64, "LuminaSharp FMatrix mirror size mismatch (update Matrix.cs).");

    // The script layer reads a whole array of these through a raw pointer (the Lumina.FInputActionState
    // mirror), so a field added on either side without the other silently misreads every action's state.
    static_assert(sizeof(FInputActionState)              == 16, "LuminaSharp FInputActionState mirror size mismatch (update InputActionState.cs).");
    static_assert(offsetof(FInputActionState, X)         == 0,  "InputActionState.X offset mismatch.");
    static_assert(offsetof(FInputActionState, Y)         == 4,  "InputActionState.Y offset mismatch.");
    static_assert(offsetof(FInputActionState, HeldTime)  == 8,  "InputActionState.HeldTime offset mismatch.");
    static_assert(offsetof(FInputActionState, Flags)     == 12, "InputActionState.Flags offset mismatch.");

    // SIMD FTransform (VTransform): three 16-byte VFloat4 (Location.xyz+pad, Rotation.xyzw, Scale.xyz+pad).
    // The hand-written C# mirror (Transform.cs) reproduces this padded 48-byte layout for the by-value blit.
    static_assert(sizeof(FTransform)              == 48, "LuminaSharp FTransform mirror size mismatch (update Transform.cs).");
    static_assert(offsetof(FTransform, Location)  == 0,  "FTransform.Location offset mismatch.");
    static_assert(offsetof(FTransform, Rotation)  == 16, "FTransform.Rotation offset mismatch.");
    static_assert(offsetof(FTransform, Scale)     == 32, "FTransform.Scale offset mismatch.");

    // Runtime cross-check: report each math mirror's native sizeof so the managed LayoutValidator can compare
    // it against Unsafe.SizeOf<T>() at bootstrap (keys match the C# [LuminaSharp.NativeLayout("...")]).
    LE_REGISTER_LAYOUT("FVector2",     FVector2);
    LE_REGISTER_LAYOUT("FVector3",     FVector3);
    LE_REGISTER_LAYOUT("FVector4",     FVector4);
    LE_REGISTER_LAYOUT("FQuat",        FQuat);
    LE_REGISTER_LAYOUT("FMatrix",      FMatrix4);
    LE_REGISTER_LAYOUT("FTransform",   FTransform);
    LE_REGISTER_LAYOUT("FInputActionState", FInputActionState);
    LE_REGISTER_LAYOUT("FUIntVector2", FUIntVector2);
    LE_REGISTER_LAYOUT("FUIntVector3", FUIntVector3);
    LE_REGISTER_LAYOUT("FIntVector2",  FIntVector2);
    LE_REGISTER_LAYOUT("FIntVector3",  FIntVector3);

    // LuminaSharp.NativeMarshal reads FString / TVector<T> in place from C# by hard-coding the EASTL byte
    // layout (SSO flag at byte 23, heap ptr@0 / size@8; vector mpBegin@0 / mpEnd@8). size_type is 64-bit.
    static_assert(sizeof(size_t) == 8, "NativeMarshal assumes a 64-bit eastl size_type.");

    // LuminaSharp.VectorOps (NativeList.cs) overlays FVectorOps and calls these three by offset.
    static_assert(offsetof(FVectorOps, PushBack) == 16, "VectorOps.PushBack offset drift (update NativeList.cs).");
    static_assert(offsetof(FVectorOps, RemoveAt) == 24, "VectorOps.RemoveAt offset drift (update NativeList.cs).");
    static_assert(offsetof(FVectorOps, Clear)    == 32, "VectorOps.Clear offset drift (update NativeList.cs).");

#if defined(LE_DEBUG) || defined(LE_DEVELOPMENT)
    namespace
    {
        // Replays NativeMarshal.ReadString's decode against the real eastl accessors. A layout/config drift
        // (SSO flag offset, heap ptr/size offsets, endianness) is caught here at host init rather than
        // silently corrupting a managed string field.
        bool FStringDecodeMatches(const FString& S)
        {
            const uint8* Base = reinterpret_cast<const uint8*>(&S);
            const uint8 Flag = Base[23];
            const char* Data;
            size_t Length;
            if (Flag & 0x80)
            {
                Data = *reinterpret_cast<const char* const*>(Base);
                Length = *reinterpret_cast<const size_t*>(Base + 8);
            }
            else
            {
                Data = reinterpret_cast<const char*>(Base);
                Length = static_cast<size_t>(23 - Flag);
            }
            return Data == S.data() && Length == S.length();
        }
    }

    // Called once from the C# host bootstrap (DotNetHost::Initialize) before any managed code reads a
    // reflected FString/TVector property through the zero-crossing NativeMarshal path.
    void VerifyEASTLInteropLayout()
    {
        const FString Empty;
        const FString SSO = "short";
        const FString Heap = "this string is comfortably longer than the twenty-three character SSO buffer";
        LUMINA_DEBUG_ASSERT(FStringDecodeMatches(Empty), "NativeMarshal FString (empty) layout drift.");
        LUMINA_DEBUG_ASSERT(FStringDecodeMatches(SSO),   "NativeMarshal FString (SSO) layout drift.");
        LUMINA_DEBUG_ASSERT(FStringDecodeMatches(Heap),  "NativeMarshal FString (heap) layout drift.");

        TVector<int32> V;
        V.push_back(10);
        V.push_back(20);
        V.push_back(30);
        const uint8* H = reinterpret_cast<const uint8*>(&V);
        const int32* Begin = *reinterpret_cast<const int32* const*>(H);
        const int32* End = *reinterpret_cast<const int32* const*>(H + sizeof(void*));
        LUMINA_DEBUG_ASSERT(Begin == V.data(), "NativeMarshal TVector mpBegin offset drift.");
        LUMINA_DEBUG_ASSERT(static_cast<size_t>(End - Begin) == V.size(), "NativeMarshal TVector mpEnd offset drift.");
    }
#else
    void VerifyEASTLInteropLayout() {}
#endif
}
