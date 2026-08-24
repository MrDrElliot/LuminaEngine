#include "RuntimePCH.h"

#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Math/Matrix/Matrix.h"
#include "Core/Math/Transform.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Containers/ContainerOps.h"
#include "Core/Assertions/Assert.h"
#include "Input/InputAction.h"
#include "Scripting/DotNet/LayoutRegistry.h"

namespace Lumina
{
    // CSharpValueMirror suppresses the auto-emitted size assert, so these guard Math.cs and Matrix.cs.
    static_assert(sizeof(FVector2) == 8,  "LuminaSharp FVector2 mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FVector3) == 12, "LuminaSharp FVector3 mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FVector4) == 16, "LuminaSharp FVector4 mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FQuat)    == 16, "LuminaSharp FQuat mirror size mismatch (update Math.cs).");
    static_assert(sizeof(FMatrix4) == 64, "LuminaSharp FMatrix4 mirror size mismatch (update Matrix.cs).");

    // A field added on either side without the other silently misreads every action's state.
    static_assert(sizeof(FInputActionState)              == 16, "LuminaSharp FInputActionState mirror size mismatch (update InputActionState.cs).");
    static_assert(offsetof(FInputActionState, X)         == 0,  "InputActionState.X offset mismatch.");
    static_assert(offsetof(FInputActionState, Y)         == 4,  "InputActionState.Y offset mismatch.");
    static_assert(offsetof(FInputActionState, HeldTime)  == 8,  "InputActionState.HeldTime offset mismatch.");
    static_assert(offsetof(FInputActionState, Flags)     == 12, "InputActionState.Flags offset mismatch.");

    // The hand-written C# mirror reproduces this padded layout for the by-value blit.
    static_assert(sizeof(FTransform)              == 48, "LuminaSharp FTransform mirror size mismatch (update Transform.cs).");
    static_assert(offsetof(FTransform, Location)  == 0,  "FTransform.Location offset mismatch.");
    static_assert(offsetof(FTransform, Rotation)  == 16, "FTransform.Rotation offset mismatch.");
    static_assert(offsetof(FTransform, Scale)     == 32, "FTransform.Scale offset mismatch.");

    // Reports each mirror's native size so the managed validator can compare it at bootstrap.
    LE_REGISTER_LAYOUT("FVector2",     FVector2);
    LE_REGISTER_LAYOUT("FVector3",     FVector3);
    LE_REGISTER_LAYOUT("FVector4",     FVector4);
    LE_REGISTER_LAYOUT("FQuat",        FQuat);
    LE_REGISTER_LAYOUT("FMatrix4",     FMatrix4);
    LE_REGISTER_LAYOUT("FTransform",   FTransform);
    // Stops a field added on either side from silently shifting every element of an FName container.
    LE_REGISTER_LAYOUT("FName",        FName);
    LE_REGISTER_LAYOUT("FInputActionState", FInputActionState);
    LE_REGISTER_LAYOUT("FUIntVector2", FUIntVector2);
    LE_REGISTER_LAYOUT("FUIntVector3", FUIntVector3);
    LE_REGISTER_LAYOUT("FIntVector2",  FIntVector2);
    LE_REGISTER_LAYOUT("FIntVector3",  FIntVector3);

    // The managed marshal reads these containers in place by hard-coding their byte layout.
    static_assert(sizeof(size_t) == 8, "NativeMarshal assumes a 64-bit pointer.");
    static_assert(sizeof(FString) == 16, "NativeMarshal assumes a 16-byte FString (update NativeMarshal.cs).");
    static_assert(sizeof(TVector<int32>) == 16, "NativeMarshal assumes a 16-byte TVector (update NativeMarshal.cs).");

    // LuminaSharp.VectorOps (TVector.cs) overlays FVectorOps and calls these three by offset.
    static_assert(offsetof(FVectorOps, PushBack) == 16, "VectorOps.PushBack offset drift (update TVector.cs).");
    static_assert(offsetof(FVectorOps, RemoveAt) == 24, "VectorOps.RemoveAt offset drift (update TVector.cs).");
    static_assert(offsetof(FVectorOps, Clear)    == 32, "VectorOps.Clear offset drift (update TVector.cs).");

#if defined(LE_DEBUG) || defined(LE_DEVELOPMENT)
    namespace
    {
        // A layout drift is caught at host init rather than silently corrupting a managed string field.
        bool FStringDecodeMatches(const FString& S)
        {
            const uint8* Base = reinterpret_cast<const uint8*>(&S);
            const uint8 Flag = Base[15];
            const char* Data;
            size_t Length;
            if (Flag & 0x80)
            {
                Data = *reinterpret_cast<const char* const*>(Base);
                Length = *reinterpret_cast<const uint32*>(Base + 8);
            }
            else
            {
                Data = reinterpret_cast<const char*>(Base);
                Length = static_cast<size_t>(15 - Flag);
            }
            return Data == S.data() && Length == S.length();
        }
    }

    // Called once from the host bootstrap before any managed code reads a container property.
    void VerifyContainerInteropLayout()
    {
        const FString Empty;
        const FString SSO = "short";
        const FString Heap = "this string is comfortably longer than the fifteen character inline buffer";
        LUMINA_DEBUG_ASSERT(FStringDecodeMatches(Empty), "NativeMarshal FString (empty) layout drift.");
        LUMINA_DEBUG_ASSERT(FStringDecodeMatches(SSO),   "NativeMarshal FString (SSO) layout drift.");
        LUMINA_DEBUG_ASSERT(FStringDecodeMatches(Heap),  "NativeMarshal FString (heap) layout drift.");

        TVector<int32> V;
        V.push_back(10);
        V.push_back(20);
        V.push_back(30);
        const uint8* H = reinterpret_cast<const uint8*>(&V);
        const int32* Begin = *reinterpret_cast<const int32* const*>(H);
        const uint32 Length = *reinterpret_cast<const uint32*>(H + sizeof(void*));
        LUMINA_DEBUG_ASSERT(Begin == V.data(), "NativeMarshal TVector data offset drift.");
        LUMINA_DEBUG_ASSERT(static_cast<size_t>(Length) == V.size(), "NativeMarshal TVector count offset drift.");
    }
#else
    void VerifyContainerInteropLayout() {}
#endif
}
