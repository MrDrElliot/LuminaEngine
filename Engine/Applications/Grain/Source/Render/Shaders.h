#pragma once

// One embedded Slang module with named entry points, so a normal engine boot compiles none of it.
namespace Grain::Shaders
{
    // Shared by both modules, so the traversal cannot drift between the renderer and the compute passes.
    constexpr const char* kVoxelCommon = R"SLANG(
        struct FRHIRoot { uint64_t Args; };
        [[vk::push_constant]] FRHIRoot gRHI;
        Ptr<T> GetArgs<T>() { return (T*)gRHI.Args; }

        //~ World layout, mirrored from VoxelTypes.h.

        static const float kVoxelSize = 1.0 / 16.0;
        static const int   kRootX = 5;
        static const int   kRootY = 4;
        static const int   kRootZ = 5;
        static const float kRootSpan = 512.0;

        static const uint FLAG_SOLID   = 1u;
        static const uint FLAG_UNIFORM = 2u;
        static const uint FLAG_PRESENT = 4u;
        static const uint FLAG_LEAF    = 8u;

        static const uint MAT_AIR = 0u, MAT_GRASS = 1u, MAT_DIRT = 2u, MAT_STONE = 3u;
        static const uint MAT_ROCK = 4u, MAT_SAND = 5u, MAT_SNOW = 6u, MAT_WATER = 7u;
        static const uint MAT_WOOD = 8u, MAT_LEAVES = 9u, MAT_GRAVEL = 10u, MAT_CLAY = 11u;
        static const uint MAT_MOSS = 12u, MAT_ORE = 13u, MAT_CRYSTAL = 14u, MAT_LAVA = 15u;

        static const int kSimSide       = 256;
        static const int kSimCoarseStep = 8;
        static const int kSimCoarseSide = kSimSide / kSimCoarseStep;

        struct FVoxNode
        {
            uint ChildBase;
            uint Palette;
            uint Flags;
            uint PayloadBase;
        };

        //~ A simulated cell carries a solid material and a water volume, so water can be partly full.

        static const uint kMassFull      = 4096u;
        static const uint kMassRender    = 480u;
        static const uint kMassMin       = 48u;
        static const uint kLateralMax    = 1100u;
        static const uint kLateralDead   = 110u;

        uint CellSolid(uint Cell) { return Cell & 0xFFu; }
        uint CellMass(uint Cell)  { return Cell >> 8u; }

        uint PackCell(uint Solid, uint Mass)
        {
            return (Solid & 0xFFu) | (min(Mass, 0xFFFFFFu) << 8u);
        }

        uint LoadMaskWord(uint* Masks, uint NodeIndex, uint Word)
        {
            return Masks[NodeIndex * 16u + Word];
        }

        bool TestSlot(uint* Masks, uint NodeIndex, uint Slot)
        {
            return (LoadMaskWord(Masks, NodeIndex, Slot >> 5u) & (1u << (Slot & 31u))) != 0u;
        }

        // Two loads and one popcount. Summing the words below the slot was costing about eight extra
        // loads per descent level, three levels deep, on every step of every ray.
        uint RankOfSlot(uint* Masks, uint* Prefix, uint NodeIndex, uint Slot)
        {
            const uint Word = Slot >> 5u;
            const uint Packed = Prefix[NodeIndex * 8u + (Word >> 1u)];
            const uint Below = (Word & 1u) != 0u ? (Packed >> 16u) : (Packed & 0xFFFFu);

            return Below + countbits(LoadMaskWord(Masks, NodeIndex, Word) & ((1u << (Slot & 31u)) - 1u));
        }

        uint LeafMaterial(uint* Payload, FVoxNode Node, uint Slot)
        {
            if ((Node.Flags & FLAG_UNIFORM) != 0u)
            {
                return Node.Palette & 0xFFu;
            }

            const uint Word = Payload[Node.PayloadBase + (Slot >> 4u)];
            const uint Index = (Word >> ((Slot & 15u) * 2u)) & 3u;
            return (Node.Palette >> (Index * 8u)) & 0xFFu;
        }

        // Walks to the 0.5 m leaf owning a voxel. A collapsed interior node has no per voxel mask to
        // clear, so digging stops at the depth where the tree stops being subdivided.
        bool FindLeaf(FVoxNode* Nodes, uint* Masks, uint* Prefix, uint* Children, int3 Voxel,
                      out uint OutNode, out int3 OutBase)
        {
            OutNode = 0u;
            OutBase = int3(0);

            const int3 Root = Voxel >> 9;
            if (any(Root < 0) || Root.x >= kRootX || Root.y >= kRootY || Root.z >= kRootZ)
            {
                return false;
            }

            uint NodeIndex = uint((Root.y * kRootZ + Root.z) * kRootX + Root.x);
            FVoxNode Node = Nodes[NodeIndex];

            if ((Node.Flags & FLAG_PRESENT) == 0u)
            {
                return false;
            }

            int3 NodeMin = Root << 9;
            int NodeSize = 512;

            for (int Level = 0; Level < 3; ++Level)
            {
                if ((Node.Flags & FLAG_LEAF) != 0u)
                {
                    OutNode = NodeIndex;
                    OutBase = NodeMin;
                    return true;
                }

                if ((Node.Flags & FLAG_SOLID) != 0u)
                {
                    return false;
                }

                const int ChildSize = NodeSize >> 3;
                const int3 Local = (Voxel - NodeMin) / ChildSize;
                const uint Slot = uint((Local.y * 8 + Local.z) * 8 + Local.x);

                if (!TestSlot(Masks, NodeIndex, Slot))
                {
                    return false;
                }

                NodeIndex = Children[Node.ChildBase + RankOfSlot(Masks, Prefix, NodeIndex, Slot)];
                Node = Nodes[NodeIndex];
                NodeMin += Local * ChildSize;
                NodeSize = ChildSize;
            }

            return false;
        }
    )SLANG";

    constexpr const char* kModule = R"SLANG(
        [[vk::binding(0, 0)]] SamplerState gSamplers[];
        [[vk::binding(1, 0)]] Texture2D    gTextures2D[];

        static const uint SAMPLER_LINEAR_CLAMP = 1;

        float4 SampleLevel0(uint TextureID, float2 UV)
        {
            return gTextures2D[TextureID].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], UV, 0.0);
        }

        struct FViewArgs
        {
            FVoxNode* Nodes;
            uint*     Masks;
            uint*     Prefix;
            uint*     Children;
            uint*     Payload;
            uint*     SimGrid;
            uint*     SimCoarse;

            // Seven pointers leave the next float4 at offset 56, which straddles a 16 byte boundary.
            uint64_t Pad0;

            float4 CameraPos;
            float4 CameraFwd;
            float4 CameraRight;
            float4 CameraUp;
            float4 SunDir;
            float4 PrevPos;
            float4 PrevFwd;
            float4 PrevRight;
            float4 PrevUp;
            float4 Params;
            float4 SimOrigin;

            uint4 IDs;
        };

        FViewArgs View() { return *GetArgs<FViewArgs>(); }

        struct FCell
        {
            bool   bSolid;
            uint   Material;
            float3 Min;
            float  Size;
        };

        bool InsideSim(FViewArgs V, float3 P)
        {
            if (V.IDs.w == 0u)
            {
                return false;
            }
            const float3 Local = P - V.SimOrigin.xyz;
            return all(Local >= 0.0) && all(Local < float(kSimSide));
        }

        // The dense volume owns its box outright, so a cell that moved there never disagrees with the tree.
        FCell ResolveSim(FViewArgs V, float3 P)
        {
            FCell Out;
            Out.bSolid = false;
            Out.Material = MAT_AIR;

            const int3 Local = int3(floor(P - V.SimOrigin.xyz));
            const int3 Block = Local >> 3;

            const uint BlockIndex = uint((Block.y * kSimCoarseSide + Block.z) * kSimCoarseSide + Block.x);
            if (V.SimCoarse[BlockIndex] == 0u)
            {
                Out.Min = V.SimOrigin.xyz + float3(Block << 3);
                Out.Size = float(kSimCoarseStep);
                return Out;
            }

            const uint Index = uint((Local.y * kSimSide + Local.z) * kSimSide + Local.x);
            const uint Cell = V.SimGrid[Index];
            const uint Solid = CellSolid(Cell);
            const uint Mass = CellMass(Cell);

            Out.Min = V.SimOrigin.xyz + float3(Local);
            Out.Size = 1.0;

            if (Solid != 0u)
            {
                Out.bSolid = true;
                Out.Material = Solid;
            }
            else if (Mass >= kMassRender)
            {
                Out.bSolid = true;
                Out.Material = MAT_WATER;
            }

            return Out;
        }

        // A miss reports the empty cell it landed in, which is what lets the ray skip whole subtrees.
        FCell Resolve(FViewArgs V, float3 P, float MinCell)
        {
            if (InsideSim(V, P))
            {
                return ResolveSim(V, P);
            }

            FCell Out;
            Out.bSolid = false;
            Out.Material = MAT_AIR;
            Out.Min = float3(0.0);
            Out.Size = kRootSpan;

            const int3 Root = int3(floor(P / kRootSpan));
            if (Root.x < 0 || Root.y < 0 || Root.z < 0 || Root.x >= kRootX || Root.y >= kRootY || Root.z >= kRootZ)
            {
                Out.Min = float3(Root) * kRootSpan;
                return Out;
            }

            uint NodeIndex = uint((Root.y * kRootZ + Root.z) * kRootX + Root.x);
            FVoxNode Node = V.Nodes[NodeIndex];

            float3 NodeMin = float3(Root) * kRootSpan;
            float NodeSize = kRootSpan;

            if ((Node.Flags & FLAG_PRESENT) == 0u)
            {
                Out.Min = NodeMin;
                Out.Size = NodeSize;
                return Out;
            }

            for (int Level = 0; Level < 3; ++Level)
            {
                if ((Node.Flags & FLAG_SOLID) != 0u)
                {
                    Out.bSolid = true;
                    Out.Material = Node.Palette & 0xFFu;
                    Out.Min = NodeMin;
                    Out.Size = NodeSize;
                    return Out;
                }

                const float ChildSize = NodeSize * 0.125;
                int3 Local = int3(floor((P - NodeMin) / ChildSize));
                Local = clamp(Local, int3(0), int3(7));

                const uint Slot = uint((Local.y * 8 + Local.z) * 8 + Local.x);
                const float3 ChildMin = NodeMin + float3(Local) * ChildSize;

                if (!TestSlot(V.Masks, NodeIndex, Slot))
                {
                    Out.Min = ChildMin;
                    Out.Size = ChildSize;
                    return Out;
                }

                if (V.IDs.w != 0u && ChildSize > float(kSimCoarseStep))
                {
                    const float3 SimMax = V.SimOrigin.xyz + float(kSimSide);
                    if (all(ChildMin < SimMax) && all(ChildMin + ChildSize > V.SimOrigin.xyz))
                    {
                        // Descend rather than trust a coarse cell that straddles the simulated box.
                        MinCell = 0.0;
                    }
                }

                // Below a pixel's footprint the descent stops and shades with the node's dominant
                // material, which removes the aliasing that one ray per 6 cm voxel would otherwise give.
                const bool bStop = ChildSize <= MinCell;

                if ((Node.Flags & FLAG_LEAF) != 0u)
                {
                    Out.bSolid = true;
                    Out.Material = bStop ? (Node.Palette & 0xFFu) : LeafMaterial(V.Payload, Node, Slot);
                    Out.Min = ChildMin;
                    Out.Size = ChildSize;
                    return Out;
                }

                const uint ChildIndex = V.Children[Node.ChildBase + RankOfSlot(V.Masks, V.Prefix, NodeIndex, Slot)];

                if (bStop)
                {
                    Out.bSolid = true;
                    Out.Material = V.Nodes[ChildIndex].Palette & 0xFFu;
                    Out.Min = ChildMin;
                    Out.Size = ChildSize;
                    return Out;
                }

                NodeIndex = ChildIndex;
                Node = V.Nodes[NodeIndex];
                NodeMin = ChildMin;
                NodeSize = ChildSize;
            }

            Out.Min = NodeMin;
            Out.Size = NodeSize;
            return Out;
        }

        float3 SafeInverse(float3 D)
        {
            float3 R;
            R.x = abs(D.x) < 1e-7 ? 1e30 : 1.0 / D.x;
            R.y = abs(D.y) < 1e-7 ? 1e30 : 1.0 / D.y;
            R.z = abs(D.z) < 1e-7 ? 1e30 : 1.0 / D.z;
            return R;
        }

        float ExitDistance(float3 Origin, float3 Dir, float3 InvDir, float3 CellMin, float CellSize)
        {
            const float3 Planes = CellMin + step(float3(0.0), Dir) * CellSize;
            const float3 T = (Planes - Origin) * InvDir;
            return min(T.x, min(T.y, T.z));
        }

        float3 EntryNormal(float3 Origin, float3 Dir, float3 InvDir, float3 CellMin, float CellSize)
        {
            const float3 Planes = CellMin + step(Dir, float3(0.0)) * CellSize;
            const float3 T = (Planes - Origin) * InvDir;

            if (T.x > T.y && T.x > T.z)
            {
                return float3(Dir.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
            }
            if (T.y > T.z)
            {
                return float3(0.0, Dir.y > 0.0 ? -1.0 : 1.0, 0.0);
            }
            return float3(0.0, 0.0, Dir.z > 0.0 ? -1.0 : 1.0);
        }

        bool ClipToWorld(float3 Origin, float3 InvDir, out float TMin, out float TMax)
        {
            const float3 BoxMax = float3(kRootX, kRootY, kRootZ) * kRootSpan;

            const float3 A = (float3(0.0) - Origin) * InvDir;
            const float3 B = (BoxMax - Origin) * InvDir;
            const float3 Near = min(A, B);
            const float3 Far  = max(A, B);

            TMin = max(max(Near.x, Near.y), max(Near.z, 0.0));
            TMax = min(Far.x, min(Far.y, Far.z));
            return TMax > TMin;
        }

        struct FHit
        {
            bool   bHit;
            float  T;
            uint   Material;
            float3 Normal;
            float3 Position;
        };

        FHit March(FViewArgs V, float3 Origin, float3 Dir, float MaxDistance, int MaxSteps, float PixelScale)
        {
            FHit Out;
            Out.bHit = false;
            Out.T = MaxDistance;
            Out.Material = MAT_AIR;
            Out.Normal = float3(0.0, 1.0, 0.0);
            Out.Position = Origin;

            const float3 InvDir = SafeInverse(Dir);

            float TMin;
            float TMax;
            if (!ClipToWorld(Origin, InvDir, TMin, TMax))
            {
                return Out;
            }

            float T = max(TMin, 0.0) + 1e-4;
            TMax = min(TMax, MaxDistance);

            for (int Step = 0; Step < MaxSteps; ++Step)
            {
                if (T >= TMax)
                {
                    break;
                }

                const float3 P = Origin + Dir * T;
                const FCell Cell = Resolve(V, P, T * PixelScale);

                if (Cell.bSolid)
                {
                    Out.bHit = true;
                    Out.T = T;
                    Out.Material = Cell.Material;
                    Out.Normal = EntryNormal(Origin, Dir, InvDir, Cell.Min, Cell.Size);
                    Out.Position = P;
                    return Out;
                }

                const float Next = ExitDistance(Origin, Dir, InvDir, Cell.Min, Cell.Size);
                T = max(Next, T + 1e-3) + 1e-4;
            }

            return Out;
        }

        bool MarchOccluded(FViewArgs V, float3 Origin, float3 Dir, float MaxDistance, int MaxSteps, float PixelScale)
        {
            const float3 InvDir = SafeInverse(Dir);

            float TMin;
            float TMax;
            if (!ClipToWorld(Origin, InvDir, TMin, TMax))
            {
                return false;
            }

            float T = max(TMin, 0.0) + 1e-4;
            TMax = min(TMax, MaxDistance);

            for (int Step = 0; Step < MaxSteps; ++Step)
            {
                if (T >= TMax)
                {
                    return false;
                }

                const FCell Cell = Resolve(V, Origin + Dir * T, T * PixelScale);
                if (Cell.bSolid && Cell.Material != MAT_WATER)
                {
                    return true;
                }

                const float Next = ExitDistance(Origin, Dir, InvDir, Cell.Min, Cell.Size);
                T = max(Next, T + 1e-3) + 1e-4;
            }

            return false;
        }

        float HashVoxel(float3 P)
        {
            const float3 I = floor(P);
            return frac(sin(dot(I, float3(12.9898, 78.233, 37.719))) * 43758.5453);
        }

        struct FSurfaceMaterial
        {
            float3 Albedo;
            float3 Emissive;
            float  Gloss;
        };

        FSurfaceMaterial MaterialOf(uint Id, float3 VoxelPos)
        {
            FSurfaceMaterial M;
            M.Emissive = float3(0.0);
            M.Gloss = 0.0;
            M.Albedo = float3(0.5);

            switch (Id)
            {
            case MAT_GRASS:  M.Albedo = float3(0.13, 0.30, 0.09); break;
            case MAT_DIRT:   M.Albedo = float3(0.25, 0.17, 0.11); break;
            case MAT_STONE:  M.Albedo = float3(0.31, 0.30, 0.29); break;
            case MAT_ROCK:   M.Albedo = float3(0.25, 0.23, 0.21); break;
            case MAT_SAND:   M.Albedo = float3(0.48, 0.41, 0.27); break;
            case MAT_SNOW:   M.Albedo = float3(0.55, 0.60, 0.70); M.Gloss = 0.2; break;
            case MAT_WATER:  M.Albedo = float3(0.03, 0.10, 0.16); M.Gloss = 1.0; break;
            case MAT_WOOD:   M.Albedo = float3(0.20, 0.13, 0.08); break;
            case MAT_LEAVES: M.Albedo = float3(0.09, 0.25, 0.08); break;
            case MAT_GRAVEL: M.Albedo = float3(0.29, 0.28, 0.26); break;
            case MAT_CLAY:   M.Albedo = float3(0.42, 0.24, 0.16); break;
            case MAT_MOSS:   M.Albedo = float3(0.11, 0.24, 0.09); break;
            case MAT_ORE:    M.Albedo = float3(0.40, 0.36, 0.28); M.Gloss = 0.4; break;
            case MAT_CRYSTAL:
                M.Albedo = float3(0.22, 0.48, 0.66);
                M.Emissive = float3(0.55, 2.60, 4.10);
                M.Gloss = 0.7;
                break;
            case MAT_LAVA:
                M.Albedo = float3(0.30, 0.10, 0.03);
                M.Emissive = float3(4.20, 1.10, 0.18);
                break;
            default: break;
            }

            // Per voxel jitter, which keeps a wide field of one material from reading as plastic.
            const float Jitter = HashVoxel(VoxelPos) * 0.10 - 0.05;
            M.Albedo *= saturate(1.0 + Jitter);
            return M;
        }

        float WaterMassAt(FViewArgs V, float3 PosVoxels)
        {
            if (V.IDs.w == 0u)
            {
                return 0.0;
            }

            const int3 Local = int3(floor(PosVoxels - V.SimOrigin.xyz));
            if (any(Local < 0) || any(Local >= kSimSide))
            {
                return 0.0;
            }

            const uint Cell = V.SimGrid[uint((Local.y * kSimSide + Local.z) * kSimSide + Local.x)];
            if (CellSolid(Cell) != 0u)
            {
                return -1.0;
            }
            return float(CellMass(Cell)) / float(kMassFull);
        }

        // A column sum, so a deep pool reads darker than a sheet running over rock.
        float WaterColumn(FViewArgs V, float3 PosVoxels)
        {
            float Depth = 0.0;
            for (int i = 0; i < 8; ++i)
            {
                const float Mass = WaterMassAt(V, PosVoxels - float3(0.0, float(i), 0.0));
                if (Mass < 0.0)
                {
                    break;
                }
                Depth += max(Mass, 0.0);
            }
            return Depth;
        }

        // The surface follows the volume gradient, which is what makes a blocky grid read as flowing.
        float3 WaterSurfaceNormal(FViewArgs V, float3 PosVoxels, out float Slope)
        {
            Slope = 0.0;

            if (V.IDs.w == 0u)
            {
                return float3(0.0, 1.0, 0.0);
            }

            const float Here = max(WaterMassAt(V, PosVoxels), 0.0);
            const float Right = max(WaterMassAt(V, PosVoxels + float3(1.0, 0.0, 0.0)), 0.0);
            const float Left  = max(WaterMassAt(V, PosVoxels - float3(1.0, 0.0, 0.0)), 0.0);
            const float Front = max(WaterMassAt(V, PosVoxels + float3(0.0, 0.0, 1.0)), 0.0);
            const float Back  = max(WaterMassAt(V, PosVoxels - float3(0.0, 0.0, 1.0)), 0.0);

            if (Here <= 0.0)
            {
                return float3(0.0, 1.0, 0.0);
            }

            const float2 Gradient = float2(Right - Left, Front - Back) * 0.5;
            Slope = length(Gradient);

            return normalize(float3(-Gradient.x, 0.62, -Gradient.y));
        }

        float3 SunRadiance(FViewArgs V)
        {
            return float3(1.00, 0.90, 0.74) * V.CameraUp.w;
        }

        float3 SkyRadiance(FViewArgs V, float3 Dir)
        {
            const float3 SunDir = V.SunDir.xyz;
            const float Up = saturate(Dir.y * 0.5 + 0.5);

            const float3 Zenith  = float3(0.13, 0.26, 0.60);
            const float3 Horizon = float3(0.52, 0.62, 0.78);
            const float3 Ground  = float3(0.10, 0.09, 0.08);

            float3 Sky = lerp(Horizon, Zenith, pow(saturate(Dir.y), 0.55));
            Sky = lerp(Sky, Ground, smoothstep(0.0, -0.35, Dir.y));

            const float Cosine = saturate(dot(Dir, SunDir));

            Sky += float3(0.55, 0.42, 0.26) * pow(Cosine, 8.0) * 0.55;
            Sky += SunRadiance(V) * smoothstep(0.9992, 0.9997, Cosine) * 4.0;

            return Sky;
        }

        float3 CosineDirection(float3 Normal, float2 Random)
        {
            const float Angle = Random.x * 6.2831853;
            const float Radius = sqrt(Random.y);
            const float Height = sqrt(max(0.0, 1.0 - Random.y));

            float3 Tangent = abs(Normal.y) < 0.95 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
            Tangent = normalize(cross(Tangent, Normal));
            const float3 Bitangent = cross(Normal, Tangent);

            return normalize(Tangent * (cos(Angle) * Radius) + Bitangent * (sin(Angle) * Radius) + Normal * Height);
        }

        float2 PixelRotation(uint2 Pixel)
        {
            uint Seed = Pixel.x * 1973u + Pixel.y * 9277u + 26699u;
            Seed = (Seed ^ 61u) ^ (Seed >> 16u);
            Seed *= 9u;
            Seed = Seed ^ (Seed >> 4u);
            Seed *= 0x27d4eb2du;
            Seed = Seed ^ (Seed >> 15u);

            const uint A = Seed;
            const uint B = Seed * 1664525u + 1013904223u;
            return float2(float(A & 0xFFFFFFu) / 16777216.0, float(B & 0xFFFFFFu) / 16777216.0);
        }

        // R2 over the frame index with a per pixel rotation. White noise per frame never stratifies,
        // so consecutive frames kept resampling the same part of the hemisphere.
        float2 Random2(uint2 Pixel, uint Frame)
        {
            const float2 R2 = float2(0.7548776662, 0.5698402909) * float(Frame + 1u);
            return frac(R2 + PixelRotation(Pixel));
        }

        // A secondary hit never traces its own bounce, so reflections and the light cache stay noise free.
        float3 ShadeSecondary(FViewArgs V, FHit Hit)
        {
            const FSurfaceMaterial M = MaterialOf(Hit.Material, Hit.Position);
            const float NdotL = saturate(dot(Hit.Normal, V.SunDir.xyz));

            float3 Lit = M.Emissive;
            if (NdotL > 0.0)
            {
                const float3 Point = Hit.Position + Hit.Normal * 0.12;
                if (!MarchOccluded(V, Point, V.SunDir.xyz, 220.0, 48, 0.030))
                {
                    Lit += M.Albedo * SunRadiance(V) * NdotL;
                }
            }

            return Lit + M.Albedo * SkyRadiance(V, Hit.Normal) * 0.12;
        }

        // Returns irradiance rather than radiance, so the albedo can be divided out before denoising.
        float3 GatherBounce(FViewArgs V, float3 Origin, float3 Normal, uint2 Pixel, uint Frame)
        {
            const float3 Dir = CosineDirection(Normal, Random2(Pixel, Frame));
            const FHit Bounce = March(V, Origin + Normal * 0.12, Dir, 260.0, 72, 0.030);

            const float3 Radiance = Bounce.bHit ? ShadeSecondary(V, Bounce) : SkyRadiance(V, Dir);

            // One bounce onto a crystal returns far above the mean, and the accumulator smears it
            // into a blob that survives for dozens of frames. Clamping is biased and worth it.
            const float Luma = dot(Radiance, float3(0.2126, 0.7152, 0.0722));
            const float Ceiling = 6.0;
            return Luma > Ceiling ? Radiance * (Ceiling / Luma) : Radiance;
        }

        // The direct term carries no Monte Carlo noise, so it bypasses the denoiser entirely.
        float3 ShadeDirect(FViewArgs V, FHit Hit, float3 RayDir, FSurfaceMaterial M)
        {
            const float3 Point = Hit.Position + Hit.Normal * 0.12;

            float3 Color = M.Emissive;

            const float NdotL = saturate(dot(Hit.Normal, V.SunDir.xyz));
            if (NdotL > 0.0 && !MarchOccluded(V, Point, V.SunDir.xyz, 1200.0, 192, 0.006))
            {
                Color += M.Albedo * SunRadiance(V) * NdotL;

                if (M.Gloss > 0.0)
                {
                    const float3 Halfway = normalize(V.SunDir.xyz - RayDir);
                    const float Spec = pow(saturate(dot(Hit.Normal, Halfway)), 48.0);
                    Color += SunRadiance(V) * Spec * M.Gloss * 0.7;
                }
            }

            return Color;
        }

        float3 ShadeHit(FViewArgs V, FHit Hit, float3 RayDir, uint2 Pixel, uint Frame)
        {
            const FSurfaceMaterial M = MaterialOf(Hit.Material, Hit.Position);
            return ShadeDirect(V, Hit, RayDir, M)
                 + M.Albedo * GatherBounce(V, Hit.Position, Hit.Normal, Pixel, Frame) * V.SunDir.w;
        }

        //~ Axis aligned faces pack into three bits, which is all the a trous edge stop needs.

        float EncodeNormal(float3 N)
        {
            if (N.x > 0.5) { return 0.0; }
            if (N.x < -0.5) { return 1.0; }
            if (N.y > 0.5) { return 2.0; }
            if (N.y < -0.5) { return 3.0; }
            if (N.z > 0.5) { return 4.0; }
            return 5.0;
        }

        float3 ApplyFog(FViewArgs V, float3 Color, float3 Dir, float DistanceVoxels)
        {
            const float Meters = DistanceVoxels * kVoxelSize;
            const float Fog = 1.0 - exp(-Meters * 0.0016);

            const float3 FogColor = lerp(float3(0.52, 0.60, 0.72), float3(0.80, 0.75, 0.64),
                                         pow(saturate(dot(Dir, V.SunDir.xyz)), 5.0));
            return lerp(Color, FogColor, Fog * 0.80);
        }

        struct FFullscreenOut
        {
            float4 Position : SV_Position;
            float2 UV       : TEXCOORD0;
        };

        [shader("vertex")]
        FFullscreenOut FullscreenVS(uint VertexID : SV_VertexID)
        {
            FFullscreenOut Output;
            Output.UV = float2((VertexID << 1) & 2, VertexID & 2);
            Output.Position = float4(Output.UV * 2.0 - 1.0, 0.5, 1.0);
            return Output;
        }

        struct FRaymarchOut
        {
            float4 Indirect : SV_Target0;
            float4 Direct   : SV_Target1;
            float4 Albedo   : SV_Target2;
        };

        [shader("fragment")]
        FRaymarchOut RaymarchPS(FFullscreenOut Input)
        {
            const FViewArgs V = View();

            const float2 Resolution = V.Params.xy;
            const uint Frame = uint(V.Params.z);
            const uint2 Pixel = uint2(Input.UV * Resolution);

            // A per frame sub pixel offset, resolved by the history blend, is what antialiases the voxels.
            const float2 Jitter = (frac(float2(0.7548776662, 0.5698402909) * float(Frame + 1u)) - 0.5) / Resolution;
            const float PixelScale = V.Params.w;

            const float2 Ndc = (Input.UV + Jitter) * 2.0 - 1.0;
            const float3 Dir = normalize(V.CameraFwd.xyz
                + V.CameraRight.xyz * (Ndc.x * V.CameraPos.w * V.CameraFwd.w)
                - V.CameraUp.xyz * (Ndc.y * V.CameraPos.w));

            const float3 Origin = V.CameraPos.xyz / kVoxelSize;

            const FHit Hit = March(V, Origin, Dir, 4000.0, 256, PixelScale);

            FRaymarchOut Out;
            Out.Indirect = float4(0.0, 0.0, 0.0, 1e5);
            Out.Direct = float4(0.0, 0.0, 0.0, 7.0);
            Out.Albedo = float4(0.0, 0.0, 0.0, 1.0);

            if (!Hit.bHit)
            {
                Out.Direct.rgb = SkyRadiance(V, Dir);
                return Out;
            }

            Out.Indirect.w = Hit.T;
            Out.Direct.w = EncodeNormal(Hit.Normal);

            if (V.IDs.z != 0u)
            {
                if (V.IDs.z == 1u)
                {
                    const float H = float(Hit.Material) * 0.0625;
                    Out.Direct.rgb = saturate(abs(frac(H + float3(0.0, 0.667, 0.333)) * 6.0 - 3.0) - 1.0);
                }
                else
                {
                    Out.Direct.rgb = abs(Hit.Normal);
                }
                return Out;
            }

            if (Hit.Material == MAT_WATER)
            {
                // Water resolves to one radiance with no separable albedo, so it skips the denoiser.
                float3 Color = float3(0.0);
            const float Time = V.CameraRight.w;
            const float2 W = Hit.Position.xz;

            // Crossed waves at three scales, or a single sine reads as flat horizontal banding.
            // Ripple detail fades with distance, or a flat plane aliases into a moire grid.
            const float Detail = saturate(1.0 - Hit.T / 900.0);

            float2 Ripple = float2(0.0);
            Ripple += float2(sin(W.x * 0.21 + Time * 1.9), cos(W.y * 0.19 - Time * 1.7)) * 0.055;
            Ripple += float2(sin(W.y * 0.47 - Time * 2.6), cos(W.x * 0.51 + Time * 2.3)) * 0.030 * Detail;
            Ripple += float2(sin(dot(W, float2(0.77, 0.63)) + Time * 3.4)) * 0.018 * Detail * Detail;

            float Slope = 0.0;
            const float3 Surface = WaterSurfaceNormal(V, Hit.Position, Slope);

            const float3 Normal = normalize(Surface + float3(Ripple.x, 0.0, Ripple.y));
            const float3 Reflected = reflect(Dir, Normal);

            const FHit Mirror = March(V, Hit.Position + Normal * 0.05, Reflected, 900.0, 112, 0.012);
            const float3 Reflection = Mirror.bHit
                ? ShadeSecondary(V, Mirror)
                : SkyRadiance(V, Reflected);

            const FHit Floor = March(V, Hit.Position + Dir * 0.05, Dir, 220.0, 96, 0.012);

            // Beer absorption over the path through the body, so shallows stay clear and pools go deep.
            const float Through = Floor.bHit ? Floor.T * kVoxelSize : 6.0;
            const float3 Absorb = exp(-float3(0.55, 0.16, 0.10) * Through * 3.0);

            const float3 Below = Floor.bHit
                ? ShadeSecondary(V, Floor) * Absorb
                : float3(0.010, 0.035, 0.045);

            const float Fresnel = 0.03 + 0.97 * pow(saturate(1.0 + dot(Dir, Normal)), 5.0);
            Color = lerp(Below, Reflection, Fresnel);

            // In scattering, which is what keeps a deep pool from reading as black glass.
            const float Column = WaterColumn(V, Hit.Position);
            Color += float3(0.03, 0.20, 0.24) * saturate(0.25 + Column * 0.30) * (1.0 - Fresnel);

            // Sun glint, which is most of what makes a water plane read as a surface at all.
            const float3 Halfway = normalize(V.SunDir.xyz - Dir);
            Color += SunRadiance(V) * pow(saturate(dot(Normal, Halfway)), 220.0) * 2.6;

            // Whitewater only where the surface actually breaks, or the whole run reads as snow.
            const float Thin = 1.0 - saturate(Column * 1.4);
            const float Break = saturate((Slope - 0.22) * 2.4);
            const float Foam = Break * 0.30 + Break * Thin * 0.34;
            Color = lerp(Color, float3(0.88, 0.94, 1.00) * 1.25, saturate(Foam));


                Out.Direct.rgb = Color;
                return Out;
            }

            const FSurfaceMaterial M = MaterialOf(Hit.Material, Hit.Position);

            Out.Direct.rgb = ShadeDirect(V, Hit, Dir, M);
            Out.Albedo.rgb = M.Albedo;
            Out.Indirect.rgb = GatherBounce(V, Hit.Position, Hit.Normal, Pixel, Frame) * V.SunDir.w;

            return Out;
        }

        struct FDenoiseArgs
        {
            uint4  IDs;
            uint4  Extra;
            float4 Params;
            float4 CameraPos;
            float4 CameraFwd;
            float4 CameraRight;
            float4 CameraUp;
            float4 PrevPos;
            float4 PrevFwd;
            float4 PrevRight;
            float4 PrevUp;
            float4 SunDir;
        };

        float Luminance(float3 C) { return dot(C, float3(0.2126, 0.7152, 0.0722)); }

        struct FTemporalOut
        {
            float4 Color  : SV_Target0;
            float4 Moment : SV_Target1;
        };

        // An exponential blend with a fixed rate caps the sample count no matter how long a pixel has
        // been stable, so the count itself drives the rate until it reaches the floor.
        [shader("fragment")]
        FTemporalOut TemporalPS(FFullscreenOut Input)
        {
            const FDenoiseArgs Args = *GetArgs<FDenoiseArgs>();

            const float4 Raw = SampleLevel0(Args.IDs.x, Input.UV);
            const float3 Current = Raw.rgb;
            const float Distance = Raw.w;

            FTemporalOut Out;

            float History = 0.0;
            float3 Blended = Current;
            float2 Moments = float2(Luminance(Current), Luminance(Current) * Luminance(Current));

            const float2 Ndc = Input.UV * 2.0 - 1.0;
            const float3 Dir = normalize(Args.CameraFwd.xyz
                + Args.CameraRight.xyz * (Ndc.x * Args.CameraPos.w * Args.CameraFwd.w)
                - Args.CameraUp.xyz * (Ndc.y * Args.CameraPos.w));

            if (Args.Extra.x != 0u && Distance < 9e4)
            {
                const float3 World = Args.CameraPos.xyz + Dir * Distance;
                const float3 Rel = World - Args.PrevPos.xyz;
                const float Z = dot(Rel, Args.PrevFwd.xyz);

                if (Z > 0.05)
                {
                    const float2 PrevUV = float2(
                        dot(Rel, Args.PrevRight.xyz) / (Z * Args.CameraPos.w * Args.CameraFwd.w),
                        -dot(Rel, Args.PrevUp.xyz) / (Z * Args.CameraPos.w)) * 0.5 + 0.5;

                    if (all(PrevUV > 0.001) && all(PrevUV < 0.999))
                    {
                        const float4 PrevMoment = SampleLevel0(Args.IDs.z, PrevUV);
                        const float Expected = length(Rel);

                        if (abs(PrevMoment.b - Expected) < max(Expected * 0.035, 1.2))
                        {
                            const float4 PrevColor = SampleLevel0(Args.IDs.y, PrevUV);

                            History = min(PrevMoment.a + 1.0, Args.Params.z);
                            const float Alpha = max(1.0 / (History + 1.0), Args.Params.w);

                            Blended = lerp(PrevColor.rgb, Current, Alpha);
                            Moments = lerp(PrevMoment.rg, Moments, Alpha);
                        }
                    }
                }
            }

            const float Variance = max(Moments.y - Moments.x * Moments.x, 0.0);

            Out.Color = float4(Blended, Variance);
            Out.Moment = float4(Moments, Distance, History);
            return Out;
        }

        // Edge stopping on distance, face and luminance, which is what lets the blur cross noise but
        // not geometry. Variance rides in alpha so later passes widen only where the signal is unsure.
        [shader("fragment")]
        float4 AtrousPS(FFullscreenOut Input) : SV_Target
        {
            const FDenoiseArgs Args = *GetArgs<FDenoiseArgs>();

            const float2 Texel = 1.0 / Args.Params.xy;
            const float Step = Args.Params.z;

            const float4 Center = SampleLevel0(Args.IDs.x, Input.UV);
            const float4 CenterGeo = SampleLevel0(Args.IDs.y, Input.UV);
            const float CenterNormal = SampleLevel0(Args.IDs.z, Input.UV).w;

            if (CenterGeo.w > 9e4)
            {
                return Center;
            }

            const float CenterLuma = Luminance(Center.rgb);
            const float Sigma = sqrt(max(Center.w, 0.0)) + 1e-3;

            const float Kernel[3] = { 0.375, 0.25, 0.0625 };

            float3 Sum = Center.rgb * Kernel[0] * Kernel[0];
            float SumVariance = Center.w * Kernel[0] * Kernel[0] * Kernel[0] * Kernel[0];
            float Weight = Kernel[0] * Kernel[0];

            for (int Y = -2; Y <= 2; ++Y)
            {
                for (int X = -2; X <= 2; ++X)
                {
                    if (X == 0 && Y == 0)
                    {
                        continue;
                    }

                    const float2 UV = Input.UV + float2(X, Y) * Texel * Step;
                    if (any(UV < 0.0) || any(UV > 1.0))
                    {
                        continue;
                    }

                    const float4 Tap = SampleLevel0(Args.IDs.x, UV);
                    const float4 TapGeo = SampleLevel0(Args.IDs.y, UV);
                    const float TapNormal = SampleLevel0(Args.IDs.z, UV).w;

                    if (TapGeo.w > 9e4)
                    {
                        continue;
                    }

                    const float DepthWeight = exp(-abs(TapGeo.w - CenterGeo.w)
                        / (Args.Params.w * max(CenterGeo.w, 1.0) + 1e-3));
                    const float NormalWeight = TapNormal == CenterNormal ? 1.0 : 0.08;
                    const float LumaWeight = exp(-abs(Luminance(Tap.rgb) - CenterLuma) / (4.0 * Sigma));

                    const float Spatial = Kernel[abs(X)] * Kernel[abs(Y)];
                    const float W = Spatial * DepthWeight * NormalWeight * LumaWeight;

                    Sum += Tap.rgb * W;
                    SumVariance += Tap.w * W * W;
                    Weight += W;
                }
            }

            const float Inverse = 1.0 / max(Weight, 1e-4);
            return float4(Sum * Inverse, SumVariance * Inverse * Inverse);
        }

        [shader("fragment")]
        float4 ComposePS(FFullscreenOut Input) : SV_Target
        {
            const FDenoiseArgs Args = *GetArgs<FDenoiseArgs>();

            const float4 Direct = SampleLevel0(Args.IDs.x, Input.UV);
            const float3 Albedo = SampleLevel0(Args.IDs.y, Input.UV).rgb;
            const float4 Indirect = SampleLevel0(Args.IDs.z, Input.UV);
            const float Distance = SampleLevel0(Args.IDs.w, Input.UV).w;

            // Albedo goes back on after filtering, which is the whole point of taking it off first.
            float3 Color = Direct.rgb + Albedo * Indirect.rgb;

            const float2 Ndc = Input.UV * 2.0 - 1.0;
            const float3 Dir = normalize(Args.CameraFwd.xyz
                + Args.CameraRight.xyz * (Ndc.x * Args.CameraPos.w * Args.CameraFwd.w)
                - Args.CameraUp.xyz * (Ndc.y * Args.CameraPos.w));

            const float Meters = min(Distance, 4000.0) * kVoxelSize;
            const float Fog = 1.0 - exp(-Meters * 0.0016);

            const float3 FogColor = lerp(float3(0.52, 0.60, 0.72), float3(0.80, 0.75, 0.64),
                                         pow(saturate(dot(Dir, Args.SunDir.xyz)), 5.0));

            return float4(lerp(Color, FogColor, Fog * 0.80), Distance);
        }

        struct FBloomArgs
        {
            uint   SourceID;
            uint   bFirstPass;
            float2 SourceTexelSize;
            float  Threshold;
            float  Radius;
            float  Intensity;
            float  Pad0;
        };

        [shader("fragment")]
        float4 DownsamplePS(FFullscreenOut Input) : SV_Target
        {
            const FBloomArgs Args = *GetArgs<FBloomArgs>();
            const float2 Texel = Args.SourceTexelSize;

            float3 Sum = float3(0.0);
            Sum += SampleLevel0(Args.SourceID, Input.UV).rgb * 4.0;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2(-Texel.x, -Texel.y)).rgb;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2( Texel.x, -Texel.y)).rgb;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2(-Texel.x,  Texel.y)).rgb;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2( Texel.x,  Texel.y)).rgb;
            Sum /= 8.0;

            if (Args.bFirstPass != 0u)
            {
                const float Luma = dot(Sum, float3(0.2126, 0.7152, 0.0722));
                Sum *= max(Luma - Args.Threshold, 0.0) / max(Luma, 0.0001);
            }

            return float4(Sum, 1.0);
        }

        [shader("fragment")]
        float4 UpsamplePS(FFullscreenOut Input) : SV_Target
        {
            const FBloomArgs Args = *GetArgs<FBloomArgs>();
            const float2 Texel = Args.SourceTexelSize * Args.Radius;

            float3 Sum = float3(0.0);
            Sum += SampleLevel0(Args.SourceID, Input.UV).rgb * 4.0;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2(-Texel.x, 0.0)).rgb * 2.0;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2( Texel.x, 0.0)).rgb * 2.0;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2(0.0, -Texel.y)).rgb * 2.0;
            Sum += SampleLevel0(Args.SourceID, Input.UV + float2(0.0,  Texel.y)).rgb * 2.0;
            Sum /= 12.0;

            return float4(Sum * Args.Intensity, 1.0);
        }

        struct FCompositeArgs
        {
            uint   SceneID;
            uint   BloomID;
            float2 Resolution;
            float  BloomIntensity;
            float  Exposure;
            float  Vignette;
            float  Pad0;
        };

        float3 Tonemap(float3 Color)
        {
            // Narkowicz ACES, which holds saturation far better than Reinhard against a bright sky.
            const float3 X = Color * 0.6;
            return saturate((X * (2.51 * X + 0.03)) / (X * (2.43 * X + 0.59) + 0.14));
        }

        [shader("fragment")]
        float4 CompositePS(FFullscreenOut Input) : SV_Target
        {
            const FCompositeArgs Args = *GetArgs<FCompositeArgs>();

            float3 Color = SampleLevel0(Args.SceneID, Input.UV).rgb;
            Color += SampleLevel0(Args.BloomID, Input.UV).rgb * Args.BloomIntensity;

            Color *= Args.Exposure;
            Color = Tonemap(Color);

            const float2 Centered = Input.UV * 2.0 - 1.0;
            Color *= saturate(1.0 - dot(Centered, Centered) * 0.18 * Args.Vignette);

            // The swapchain is not sRGB, so the transfer curve is applied here.
            Color = pow(max(Color, 0.0), float3(1.0 / 2.2));

            return float4(Color, 1.0);
        }
    )SLANG";

    // The compute passes take their own module, since a bindless texture array in the interface makes
    // vkCreateComputePipelines reject the layout.
    constexpr const char* kSimModule = R"SLANG(
        struct FSimArgs
        {
            uint*  Grid;
            uint*  Coarse;
            uint4  Source;
            uint4  Control;
        };

        uint SimIndex(int3 P)
        {
            return uint((P.y * kSimSide + P.z) * kSimSide + P.x);
        }

        // A pair exchange along one axis with a parity, so each thread owns both cells outright and
        // mass is conserved exactly without atomics or a second buffer.
        [shader("compute")]
        [numthreads(4, 4, 4)]
        void SimFlowCS(uint3 Thread : SV_DispatchThreadID)
        {
            const FSimArgs Args = *GetArgs<FSimArgs>();

            const uint Axis = Args.Control.z;
            const uint Parity = Args.Control.w;

            int3 A = int3(Thread);
            int3 Step = int3(0, 0, 0);

            if (Axis == 0u)      { A.y = A.y * 2 + int(Parity); Step = int3(0, 1, 0); }
            else if (Axis == 1u) { A.x = A.x * 2 + int(Parity); Step = int3(1, 0, 0); }
            else                 { A.z = A.z * 2 + int(Parity); Step = int3(0, 0, 1); }

            const int3 B = A + Step;
            if (any(A < 0) || any(B >= kSimSide))
            {
                return;
            }

            const uint IndexA = SimIndex(A);
            const uint IndexB = SimIndex(B);

            uint CellA = Args.Grid[IndexA];
            uint CellB = Args.Grid[IndexB];

            const uint SolidA = CellSolid(CellA);
            const uint SolidB = CellSolid(CellB);

            uint MassA = CellMass(CellA);
            uint MassB = CellMass(CellB);

            if (Axis == 0u)
            {
                // B sits above A, so everything B holds falls into whatever room A has left.
                if (SolidA == 0u && SolidB == 0u && MassB > 0u && MassA < kMassFull)
                {
                    const uint Flow = min(MassB, kMassFull - MassA);
                    MassA += Flow;
                    MassB -= Flow;
                }
            }
            else if (SolidA == 0u && SolidB == 0u)
            {
                const int Difference = int(MassA) - int(MassB);

                // A deadband, or a pool spreads into an infinitely thin film across the whole floor.
                if (abs(Difference) > int(kLateralDead))
                {
                    int Flow = Difference / 2;
                    Flow = clamp(Flow, -int(kLateralMax), int(kLateralMax));

                    MassA = uint(int(MassA) - Flow);
                    MassB = uint(int(MassB) + Flow);
                }
            }

            if (Axis == 0u && Args.Control.y != 0u)
            {
                // The spring and the drain both ride the vertical pass, which runs once per tick.
                const float3 Spring = float3(Args.Source.xyz);
                const float Radius = float(Args.Control.y);

                if (SolidA == 0u && distance(float3(A), Spring) < Radius)
                {
                    MassA = kMassFull;
                }
                if (SolidB == 0u && distance(float3(B), Spring) < Radius)
                {
                    MassB = kMassFull;
                }

                if (any(A < 8) || any(A >= kSimSide - 8) || A.y < 10) { MassA = 0u; }
                if (any(B < 8) || any(B >= kSimSide - 8) || B.y < 10) { MassB = 0u; }
            }

            if (MassA < kMassMin) { MassA = 0u; }
            if (MassB < kMassMin) { MassB = 0u; }

            Args.Grid[IndexA] = PackCell(SolidA, MassA);
            Args.Grid[IndexB] = PackCell(SolidB, MassB);
        }

        struct FDestroyArgs
        {
            FVoxNode* Nodes;
            uint*     Masks;
            uint*     Prefix;
            uint*     Children;
            uint*     SimGrid;
            uint*     SimCoarse;
            float*    Pick;

            // Seven pointers leave the next float4 at offset 56, which straddles a 16 byte boundary.
            uint64_t Pad0;

            float4 Origin;
            float4 Direction;
            float4 SimOrigin;
            float4 Params;
        };

        int3 DestroyCellBase(float3 HitVoxels)
        {
            return int3(floor(HitVoxels / 8.0)) - 4;
        }

        // One thread marches for the aim point, so the crater center never has to round trip to the CPU.
        [shader("compute")]
        [numthreads(1, 1, 1)]
        void PickCS()
        {
            const FDestroyArgs Args = *GetArgs<FDestroyArgs>();

            const float3 Origin = Args.Origin.xyz;
            const float3 Dir = Args.Direction.xyz;

            float3 InvDir;
            InvDir.x = abs(Dir.x) < 1e-7 ? 1e30 : 1.0 / Dir.x;
            InvDir.y = abs(Dir.y) < 1e-7 ? 1e30 : 1.0 / Dir.y;
            InvDir.z = abs(Dir.z) < 1e-7 ? 1e30 : 1.0 / Dir.z;

            float T = 0.001;
            const float Reach = Args.Params.y;

            Args.Pick[3] = 0.0;

            for (int Step = 0; Step < 512; ++Step)
            {
                if (T >= Reach)
                {
                    return;
                }

                const float3 P = Origin + Dir * T;

                const int3 SimLocal = int3(floor(P - Args.SimOrigin.xyz));
                if (Args.Params.z != 0.0 && all(SimLocal >= 0) && all(SimLocal < kSimSide))
                {
                    const uint Index = uint((SimLocal.y * kSimSide + SimLocal.z) * kSimSide + SimLocal.x);
                    const uint Cell = Args.SimGrid[Index];
                    if (CellSolid(Cell) != 0u || CellMass(Cell) >= kMassRender)
                    {
                        Args.Pick[0] = P.x;
                        Args.Pick[1] = P.y;
                        Args.Pick[2] = P.z;
                        Args.Pick[3] = 1.0;
                        return;
                    }

                    const float3 CellMin = Args.SimOrigin.xyz + float3(SimLocal);
                    const float3 Planes = CellMin + step(float3(0.0), Dir);
                    const float3 Exit = (Planes - Origin) * InvDir;
                    T = max(min(Exit.x, min(Exit.y, Exit.z)), T + 1e-3) + 1e-4;
                    continue;
                }

                uint LeafNode;
                int3 LeafBase;
                const int3 Voxel = int3(floor(P));

                if (FindLeaf(Args.Nodes, Args.Masks, Args.Prefix, Args.Children, Voxel, LeafNode, LeafBase))
                {
                    const int3 Local = Voxel - LeafBase;
                    const uint Slot = uint((Local.y * 8 + Local.z) * 8 + Local.x);

                    if (TestSlot(Args.Masks, LeafNode, Slot))
                    {
                        Args.Pick[0] = P.x;
                        Args.Pick[1] = P.y;
                        Args.Pick[2] = P.z;
                        Args.Pick[3] = 1.0;
                        return;
                    }
                }

                const float3 CellMin = float3(Voxel);
                const float3 Planes = CellMin + step(float3(0.0), Dir);
                const float3 Exit = (Planes - Origin) * InvDir;
                T = max(min(Exit.x, min(Exit.y, Exit.z)), T + 1e-3) + 1e-4;
            }
        }

        // One thread per leaf cell in the crater box, so no two threads ever write the same node.
        [shader("compute")]
        [numthreads(4, 4, 4)]
        void DestroyCS(uint3 Thread : SV_DispatchThreadID)
        {
            const FDestroyArgs Args = *GetArgs<FDestroyArgs>();

            if (Args.Pick[3] == 0.0)
            {
                return;
            }

            const float3 Center = float3(Args.Pick[0], Args.Pick[1], Args.Pick[2]);
            const float Radius = Args.Params.x;

            const int3 CellBase = DestroyCellBase(Center) + int3(Thread);
            const int3 VoxelBase = CellBase * 8;

            //~ The dense volume owns its box, so a crater there is cleared directly.

            const int3 SimLocal = VoxelBase - int3(Args.SimOrigin.xyz);
            if (Args.Params.z != 0.0 && all(SimLocal >= 0) && all(SimLocal + 8 <= kSimSide))
            {
                for (int i = 0; i < 512; ++i)
                {
                    const int3 Offset = int3(i & 7, (i >> 6) & 7, (i >> 3) & 7);
                    const float3 P = float3(VoxelBase + Offset) + 0.5;

                    if (distance(P, Center) < Radius)
                    {
                        const int3 L = SimLocal + Offset;
                        Args.SimGrid[uint((L.y * kSimSide + L.z) * kSimSide + L.x)] = 0u;
                    }
                }
                return;
            }

            uint LeafNode;
            int3 LeafBase;
            if (!FindLeaf(Args.Nodes, Args.Masks, Args.Prefix, Args.Children, VoxelBase + 4, LeafNode, LeafBase))
            {
                return;
            }

            uint Clear[16];
            for (int w = 0; w < 16; ++w)
            {
                Clear[w] = 0u;
            }

            bool bAny = false;

            for (int i = 0; i < 512; ++i)
            {
                const int3 Offset = int3(i & 7, (i >> 6) & 7, (i >> 3) & 7);
                const float3 P = float3(LeafBase + Offset) + 0.5;

                if (distance(P, Center) < Radius)
                {
                    const uint Slot = uint((Offset.y * 8 + Offset.z) * 8 + Offset.x);
                    Clear[Slot >> 5u] |= 1u << (Slot & 31u);
                    bAny = true;
                }
            }

            if (!bAny)
            {
                return;
            }

            // Clearing bits never allocates, which is the whole reason destruction can run in place.
            for (int w = 0; w < 16; ++w)
            {
                if (Clear[w] != 0u)
                {
                    Args.Masks[LeafNode * 16u + uint(w)] &= ~Clear[w];
                }
            }

            // A solid leaf ignores its mask, so it has to stop being solid before the hole shows.
            Args.Nodes[LeafNode].Flags &= ~FLAG_SOLID;
        }

        // Rebuilt every tick, because a block that empties has to stop blocking the ray's skip.
        [shader("compute")]
        [numthreads(4, 4, 4)]
        void SimCoarseCS(uint3 Thread : SV_DispatchThreadID)
        {
            const FSimArgs Args = *GetArgs<FSimArgs>();

            if (any(Thread >= uint(kSimCoarseSide)))
            {
                return;
            }

            const int3 Block = int3(Thread) * kSimCoarseStep;
            uint Any = 0u;

            for (int Y = 0; Y < kSimCoarseStep && Any == 0u; ++Y)
            {
                for (int Z = 0; Z < kSimCoarseStep && Any == 0u; ++Z)
                {
                    for (int X = 0; X < kSimCoarseStep; ++X)
                    {
                        const uint Cell = Args.Grid[SimIndex(Block + int3(X, Y, Z))];
                        if (CellSolid(Cell) != 0u || CellMass(Cell) >= kMassRender)
                        {
                            Any = 1u;
                            break;
                        }
                    }
                }
            }

            const uint Index = uint((Thread.y * kSimCoarseSide + Thread.z) * kSimCoarseSide + Thread.x);
            Args.Coarse[Index] = Any;
        }
    )SLANG";
}
