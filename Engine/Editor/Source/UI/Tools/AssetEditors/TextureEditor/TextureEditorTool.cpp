#include "TextureEditorTool.h"

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "Assets/Factories/TextureFactory/TextureFactory.h"
#include "Assets/Factories/TextureFactory/TextureArrayFactory.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Renderer/RenderManager.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    const char* TexturePreviewName           = "TexturePreview";
    const char* TexturePropertiesName        = "TextureProperties";

    void FTextureEditorTool::OnInitialize()
    {
        CreateToolWindow(TexturePreviewName, [&](bool bFocused)
        {
            CTexture* Texture = Cast<CTexture>(Asset.Get());
            if (!Texture || Texture->TextureResource == nullptr)
            {
                return;
            }

            // An array with nothing built has no GPU image at all, so there is no slice to show and the
            // usual path would sample the null slot. Say what to do instead of drawing a purple square.
            const CTextureArray* PreviewArray = Cast<CTextureArray>(Texture);
            if (PreviewArray != nullptr && PreviewArray->GetNumLayers() == 0)
            {
                auto CenteredText = [](const char* Text)
                {
                    const float Width = ImGui::GetContentRegionAvail().x;
                    const float TextW = ImGui::CalcTextSize(Text).x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax((Width - TextW) * 0.5f, 0.0f));
                    ImGui::TextUnformatted(Text);
                };

                ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.4f));
                CenteredText("No layers built yet.");
                CenteredText("Add sources under Array Layers, then press Build Array.");
                return;
            }

            // New-heap sampling by ResourceID (per-mip preview deferred until the new RHI exposes mip SRVs).
            const int32 TexResourceID = Texture->GetResourceID();
            ImTextureID TextureID = (ImTextureID)(uint32)(TexResourceID >= 0 ? (uint32)TexResourceID : 0u);

            const FTextureResource::FDescription& ImageDesc = Texture->TextureResource->ImageDescription;

            // Float formats are the cooked-HDR path (Environment color space, scene captures): their
            // texels are linear radiance rather than display-encoded bytes. Drives both the display
            // transform on the image below and whether the exposure control is shown at all.
            const bool bIsHDRPreview =
                ImageDesc.Format == EFormat::RGBA16_FLOAT ||
                ImageDesc.Format == EFormat::RGBA32_FLOAT ||
                ImageDesc.Format == EFormat::R11G11B10_FLOAT;

            // Slice picker, above the image so it doesn't move as the preview is panned/zoomed.
            // Clamped every frame against the live count: a rebuild can shorten the array, and an
            // out-of-range layer index is undefined on the GPU rather than a wrap.
            if (PreviewArray != nullptr)
            {
                const uint32 Layers = PreviewArray->GetNumLayers();
                PreviewSlice = ImMin(PreviewSlice, Layers - 1u);

                ImGui::TextUnformatted("Slice");
                ImGui::SameLine(60);
                ImGui::SetNextItemWidth(-1);
                if (Layers > 1)
                {
                    int SliceInt = (int)PreviewSlice;
                    if (ImGui::SliderInt("##ArraySlice", &SliceInt, 0, (int)Layers - 1, "%d"))
                    {
                        PreviewSlice = (uint32)ImClamp(SliceInt, 0, (int)Layers - 1);
                    }
                }
                else
                {
                    ImGui::TextDisabled("0 (single layer)");
                }
                ImGui::Separator();
            }

            // Captured AFTER the slice picker, not before: the checkerboard and the image are painted
            // from these across the whole remaining region, so a position taken above the picker would
            // put them over it -- the slider still hit-tests, but nothing of it is visible.
            ImVec2 WindowSize = ImGui::GetContentRegionAvail();
            ImVec2 WindowPos  = ImGui::GetCursorScreenPos();

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            {
                float Wheel = ImGui::GetIO().MouseWheel;
                if (Wheel != 0.0f)
                {
                    float ZoomSpeed = ImGui::GetIO().KeyCtrl ? 0.025f : 0.25f;
                    ZoomFactor += Wheel * ZoomSpeed;
                    ZoomFactor = ImClamp(ZoomFactor, 0.1f, 20.0f);
                }

                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                {
                    ImVec2 MouseDelta = ImGui::GetIO().MouseDelta;
                    PanOffset.x += MouseDelta.x;
                    PanOffset.y += MouseDelta.y;
                }

                // Reset view on double-click
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    ZoomFactor = 1.0f;
                    PanOffset = ImVec2(0, 0);
                }
            }

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            const int CheckerSize = 16;
            const ImU32 CheckerColor1 = IM_COL32(50, 50, 55, 255);
            const ImU32 CheckerColor2 = IM_COL32(40, 40, 45, 255);

            for (int y = 0; y < (int)WindowSize.y; y += CheckerSize)
            {
                for (int x = 0; x < (int)WindowSize.x; x += CheckerSize)
                {
                    bool isEven = ((x / CheckerSize) + (y / CheckerSize)) % 2 == 0;
                    ImU32 color = isEven ? CheckerColor1 : CheckerColor2;
                    DrawList->AddRectFilled(ImVec2(WindowPos.x + x, WindowPos.y + y), ImVec2(WindowPos.x + x + CheckerSize, WindowPos.y + y + CheckerSize), color);
                }
            }

            ImVec2 TextureSize = ImVec2((float)ImageDesc.Extent.x, (float)ImageDesc.Extent.y);
            ImVec2 ScaledSize = ImVec2(TextureSize.x * ZoomFactor, TextureSize.y * ZoomFactor);
            ImVec2 CenterPos = ImVec2(WindowPos.x + (WindowSize.x - ScaledSize.x) * 0.5f + PanOffset.x,WindowPos.y + (WindowSize.y - ScaledSize.y) * 0.5f + PanOffset.y);

            // Draw border around texture
            DrawList->AddRect(
                ImVec2(CenterPos.x - 1, CenterPos.y - 1),
                ImVec2(CenterPos.x + ScaledSize.x + 1, CenterPos.y + ScaledSize.y + 1),
                IM_COL32(100, 100, 120, 255),
                0.0f,
                0,
                2.0f
            );

            // Float formats hold LINEAR radiance, so they need the scene's display transform to read
            // the way the same texture does in-world; blitting them straight to the _UNORM swapchain
            // is what made cooked HDRIs look far darker here than in the viewport. Cooked color
            // textures are already display-encoded and must go through untouched.
            // Array slices must be sampled through gTextures2DArray[]; the two preview modes write the
            // same display state, so they cannot both be active (array layers are always LDR anyway,
            // since the layer cook rejects float sources outright).
            const bool bIsArrayPreview = (PreviewArray != nullptr);
            if (bIsArrayPreview)
            {
                ImGuiX::BeginArrayPreview(DrawList, PreviewSlice);
            }
            else if (bIsHDRPreview)
            {
                ImGuiX::BeginHDRPreview(DrawList, ExposureStops);
            }

            DrawList->AddImage(
                TextureID,
                CenterPos,
                ImVec2(CenterPos.x + ScaledSize.x, CenterPos.y + ScaledSize.y),
                ImVec2(0, 0),
                ImVec2(1, 1)
            );

            if (bIsArrayPreview)
            {
                ImGuiX::EndArrayPreview(DrawList);
            }
            else if (bIsHDRPreview)
            {
                ImGuiX::EndHDRPreview(DrawList);
            }

            ImVec2 MousePos = ImGui::GetMousePos();
            bool isOverTexture = MousePos.x >= CenterPos.x && MousePos.x <= CenterPos.x + ScaledSize.x &&
                MousePos.y >= CenterPos.y && MousePos.y <= CenterPos.y + ScaledSize.y;

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && isOverTexture)
            {
                float NormX = (MousePos.x - CenterPos.x) / ScaledSize.x;
                float NormY = (MousePos.y - CenterPos.y) / ScaledSize.y;
                int PixelX = (int)(NormX * TextureSize.x);
                int PixelY = (int)(NormY * TextureSize.y);

                // Draw crosshair
                const ImU32 CrosshairColor = IM_COL32(255, 255, 255, 180);
                DrawList->AddLine(ImVec2(CenterPos.x, MousePos.y), ImVec2(CenterPos.x + ScaledSize.x, MousePos.y), CrosshairColor);
                DrawList->AddLine(ImVec2(MousePos.x, CenterPos.y), ImVec2(MousePos.x, CenterPos.y + ScaledSize.y), CrosshairColor);

                // Pixel info tooltip
                ImGui::BeginTooltip();
                ImGui::Text("Pixel: [%d, %d]", PixelX, PixelY);
                ImGui::Text("UV: [%.3f, %.3f]", NormX, NormY);
                ImGui::EndTooltip();
            }

            ImGui::SetCursorScreenPos(ImVec2(WindowPos.x + 10, WindowPos.y + 10));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 20, 25, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));

            if (ImGui::BeginChild("##Toolbar", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding))
            {
                // Mip level selector
                if (ImageDesc.NumMips > 1)
                {
                    ImGui::Text("Mip Level:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);
                    ImGui::SliderInt("##MipLevel", &CurrentMipLevel, 0, ImageDesc.NumMips - 1);
                }

                // HDR preview exposure: only for float-format textures (cooked HDRIs);
                // hidden for LDR so the toolbar has no unused controls.
                if (bIsHDRPreview)
                {
                    if (ImageDesc.NumMips > 1)
                    {
                        ImGui::SameLine(0, 20);
                    }
                    ImGui::Text("Exposure:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140);
                    ImGui::SliderFloat("##Exposure", &ExposureStops, -8.0f, 4.0f, "%+.1f stops");
                    ImGuiX::TextTooltip("Exposure bias on the preview, in stops. The image is tone-mapped with the same transform the viewport uses, so 0 stops shows the texture as the scene sees it; dial down to read detail in bright regions.");
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();

            ImGui::SetCursorScreenPos(ImVec2(WindowPos.x + 10, WindowPos.y + WindowSize.y - 35));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 20, 25, 230));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));

            if (ImGui::BeginChild("##InfoBar", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding))
            {
                ImGui::Text("%.0f%% | %dx%d | Pan: [%.0f, %.0f]",
                    ZoomFactor * 100.0f,
                    (int)TextureSize.x,
                    (int)TextureSize.y,
                    PanOffset.x,
                    PanOffset.y
                );
                ImGui::SameLine(0, 20);
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "RMB/MMB: Pan | Scroll: Zoom | Ctrl+Scroll: Fine Zoom | Double-Click: Reset");
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        });
    
        CreateToolWindow(TexturePropertiesName, [&](bool bFocused)
        {
            CTexture* Texture = Cast<CTexture>(Asset.Get());
            if (!Texture)
            {
                return;
            }
        
            const FTextureResource::FDescription& ImageDesc = Texture->TextureResource->ImageDescription;
        
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", Texture->GetName().c_str());
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
        
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Texture Information");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
        
            if (ImGui::BeginTable("##TextureInfo", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
            
                auto PropertyRow = [](const char* label, const FString& value, const ImVec4* color = nullptr)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    if (color)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, *color);
                    }
                    ImGui::TextUnformatted(value.c_str());
                    if (color)
                    {
                        ImGui::PopStyleColor();
                    }
                };
            
                ImVec4 dimensionColor(0.5f, 0.9f, 0.5f, 1.0f);
                PropertyRow("Type", "2D Texture", &dimensionColor);
                PropertyRow("Resolution", eastl::to_string(ImageDesc.Extent.x) + " x " + eastl::to_string(ImageDesc.Extent.y));

                // Format
                const FFormatInfo& FormatInfo = RHI::Format::Info(ImageDesc.Format);
                PropertyRow("Pixel Format", FString(FormatInfo.Name));
                
                // Memory
                size_t MemorySizeBytes = Texture->TextureResource->CalcTotalSizeBytes();
                size_t MemorySizeKB = MemorySizeBytes / 1024;
                size_t MemorySizeMB = MemorySizeKB / 1024;
                
                FString memoryStr;
                ImVec4 memoryColor(0.7f, 1.0f, 0.7f, 1.0f);
                
                if (MemorySizeMB > 0)
                {
                    memoryStr = eastl::to_string(MemorySizeMB) + " MB";
                    if (MemorySizeMB > 10)  memoryColor = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
                    if (MemorySizeMB > 50)  memoryColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                }
                else
                {
                    memoryStr = eastl::to_string(MemorySizeKB) + " KB";
                }
                
                PropertyRow("Memory Size", memoryStr, &memoryColor);
            
                ImGui::EndTable();
            }
        
            ImGui::Spacing();
            ImGui::Spacing();
        
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Mipmap Chain");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
        
            if (ImageDesc.NumMips > 1)
            {
                // Summary stats
                uint64 totalMipMemory = 0;
                uint64 baseMipMemory = 0;
                
                if (!Texture->TextureResource->Mips.empty())
                {
                    baseMipMemory = Texture->TextureResource->Mips[0].Pixels.size();
                    for (const auto& Mip : Texture->TextureResource->Mips)
                    {
                        totalMipMemory += Mip.Pixels.size();
                    }
                }
                
                float mipOverhead = baseMipMemory > 0 ? ((float)(totalMipMemory - baseMipMemory) / (float)baseMipMemory * 100.0f) : 0.0f;
                
                ImGui::Text("Total Mip Levels: ");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "%u", ImageDesc.NumMips);
                
                ImGui::Text("Memory Overhead: ");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "%.1f%%", mipOverhead);
                ImGui::SameLine();
                ImGui::TextDisabled("(~33%% is typical)");
        
                ImGui::Spacing();
        
                // Mip level table
                if (ImGui::BeginTable("##MipLevels", 5, 
                    ImGuiTableFlags_Borders | 
                    ImGuiTableFlags_RowBg | 
                    ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingFixedFit,
                    ImVec2(0.0f, eastl::min(300.0f, ImGui::GetContentRegionAvail().y * 0.5f))))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Texels", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("% of Total", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();
        
                    for (uint32 i = 0; i < ImageDesc.NumMips; ++i)
                    {
                        ImGui::TableNextRow();
                        
                        uint32 mipWidth = eastl::max<uint32>(1u, ImageDesc.Extent.x >> i);
                        uint32 mipHeight = eastl::max<uint32>(1u, ImageDesc.Extent.y >> i);
                        uint32 mipTexels = mipWidth * mipHeight;
                        
                        // Calculate expected memory size
                        uint32 bytesPerBlock = RHI::Format::BytesPerBlock(ImageDesc.Format);
                        uint64 expectedSize = (uint64)mipWidth * mipHeight * bytesPerBlock;
                        
                        // Get actual size if available
                        uint64 actualSize = expectedSize;
                        if (i < Texture->TextureResource->Mips.size())
                        {
                            actualSize = Texture->TextureResource->Mips[i].Pixels.size();
                        }
                        
                        float percentOfTotal = totalMipMemory > 0 ? ((float)actualSize / (float)totalMipMemory * 100.0f) : 0.0f;
        
                        // Level number
                        ImGui::TableSetColumnIndex(0);
                        if (i == 0)
                        {
                            ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "%u", i);
                        }
                        else
                        {
                            ImGui::Text("%u", i);
                        }
        
                        // Resolution
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%ux%u", mipWidth, mipHeight);
        
                        // Texel count
                        ImGui::TableSetColumnIndex(2);
                        if (mipTexels >= 1000000)
                        {
                            ImGui::Text("%.2fM", mipTexels / 1000000.0f);
                        }
                        else if (mipTexels >= 1000)
                        {
                            ImGui::Text("%.1fK", mipTexels / 1000.0f);
                        }
                        else
                        {
                            ImGui::Text("%u", mipTexels);
                        }
        
                        // Size
                        ImGui::TableSetColumnIndex(3);
                        if (actualSize >= 1024 * 1024)
                        {
                            ImGui::Text("%.2f MB", actualSize / (1024.0f * 1024.0f));
                        }
                        else if (actualSize >= 1024)
                        {
                            ImGui::Text("%.1f KB", actualSize / 1024.0f);
                        }
                        else
                        {
                            ImGui::Text("%llu B", actualSize);
                        }
        
                        // Percentage
                        ImGui::TableSetColumnIndex(4);
                        if (i == 0)
                        {
                            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "%.1f%%", percentOfTotal);
                        }
                        else
                        {
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%.1f%%", percentOfTotal);
                        }
                    }
        
                    ImGui::EndTable();
                }
            }
            else
            {
                ImGui::TextDisabled("No mipmaps generated (single mip level only)");
            }
        
            ImGui::Spacing();
            ImGui::Spacing();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Statistics");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            float aspectRatio = (float)ImageDesc.Extent.x / (float)ImageDesc.Extent.y;
            uint32 baseTexels = ImageDesc.Extent.x * ImageDesc.Extent.y;
            
            if (ImGui::BeginTable("##Stats", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("##Label", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);
                
                auto StatRow = [](const char* label, const FString& value)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(value.c_str());
                };
                
                char aspectStr[32];
                snprintf(aspectStr, sizeof(aspectStr), "%.3f:1", aspectRatio);
                StatRow("Aspect Ratio:", aspectStr);
                
                StatRow("Base Mip Texels:", eastl::to_string(baseTexels));
                
                if (ImageDesc.NumMips > 1)
                {
                    uint64 mipChainTexels = 0;
                    for (uint32 i = 0; i < ImageDesc.NumMips; ++i)
                    {
                        uint32 mipWidth = eastl::max<uint32>(1u, ImageDesc.Extent.x >> i);
                        uint32 mipHeight = eastl::max<uint32>(1u, ImageDesc.Extent.y >> i);
                        mipChainTexels += mipWidth * mipHeight;
                    }
                    StatRow("Total Mip Texels:", eastl::to_string(mipChainTexels));
                    
                    float texelIncrease = ((float)mipChainTexels / (float)baseTexels - 1.0f) * 100.0f;
                    char texelIncStr[32];
                    snprintf(texelIncStr, sizeof(texelIncStr), "+%.1f%%", texelIncrease);
                    StatRow("Texel Increase:", texelIncStr);
                }
        
                ImGui::EndTable();
            }
        
            ImGui::Spacing();
            ImGui::Spacing();
            
            // Array layer list. Only shown for CTextureArray; a plain texture has no layers and the
            // section would just be dead UI.
            if (CTextureArray* Array = Cast<CTextureArray>(Texture))
            {
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
                ImGui::SeparatorText("Array Layers");
                ImGuiX::Font::PopFont();

                ImGui::Spacing();
                ImGui::Text("%u layer(s) built", Array->GetNumLayers());
                ImGuiX::TextTooltip("Sample a layer in a material with TextureSampleArray; Slice 0 is the first entry below.");

                ImGui::Spacing();

                int RemoveIndex = -1;
                int MoveFrom    = -1;
                int MoveTo      = -1;
                for (size_t i = 0; i < Array->SourceTextures.size(); ++i)
                {
                    const TObjectPtr<CTexture>& Layer = Array->SourceTextures[i];

                    ImGui::PushID((int)i);
                    ImGui::Text("%2zu", i);
                    ImGui::SameLine(40);
                    ImGui::TextUnformatted(Layer.IsValid() ? Layer->GetName().c_str() : "<missing>");

                    // Slice order is the material's Slice index, so reordering has to be possible
                    // without clearing and re-adding the whole list.
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 66.0f);
                    ImGui::BeginDisabled(i == 0);
                    if (ImGui::SmallButton("^")) { MoveFrom = (int)i; MoveTo = (int)i - 1; }
                    ImGui::EndDisabled();

                    ImGui::SameLine();
                    ImGui::BeginDisabled(i + 1 >= Array->SourceTextures.size());
                    if (ImGui::SmallButton("v")) { MoveFrom = (int)i; MoveTo = (int)i + 1; }
                    ImGui::EndDisabled();

                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) { RemoveIndex = (int)i; }
                    ImGui::PopID();
                }

                if (MoveFrom >= 0)
                {
                    eastl::swap(Array->SourceTextures[MoveFrom], Array->SourceTextures[MoveTo]);
                    Asset->GetPackage()->MarkDirty();
                }
                if (RemoveIndex >= 0)
                {
                    Array->SourceTextures.erase(Array->SourceTextures.begin() + RemoveIndex);
                    Asset->GetPackage()->MarkDirty();
                }

                ImGui::Spacing();

                // Drop target rather than a file dialog: layers are texture ASSETS now, so they come
                // from the content browser and keep working when the project moves machines.
                ImGui::Button("Drop textures here to add a layer##TextureArray", ImVec2(-1, 0));
                if (ImGui::BeginDragDropTarget())
                {
                    if (CTexture* Dropped = DragDrop::AcceptAsset<CTexture>())
                    {
                        // A nested array would recurse through layer-major mips and build a silently
                        // wrong image; Rebuild refuses it too, but there is no reason to accept it here.
                        if (Dropped->IsA<CTextureArray>())
                        {
                            ImGuiX::Notifications::NotifyError("'{0}' is a texture array; layers must be plain textures",
                                Dropped->GetName().c_str());
                        }
                        else
                        {
                            Array->SourceTextures.push_back(Dropped);
                            Asset->GetPackage()->MarkDirty();
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGuiX::TextTooltip("Appended as the next slice. Every layer must match layer 0's size and format, "
                                    "or enable Resize Layers To First.");

                ImGui::Spacing();
                const bool bCanBuild = !Array->SourceTextures.empty();
                ImGui::BeginDisabled(!bCanBuild);
                if (ImGui::Button("Build Array##TextureArray", ImVec2(-1, 0)))
                {
                    if (CTextureArrayFactory::Rebuild(Array))
                    {
                        ImGuiX::Notifications::NotifySuccess("Built '{0}' with {1} layers",
                            Array->GetName().c_str(), Array->GetNumLayers());
                    }
                    else
                    {
                        ImGuiX::Notifications::NotifyError("Build failed for '{0}' -- check log",
                            Array->GetName().c_str());
                    }
                }
                ImGui::EndDisabled();
                ImGuiX::TextTooltip("Re-cooks every layer from its source file, in list order, into one Texture2DArray.");

                ImGui::Spacing();
                ImGui::Spacing();
            }

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Compression");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            // Color-space combo + recook. ColorSpace alone doesn't re-encode (BC baked at import);
            // Recook applies the new format. Stored format shown so staleness is visible.
            {
                static const char* ColorSpaceLabels[] =
                {
                    "Auto (re-classify on next recook)",
                    "Linear (data, non-color)",
                    "sRGB (color: albedo / emissive / UI)",
                    "Normal Map (BC5, XY + reconstructed Z)",
                    "Packed Data (ORM / MRA / etc.)",
                    "HDR Environment (RGBA16F equirect, no compression)",
                };
                int CurrentIndex = (int)Texture->ColorSpace;
                ImGui::TextUnformatted("Color Space");
                ImGui::SameLine(150);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##ColorSpace", &CurrentIndex, ColorSpaceLabels, IM_ARRAYSIZE(ColorSpaceLabels)))
                {
                    Texture->ColorSpace = (ETextureColorSpace)CurrentIndex;
                    Asset->GetPackage()->MarkDirty();
                }

                ImGui::Spacing();
                if (Texture->SourcePath.empty())
                {
                    ImGui::BeginDisabled();
                    ImGui::Button("Recook (no source path)##Texture", ImVec2(-1, 0));
                    ImGui::EndDisabled();
                    ImGuiX::TextTooltip("This asset wasn't imported from a standalone file (likely embedded in a mesh import), so there's nothing on disk to re-cook from.");
                }
                else
                {
                    if (ImGui::Button("Recook##Texture", ImVec2(-1, 0)))
                    {
                        if (CTextureFactory::Recook(Texture))
                        {
                            ImGuiX::Notifications::NotifySuccess("Recooked '{0}' as {1}",
                                Texture->GetName().c_str(), ColorSpaceLabels[(int)Texture->ColorSpace]);
                        }
                        else
                        {
                            ImGuiX::Notifications::NotifyError("Recook failed for '{0}' -- check log",
                                Texture->GetName().c_str());
                        }
                    }
                    ImGuiX::TextTooltip("Re-run Basis Universal compression with the current Color Space, picking the right BC format and encoder mode. Source: %s", Texture->SourcePath.c_str());
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Actions");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();
            ImGui::TextDisabled("Transform Tools");
            ImGui::Spacing();
            
            if (ImGui::Button("Flip Vertical##Texture", ImVec2(150, 0)))
            {
                for (FTextureResource::FMip& Mip : Texture->GetTextureResource().Mips)
                {
                    uint8* Pixels           = Mip.Pixels.data();
                    uint32 Height           = Mip.Height;
                    uint32 RowSize          = Mip.RowPitch;
                    
                    TVector<uint8> TempRow(RowSize);

                    for (uint32 Y = 0; Y < Height / 2; Y++)
                    {
                        uint8* Top    = Pixels + Y * RowSize;
                        uint8* Bottom = Pixels + (Height - 1 - Y) * RowSize;
                    
                        memcpy(TempRow.data(), Top, RowSize);
                        memcpy(Top, Bottom, RowSize);
                        memcpy(Bottom, TempRow.data(), RowSize);
                    }
                }
                
                Asset->PostLoad();
                Asset->GetPackage()->MarkDirty();
            }
            ImGuiX::TextTooltip("Flip the texture along the vertical axis.");
            
            ImGui::SameLine();
            
            if (ImGui::Button("Flip Horizontal##Texture", ImVec2(150, 0)))
            {
                for (FTextureResource::FMip& Mip : Texture->GetTextureResource().Mips)
                {
                    uint8* Pixels        = Mip.Pixels.data();
                    uint32 Width         = Mip.Width;
                    uint32 Height        = Mip.Height;
                    uint32 BytesPerPixel = Mip.RowPitch / Mip.Width;

                    for (uint32 Y = 0; Y < Height; Y++)
                    {
                        for (uint32 X = 0; X < Width / 2; X++)
                        {
                            uint8* Left  = Pixels + (Y * Width + X) * BytesPerPixel;
                            uint8* Right = Pixels + (Y * Width + (Width - 1 - X)) * BytesPerPixel;

                            for (uint32 C = 0; C < BytesPerPixel; C++)
                            {
                                std::swap(Left[C], Right[C]);
                            }
                        }
                    }
                }
                
                Asset->PostLoad();
                Asset->GetPackage()->MarkDirty();
            }
            
            ImGuiX::TextTooltip("Flip the texture along the horizontal axis.");

            if (ImGui::Button("Export to File...", ImVec2(-1, 0)))
            {
                // TODO: Implement export
            }
            ImGuiX::TextTooltip("Export the texture to disk.");
            
            if (ImGui::Button("Analyze Color Distribution", ImVec2(-1, 0)))
            {
                // TODO: Implement analysis
            }
            ImGuiX::TextTooltip("Analyze the color distribution across all pixels.");
        });
    }

    void FTextureEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FTextureEditorTool::OnAssetLoadFinished()
    {
    }

    void FTextureEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::DrawToolMenu(UpdateContext);
    }

    void FTextureEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Pan / Zoom",
            "Middle-mouse drag pans, mouse wheel zooms. Hold Ctrl while scrolling to zoom finer. "
            "The status bar shows current zoom percentage.");
        DrawHelpTextRow("Mip Levels",
            "If the texture has mips, the Mip slider lets you preview each level. The on-disk pixel "
            "data is unchanged, this only changes which level is sampled for display.");
        DrawHelpTextRow("HDR / Exposure",
            "HDR textures (RGBA16F, RGB9E5, etc.) store linear radiance, so they are tone-mapped for "
            "display using the same transform the scene viewport applies. At 0 stops the preview shows "
            "the texture as the renderer sees it; the Exposure slider biases that to read into very "
            "bright or very dark regions.");
        DrawHelpTextRow("Channels",
            "Toggle R/G/B/A channels from the View menu to inspect them in isolation. "
            "Useful for verifying packed maps (e.g. ORM, normal maps with alpha height).");
        DrawHelpTextRow("Compression / Format",
            "Format and size are shown in the Details panel. Re-import via Content Browser to change "
            "compression settings (BC1/BC3/BC5/BC7).");
    }

    void FTextureEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &rightDockID, &leftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(TexturePreviewName).c_str(), leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(TexturePropertiesName).c_str(), rightDockID);
    }
}
