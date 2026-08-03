    void FWorldEditorTool::DrawNetworkDebugOverlay()
    {
        if (!bDrawNetworkDebug || World == nullptr)
        {
            return;
        }
        FNetWorldState* Net = ECS::GetWorldRegistry(*World).ctx().find<FNetWorldState>();
        if (Net == nullptr)
        {
            return; // world isn't networked
        }

        const FNetExtract& Ex   = Net->Extract;
        const FNetGrid&    Grid = Net->Grid;
        const float        Y    = 0.05f; // just above the ground plane

        // 1) Occupied grid cells only (cheap even with a huge grid -- empty cells are skipped).
        const FVector4 CellColor(0.30f, 0.55f, 1.0f, 1.0f);
        const int32 NumCells = Grid.NumCells();
        if (static_cast<int32>(Grid.CellStart.size()) == NumCells + 1)
        {
            for (int32 cz = 0; cz < Grid.DimZ; ++cz)
            {
                for (int32 cx = 0; cx < Grid.DimX; ++cx)
                {
                    const int32 C = Grid.CellIndex(cx, cz);
                    if (Grid.CellStart[C + 1] <= Grid.CellStart[C]) { continue; }
                    const FVector3 M = Grid.CellOrigin(cx, cz);
                    const float    s = Grid.CellSize;
                    const FVector3 A(M.x,     Y, M.z);
                    const FVector3 B(M.x + s, Y, M.z);
                    const FVector3 Cc(M.x + s, Y, M.z + s);
                    const FVector3 D(M.x,     Y, M.z + s);
                    World->DrawLine(A, B,  CellColor, 1.0f, false, -1.0f);
                    World->DrawLine(B, Cc, CellColor, 1.0f, false, -1.0f);
                    World->DrawLine(Cc, D, CellColor, 1.0f, false, -1.0f);
                    World->DrawLine(D, A,  CellColor, 1.0f, false, -1.0f);
                }
            }
        }

        // 2) Per-client AOI circles on the XZ plane: enter (green) + leave (yellow).
        const SDefaultWorldSettings& Settings = World->GetDefaultWorldSettings();
        auto DrawCircleXZ = [&](const FVector3& Center, float Radius, const FVector4& Col)
        {
            constexpr int Segs = 48;
            FVector3 Prev;
            for (int i = 0; i <= Segs; ++i)
            {
                const float a = (static_cast<float>(i) / Segs) * 6.2831853f;
                const FVector3 P(Center.x + std::cos(a) * Radius, Center.y, Center.z + std::sin(a) * Radius);
                if (i > 0) { World->DrawLine(Prev, P, Col, 1.5f, false, -1.0f); }
                Prev = P;
            }
        };
        for (const auto& KV : Net->OwnerToRecord)
        {
            const uint32 Rec = KV.second;
            if (Rec >= Ex.Num()) { continue; }
            const FVector3 VP = Ex.Pos[Rec];
            DrawCircleXZ(VP, Settings.AOIEnterRadius, FVector4(0.2f, 1.0f, 0.3f, 1.0f));
            DrawCircleXZ(VP, Settings.AOILeaveRadius, FVector4(1.0f, 0.9f, 0.2f, 1.0f));
        }

        // 3) Relevant entities per client, marked + coloured by LOD tier (near red / mid yellow / far green).
        static const FVector4 TierCol[4] = {
            FVector4(1.0f, 0.25f, 0.25f, 1.0f), // Near
            FVector4(1.0f, 0.85f, 0.20f, 1.0f), // Mid
            FVector4(0.30f, 1.0f, 0.45f, 1.0f), // Far
            FVector4(0.5f,  0.5f,  0.5f,  1.0f), // Cull (shouldn't appear)
        };
        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (const auto& CVKV : Net->ClientViews)
        {
            const FNetClientView& CV = CVKV.second;
            for (const auto& RKV : CV.Relevant)
            {
                const entt::entity E = Net->GuidTable.Find(FNetGUID{ RKV.first });
                if (E == entt::null || !Registry.valid(E)) { continue; }
                STransformComponent* T = Registry.try_get<STransformComponent>(E);
                if (T == nullptr) { continue; }
                const FVector3  P   = T->GetWorldLocationCached();
                const FVector4& Col = TierCol[static_cast<int>(RKV.second.Tier) & 3];
                const float     r   = 0.5f;
                World->DrawLine(P - FVector3(r, 0, 0), P + FVector3(r, 0, 0), Col, 2.0f, false, -1.0f);
                World->DrawLine(P - FVector3(0, 0, r), P + FVector3(0, 0, r), Col, 2.0f, false, -1.0f);
                World->DrawLine(P, P + FVector3(0, r * 2.0f, 0),               Col, 2.0f, false, -1.0f);
            }
        }
    }

