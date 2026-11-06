#include "VoxelModels.h"

namespace Grain
{
    void FVoxelModel::Reset(int32 X, int32 Y, int32 Z)
    {
        Size[0] = X;
        Size[1] = Y;
        Size[2] = Z;
        Cells.Init(uint8(EMaterial::Air), X * Y * Z);
    }

    void FVoxelModel::Set(int32 X, int32 Y, int32 Z, EMaterial Material)
    {
        if (X < 0 || Y < 0 || Z < 0 || X >= Size[0] || Y >= Size[1] || Z >= Size[2])
        {
            return;
        }
        Cells[(Y * Size[2] + Z) * Size[0] + X] = uint8(Material);
    }

    void FVoxelModel::Box(int32 X0, int32 Y0, int32 Z0, int32 X1, int32 Y1, int32 Z1, EMaterial Material)
    {
        for (int32 Y = Y0; Y <= Y1; ++Y)
        {
            for (int32 Z = Z0; Z <= Z1; ++Z)
            {
                for (int32 X = X0; X <= X1; ++X)
                {
                    Set(X, Y, Z, Material);
                }
            }
        }
    }

    void FVoxelModel::Ball(float CX, float CY, float CZ, float Radius, EMaterial Material)
    {
        const int32 X0 = int32(Math::Floor(CX - Radius));
        const int32 X1 = int32(Math::Ceil(CX + Radius));
        const int32 Y0 = int32(Math::Floor(CY - Radius));
        const int32 Y1 = int32(Math::Ceil(CY + Radius));
        const int32 Z0 = int32(Math::Floor(CZ - Radius));
        const int32 Z1 = int32(Math::Ceil(CZ + Radius));

        for (int32 Y = Y0; Y <= Y1; ++Y)
        {
            for (int32 Z = Z0; Z <= Z1; ++Z)
            {
                for (int32 X = X0; X <= X1; ++X)
                {
                    const float DX = float(X) + 0.5f - CX;
                    const float DY = float(Y) + 0.5f - CY;
                    const float DZ = float(Z) + 0.5f - CZ;

                    if (DX * DX + DY * DY + DZ * DZ <= Radius * Radius)
                    {
                        Set(X, Y, Z, Material);
                    }
                }
            }
        }
    }

    void FModelLibrary::Build()
    {
        //~ Local axes are X right, Y up, Z forward, and every figure faces positive Z.

        {
            FVoxelModel& M = Models[uint32(EModel::Delver)];
            M.Reset(10, 28, 8);

            M.Box(2, 0, 3, 3, 12, 5, EMaterial::Cloth);
            M.Box(6, 0, 3, 7, 12, 5, EMaterial::Cloth);
            M.Box(2, 0, 3, 3, 1, 6, EMaterial::Wood);
            M.Box(6, 0, 3, 7, 1, 6, EMaterial::Wood);

            M.Box(2, 12, 2, 7, 21, 6, EMaterial::Cloth);
            M.Box(2, 13, 2, 7, 14, 6, EMaterial::Metal);
            M.Box(3, 17, 6, 6, 20, 6, EMaterial::Rune);

            M.Box(0, 13, 3, 1, 21, 5, EMaterial::Skin);
            M.Box(8, 13, 3, 9, 21, 5, EMaterial::Skin);

            M.Box(3, 21, 2, 6, 25, 6, EMaterial::Skin);
            M.Box(2, 24, 1, 7, 27, 6, EMaterial::Cloth);
            M.Set(3, 23, 6, EMaterial::Eye);
            M.Set(6, 23, 6, EMaterial::Eye);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Husk)];
            M.Reset(10, 26, 7);

            M.Box(2, 0, 2, 3, 11, 4, EMaterial::Husk);
            M.Box(6, 0, 2, 7, 11, 4, EMaterial::Husk);

            M.Box(2, 11, 2, 7, 19, 5, EMaterial::Husk);
            M.Box(3, 13, 1, 6, 13, 5, EMaterial::Bone);
            M.Box(3, 16, 1, 6, 16, 5, EMaterial::Bone);

            M.Box(0, 10, 2, 1, 19, 4, EMaterial::Husk);
            M.Box(8, 10, 2, 9, 19, 4, EMaterial::Husk);
            M.Box(0, 8, 2, 1, 10, 4, EMaterial::Bone);
            M.Box(8, 8, 2, 9, 10, 4, EMaterial::Bone);

            M.Box(3, 19, 2, 6, 23, 5, EMaterial::Bone);
            M.Set(3, 21, 5, EMaterial::Eye);
            M.Set(6, 21, 5, EMaterial::Eye);
            M.Box(4, 24, 2, 5, 25, 4, EMaterial::Husk);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Wisp)];
            M.Reset(11, 11, 11);

            M.Ball(5.5f, 5.5f, 5.5f, 5.2f, EMaterial::Wisp);
            M.Ball(5.5f, 5.5f, 5.5f, 2.6f, EMaterial::Crystal);
            M.Set(4, 6, 9, EMaterial::Eye);
            M.Set(6, 6, 9, EMaterial::Eye);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Golem)];
            M.Reset(16, 24, 12);

            M.Box(3, 0, 3, 6, 9, 8, EMaterial::Rock);
            M.Box(9, 0, 3, 12, 9, 8, EMaterial::Rock);
            M.Box(2, 9, 2, 13, 19, 9, EMaterial::Rock);
            M.Box(0, 10, 3, 2, 18, 8, EMaterial::Rock);
            M.Box(13, 10, 3, 15, 18, 8, EMaterial::Rock);

            M.Box(5, 12, 9, 10, 16, 9, EMaterial::Ore);
            M.Ball(7.5f, 14.0f, 8.0f, 2.4f, EMaterial::Ember);

            M.Box(5, 19, 3, 10, 23, 8, EMaterial::Rock);
            M.Set(6, 21, 8, EMaterial::Eye);
            M.Set(9, 21, 8, EMaterial::Eye);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Shard)];
            M.Reset(6, 10, 6);

            M.Box(2, 0, 2, 3, 2, 3, EMaterial::Crystal);
            M.Box(1, 2, 1, 4, 6, 4, EMaterial::Crystal);
            M.Box(2, 6, 2, 3, 9, 3, EMaterial::Crystal);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Chest)];
            M.Reset(14, 11, 10);

            M.Box(0, 0, 0, 13, 6, 9, EMaterial::Wood);
            M.Box(0, 7, 0, 13, 10, 9, EMaterial::Wood);
            M.Box(0, 6, 0, 13, 7, 9, EMaterial::Metal);
            M.Box(2, 0, 0, 3, 10, 0, EMaterial::Metal);
            M.Box(10, 0, 0, 11, 10, 0, EMaterial::Metal);
            M.Box(6, 5, 0, 7, 8, 0, EMaterial::Gold);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Beacon)];
            M.Reset(10, 44, 10);

            M.Box(0, 0, 0, 9, 3, 9, EMaterial::Slate);
            M.Box(2, 3, 2, 7, 34, 7, EMaterial::Obsidian);

            for (int32 Band = 8; Band < 34; Band += 8)
            {
                M.Box(1, Band, 1, 8, Band + 1, 8, EMaterial::Rune);
            }

            M.Ball(4.5f, 38.0f, 4.5f, 4.0f, EMaterial::Ember);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Bolt)];
            M.Reset(5, 5, 5);
            M.Ball(2.5f, 2.5f, 2.5f, 2.4f, EMaterial::Ember);
        }

        {
            FVoxelModel& M = Models[uint32(EModel::Debris)];
            M.Reset(3, 3, 3);
            M.Box(0, 0, 0, 2, 2, 2, EMaterial::Stone);
        }

        Pack();
    }

    void FModelLibrary::Pack()
    {
        GpuDescs.Init(FModelDescGpu{}, kMaxModels);
        GpuCells.Reset();

        for (uint32 Index = 0; Index < uint32(EModel::Count); ++Index)
        {
            const FVoxelModel& M = Models[Index];

            FModelDescGpu& Desc = GpuDescs[Index];
            Desc.SizeX = uint32(M.Size[0]);
            Desc.SizeY = uint32(M.Size[1]);
            Desc.SizeZ = uint32(M.Size[2]);
            Desc.Base  = uint32(GpuCells.Num());

            // Four cells to a word, so the shader reads one load per lookup.
            const int32 Words = (M.Cells.Num() + 3) / 4;
            const int32 Start = GpuCells.Num();
            GpuCells.SetNum(Start + Words, 0u);

            for (int32 i = 0; i < M.Cells.Num(); ++i)
            {
                GpuCells[Start + (i >> 2)] |= uint32(M.Cells[i]) << ((i & 3u) * 8u);
            }
        }
    }
}
