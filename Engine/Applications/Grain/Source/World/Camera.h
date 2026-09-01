#pragma once

#include "VoxelTypes.h"

namespace Grain
{
    class FCamera
    {
    public:

        void SetPosition(const FVector3& InPosition) { Position = InPosition; }

        void Look(float DeltaX, float DeltaY)
        {
            Yaw += DeltaX * 0.0026f;
            Pitch = Math::Clamp(Pitch - DeltaY * 0.0026f, -1.53f, 1.53f);
        }

        void Move(const FVector3& Local, float Delta, bool bFast)
        {
            const FVector3 F = Forward();
            const FVector3 R = Right();
            const float Speed = (bFast ? 46.0f : 11.0f) * Delta;

            Position.x += (F.x * Local.z + R.x * Local.x) * Speed;
            Position.y += (F.y * Local.z + Local.y) * Speed;
            Position.z += (F.z * Local.z + R.z * Local.x) * Speed;

            Position.x = Math::Clamp(Position.x, 1.0f, kWorldSizeX - 1.0f);
            Position.y = Math::Clamp(Position.y, 1.0f, kWorldSizeY - 1.0f);
            Position.z = Math::Clamp(Position.z, 1.0f, kWorldSizeZ - 1.0f);
        }

        NODISCARD FVector3 Forward() const
        {
            const float CosPitch = Math::Cos(Pitch);
            return { Math::Cos(Yaw) * CosPitch, Math::Sin(Pitch), Math::Sin(Yaw) * CosPitch };
        }

        NODISCARD FVector3 Right() const
        {
            return { -Math::Sin(Yaw), 0.0f, Math::Cos(Yaw) };
        }

        NODISCARD FVector3 Up() const
        {
            const FVector3 F = Forward();
            const FVector3 R = Right();
            return { R.y * F.z - R.z * F.y, R.z * F.x - R.x * F.z, R.x * F.y - R.y * F.x };
        }

        void LookAt(const FVector3& Target)
        {
            const FVector3 D = Target - Position;
            const float Flat = Math::Sqrt(D.x * D.x + D.z * D.z);
            Yaw = Math::Atan2(D.z, D.x);
            Pitch = Math::Clamp(Math::Atan2(D.y, Math::Max(Flat, 0.0001f)), -1.53f, 1.53f);
        }

        NODISCARD const FVector3& GetPosition() const { return Position; }

    private:

        FVector3 Position { 40.0f, 46.0f, 40.0f };
        float    Yaw   = 0.9f;
        float    Pitch = -0.22f;
    };
}
