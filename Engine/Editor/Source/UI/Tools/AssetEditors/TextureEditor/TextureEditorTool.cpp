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
#include "Renderer/TextureStreamingManager.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "Log/Log.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    const char* TexturePreviewName           = "TexturePreview";
    const char* TexturePropertiesName        = "TextureProperties";

    static const char* StockSamplerLabel(RHI::EStockSampler Sampler)
    {
        switch (Sampler)
        {
        case RHI::EStockSampler::LinearClamp:  return "Linear, Clamp";
        case RHI::EStockSampler::LinearMirror: return "Linear, Mirror";
        case RHI::EStockSampler::PointWrap:    return "Nearest, Wrap";
        case RHI::EStockSampler::PointClamp:   return "Nearest, Clamp";
        case RHI::EStockSampler::PointMirror:  return "Nearest, Mirror";
        case RHI::EStockSampler::AnisoWrap:    return "Anisotropic, Wrap";
        case RHI::EStockSampler::AnisoClamp:   return "Anisotropic, Clamp";
        case RHI::EStockSampler::AnisoMirror:  return "Anisotropic, Mirror";
        case RHI::EStockSampler::LinearWrap:
        default:                               return "Linear, Wrap";
        }
    }

    void FTextureEditorTool::OnPropertyEditFinished(const FPropertyChangedEvent& Event)
    {
        // Never Stream, Filter and Address Mode carry no RequiresRecook: no stored pixel depends on them.
        if (Event.Property == nullptr || !Event.Property->HasMetadata("RequiresRecook"))
        {
            return;
        }

        CTexture* Texture = Cast<CTexture>(Asset.Get());
        if (Texture == nullptr)
        {
            return;
        }

        if (RecookForPropertyChange(Texture))
        {
            ImGuiX::Notifications::NotifySuccess("Recooked '{0}' ({1})",
                Texture->GetName().c_str(), Event.PropertyName.c_str());
        }
        else
        {
            ImGuiX::Notifications::NotifyError("Recook failed for '{0}' -- check log",
                Texture->GetName().c_str());
        }
    }

    bool FTextureEditorTool::RecookForPropertyChange(CTexture* Texture)
    {
        // Pristine bytes, on disk or stored on the asset, make an edit replace the last cook rather than layer on it.
        if (!Texture->SourcePath.empty() || Texture->HasSourceFile())
        {
            return CTextureFactory::Recook(Texture);
        }

        if (!SourcelessBaseline.has_value())
        {
            Import::Textures::FTextureImportResult Recovered;
            if (!CTextureFactory::RecoverSourceImage(Texture, Recovered))
            {
                LOG_ERROR("TextureEditor: '{0}' has no source file and its cooked format cannot be decoded; "
                          "settings changes cannot be applied.", Texture->GetName().c_str());
                return false;
            }

            // Only an asset imported before sources were stored lands here; a reimport fixes it for good.
            SourcelessBaseline = Move(Recovered);
        }

        // Copied, not handed over: CookFromSource consumes what it is given and the baseline has to survive.
        Import::Textures::FTextureImportResult Working = SourcelessBaseline.value();
        return CTextureFactory::CookFromSource(Texture, Working);
    }

    void FTextureEditorTool::OnAssetDataChangedExternally()
    {
        // The baseline describes pixels that have just been replaced.
        SourcelessBaseline.reset();
        FAssetEditorTool::OnAssetDataChangedExternally();
    }

    void FTextureEditorTool::OnInitialize()
    {
        CreateToolWindow(TexturePreviewName, [&](bool bFocused)
        {
            CTexture* Texture = Cast<CTexture>(Asset.Get());
            if (!Texture || Texture->TextureResource == nullptr)
            {
                return;
            }

            // Reaching here means ImGui actually began this window: not collapsed, its dock tab selected, and
            // its tool visible. That is the whole condition for holding the streaming pin -- Update reads and
            // clears this next frame. Set before the early-outs below on purpose: an array still building is
            // just as much "the user is looking at it" as one that draws.
            bPreviewDrawnSinceUpdate = true;

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
                // No mip selector: the preview samples the texture through ImGui's bindless 2D path, which
                // takes a heap slot and no explicit LOD, so there is nothing for a slider to change. It
                // used to be here and silently did nothing. Showing a per-mip preview needs per-mip SRVs
                // exposed through the heap.

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
                PropertyRow("Resolution", Format("{}", ImageDesc.Extent.x) + " x " + Format("{}", ImageDesc.Extent.y));

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
                    memoryStr = Format("{}", MemorySizeMB) + " MB";
                    if (MemorySizeMB > 10)  memoryColor = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
                    if (MemorySizeMB > 50)  memoryColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                }
                else
                {
                    memoryStr = Format("{}", MemorySizeKB) + " KB";
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
                    // SizeBytes(), not Pixels.size(): a streamed mip's bytes live in the package's bulk
                    // region, so its Pixels are empty whenever it is not resident and reading them would
                    // report the top of a 4K chain as 0 B.
                    baseMipMemory = Texture->TextureResource->Mips[0].SizeBytes();
                    for (const auto& Mip : Texture->TextureResource->Mips)
                    {
                        totalMipMemory += Mip.SizeBytes();
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
                    ImVec2(0.0f, Math::Min(300.0f, ImGui::GetContentRegionAvail().y * 0.5f))))
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
                        
                        uint32 mipWidth = std::max<uint32>(1u, ImageDesc.Extent.x >> i);
                        uint32 mipHeight = std::max<uint32>(1u, ImageDesc.Extent.y >> i);
                        uint32 mipTexels = mipWidth * mipHeight;
                        
                        // Calculate expected memory size
                        uint32 bytesPerBlock = RHI::Format::BytesPerBlock(ImageDesc.Format);
                        uint64 expectedSize = (uint64)mipWidth * mipHeight * bytesPerBlock;
                        
                        // Get actual size if available
                        uint64 actualSize = expectedSize;
                        if (i < Texture->TextureResource->Mips.size())
                        {
                            actualSize = Texture->TextureResource->Mips[i].SizeBytes();
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
                            ImGui::Text("%llu B", (unsigned long long)actualSize);
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
                
                StatRow("Base Mip Texels:", Format("{}", baseTexels));
                
                if (ImageDesc.NumMips > 1)
                {
                    uint64 mipChainTexels = 0;
                    for (uint32 i = 0; i < ImageDesc.NumMips; ++i)
                    {
                        uint32 mipWidth = std::max<uint32>(1u, ImageDesc.Extent.x >> i);
                        uint32 mipHeight = std::max<uint32>(1u, ImageDesc.Extent.y >> i);
                        mipChainTexels += mipWidth * mipHeight;
                    }
                    StatRow("Total Mip Texels:", Format("{}", mipChainTexels));
                    
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
                    std::swap(Array->SourceTextures[MoveFrom], Array->SourceTextures[MoveTo]);
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
            ImGui::SeparatorText("Settings");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();

            // Reflected rather than hand-drawn, so the table stays complete as settings are added.
            PropertyTable.DrawTree();

            ImGui::Spacing();

            // The settings above are recorded, not applied: the mip chain is whatever the last cook produced.
            {
                const FTextureResource::FDescription& CookedDesc = Texture->TextureResource->ImageDescription;
                const FTextureGroupPolicy Policy = Texture->GetResolvedPolicy();

                const bool bMipsStale = Policy.bGenerateMips != (CookedDesc.NumMips > 1);
                const bool bSizeStale = Policy.MaxDimension > 0
                    && ImMax(CookedDesc.Extent.x, CookedDesc.Extent.y) > Policy.MaxDimension;

                if (bMipsStale || bSizeStale)
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                        "Settings changed since the last cook. Recook to apply them.");
                    ImGui::Spacing();
                }

                if (ImGui::Button("Recook##Texture", ImVec2(-1, 0)))
                {
                    if (CTextureFactory::Recook(Texture))
                    {
                        ImGuiX::Notifications::NotifySuccess("Recooked '{0}'", Texture->GetName().c_str());
                    }
                    else
                    {
                        ImGuiX::Notifications::NotifyError("Recook failed for '{0}' -- check log",
                            Texture->GetName().c_str());
                    }
                }

                // A missing source is a quality note, not a blocker: the cooked mips decode back to an image.
                if (Texture->SourcePath.empty())
                {
                    ImGuiX::TextTooltip("Re-encodes the pixels already in this asset with the settings above. "
                                        "It has no source file (mesh-embedded, or the file moved), so a "
                                        "block-compressed texture takes a second compression generation and "
                                        "tonal adjustments stack on the previous pass instead of replacing it.");
                }
                else
                {
                    ImGuiX::TextTooltip("Re-reads the source file and re-encodes it with the settings above: color space, "
                                        "compression quality, mip policy, size cap and every source adjustment. "
                                        "Source: %s", Texture->SourcePath.c_str());
                }

                ImGui::Spacing();

                // The live split, not the setting: bNeverStream only reaches the split on the next save.
                ImGui::TextUnformatted("Streaming");
                ImGui::SameLine(150);
                if (Texture->IsStreamable())
                {
                    ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "on (mips 0-%u stream)",
                        (uint32)CookedDesc.FirstInlineMip - 1u);
                }
                else
                {
                    ImGui::TextDisabled("off (whole chain resident)");
                }
                ImGuiX::TextTooltip("Never Stream, a group that disables it, or a single-mip texture all turn "
                                    "streaming off. The split is rebuilt when the package is saved.");

                ImGui::TextUnformatted("Sampler");
                ImGui::SameLine(150);
                ImGui::TextUnformatted(StockSamplerLabel(Texture->GetStockSampler()));
                ImGuiX::TextTooltip("What a material samples this texture through when its TextureSample node is "
                                    "left at FromTexture. Materials bake it at compile time, so they need "
                                    "recompiling after a change here.");
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::SeparatorText("Actions");
            ImGuiX::Font::PopFont();

            ImGui::Spacing();
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
        // Backstop for the reconcile in Update: a tool can be torn down without ever getting another Update,
        // and a pinned texture is exempt from budget eviction, so a leaked pin is a permanent leak of however
        // many MiB that texture is.
        bPreviewDrawnSinceUpdate = false;
        UpdateStreamingPin();
    }

    void FTextureEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        UpdateStreamingPin();
    }

    void FTextureEditorTool::UpdateStreamingPin()
    {
        // The preview draws the texture at up to 1:1, so the whole point of streaming (holding only the
        // inline tail) is exactly wrong while it is on screen: showing a 4K texture as a 256px blur is not a
        // preview. Pinning is what fixes that, and being over budget is acceptable for as long as the user is
        // actually looking -- but ONLY that long. Held for the lifetime of the tab instead, ten open 4K tabs
        // sitting in background docks would pin ~200 MiB of mips nothing draws until the editor closes.
        //
        // Reconciled against LAST frame's draw flag: EditorUI calls Update before it draws the tool's
        // windows. The one-frame lag is harmless both ways -- a tab just brought forward sharpens a frame
        // later, and one just hidden holds its mips a frame longer.
        const bool bWantPin = bPreviewDrawnSinceUpdate;
        bPreviewDrawnSinceUpdate = false;

        FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
        if (Streaming == nullptr)
        {
            // No streamer means no pin counts to balance (it is gone, or never existed). Forget ours rather
            // than trying to release it against a manager that cannot honor it.
            PinnedTexture.Reset();
            return;
        }

        CTexture* Desired = bWantPin ? Cast<CTexture>(Asset.Get()) : nullptr;
        CTexture* Current = PinnedTexture.Get();
        if (Current == Desired)
        {
            return;
        }

        // Unpin first, so a reimport that swapped the asset releases the OLD texture rather than leaking it
        // and double-pinning the new one.
        if (Current != nullptr)
        {
            Streaming->Unpin(Current);
        }
        if (Desired != nullptr)
        {
            Streaming->Pin(Desired);
        }

        PinnedTexture = Desired;
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
