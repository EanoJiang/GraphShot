// Fill out your copyright notice in the Description page of Project Settings.

#include "GraphShotCapture.h"
#include "GraphShotClipboard.h"

#include "EdGraph/EdGraph.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/IConsoleManager.h"
#include "Layout/ChildrenBase.h"
#include "Layout/Geometry.h"
#include "Misc/App.h"
#include "Rendering/SlateLayoutTransform.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "SGraphPanel.h"
#include "SNodePanel.h"
#include "Slate/WidgetRenderer.h"
#include "Styling/AppStyle.h"
#include "TextureResource.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GraphShotCapture"

// SGraphPanel's registered Slate type name (SWidget::GetType).
static const FName GraphPanelTypeName(TEXT("SGraphPanel"));

// ---------------------------------------------------------------------------
// Color-matching knobs (live; no recompile needed).
//
// Slate packs vertex/solid colors as sRGB bytes but samples textures as linear,
// so no single FWidgetRenderer gamma flag makes both match the on-screen editor
// exactly (false => textures dark, true => solid colors bright). These two cvars
// let you dial the capture to your editor:
//
//   GraphShot.UseGamma 0|1  RT sRGB base: 0 = linear RT (darker), 1 = sRGB RT (brighter). Default 1.
//   GraphShot.Gamma <float>  Post-readback pow(rgb, Gamma): >1 darkens, <1 brightens, 1.0 = off. Default 2.0
//                            (2.0 + UseGamma 1 matches the in-editor appearance for this project).
//
// If the capture is too bright, try:  GraphShot.Gamma 2.2   (slightly darker)
// If it is too dark, try:              GraphShot.Gamma 1.8
// Or switch the base:                  GraphShot.UseGamma 0  then adjust GraphShot.Gamma
// ---------------------------------------------------------------------------
static TAutoConsoleVariable<int32> CVarGraphShotUseGamma(
	TEXT("GraphShot.UseGamma"), 1,
	TEXT("RT sRGB base for graph capture: 0 = linear RT (darker), 1 = sRGB RT (brighter)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarGraphShotGamma(
	TEXT("GraphShot.Gamma"), 2.0f,
	TEXT("Post-readback gamma applied to the capture: out = pow(rgb, Gamma). >1 darkens, <1 brightens, 1.0 = off. "
		 "2.0 matches the in-editor appearance when UseGamma=1 on this project."),
	ECVF_Default);

static void Notify(const FText& Message, bool bWarning)
{
	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	Info.FadeInDuration = 0.1f;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration = bWarning ? 6.0f : 3.0f;
	if (bWarning)
	{
		Info.Image = FAppStyle::GetBrush(TEXT("NotificationBlend.Warning"));
	}
	if (FSlateApplication::IsInitialized())
	{
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	if (bWarning)
	{
		UE_LOG(LogTemp, Warning, TEXT("GraphShot: %s"), *Message.ToString());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GraphShot: %s"), *Message.ToString());
	}
}

/**
 * Find the SGraphPanel the user is currently viewing.
 * 1) Walk up from the keyboard-focused widget (precise for hotkey use).
 * 2) Fallback: BFS the active top-level window and pick the largest visible SGraphPanel
 *    (handles toolbar/menu triggers where focus isn't on the graph).
 */
static TSharedPtr<SGraphPanel> FindActiveGraphPanel()
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}

	auto TryAsPanel = [](const TSharedPtr<SWidget>& Widget) -> TSharedPtr<SGraphPanel> {
		if (Widget.IsValid() && Widget->GetType() == GraphPanelTypeName)
		{
			return StaticCastSharedPtr<SGraphPanel>(Widget);
		}
		return nullptr;
	};

	// 1) Keyboard-focused widget and its ancestors.
	TSharedPtr<SWidget> Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
	while (Focus.IsValid())
	{
		if (TSharedPtr<SGraphPanel> Panel = TryAsPanel(Focus))
		{
			return Panel;
		}
		Focus = Focus->GetParentWidget();
	}

	// 2) BFS the active window for the largest visible graph panel.
	TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveTopLevelWindow();
	if (!Window.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<SGraphPanel> BestPanel;
	float BestArea = 0.0f;

	TArray<TSharedPtr<SWidget>> Queue;
	Queue.Add(Window);
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		TSharedPtr<SWidget> Cur = Queue[Head];
		if (!Cur.IsValid())
		{
			continue;
		}

		if (TSharedPtr<SGraphPanel> Panel = TryAsPanel(Cur))
		{
			if (Panel->GetVisibility() == EVisibility::Visible)
			{
				const FVector2f Size = Panel->GetCachedGeometry().GetLocalSize();
				const float Area = Size.X * Size.Y;
				if (Area >= BestArea)
				{
					BestArea = Area;
					BestPanel = Panel;
				}
			}
		}

		if (FChildren* Kids = Cur->GetChildren())
		{
			const int32 NumKids = Kids->Num();
			for (int32 i = 0; i < NumKids; ++i)
			{
				Queue.Add(Kids->GetChildAt(i));
			}
		}
	}

	return BestPanel;
}

/** Build a detached, read-only temp panel over the given graph. It only reads Graph->Nodes and owns its own widgets. */
static TSharedPtr<SGraphPanel> BuildTempPanel(UEdGraph* Graph)
{
	return SNew(SGraphPanel)
		.GraphObj(Graph)
		.IsEditable(false)
		.DisplayAsReadOnly(false)
		.ShowGraphStateOverlay(false)
		.AllowConnectionSlicing(false)
		.ShouldDrawBackground(true)
		.AllowZoom(true)
		.AllowPanning(false);
}

/** Union of every node widget's position + desired size (graph space). */
static bool ComputeAllNodeBounds(SGraphPanel& Panel, FSlateRect& OutBounds)
{
	FVector2f Min(MAX_FLT, MAX_FLT);
	FVector2f Max(-MAX_FLT, -MAX_FLT);
	bool bAny = false;

	FChildren* Kids = Panel.GetManagedChildren();
	if (!Kids)
	{
		return false;
	}

	for (int32 i = 0; i < Kids->Num(); ++i)
	{
		// Every managed child of an SGraphPanel is an SGraphNode, which derives from SNodePanel::SNode.
		TSharedPtr<SWidget> ChildWidget = Kids->GetChildAt(i);
		const TSharedPtr<SNodePanel::SNode> NodeWidget = StaticCastSharedPtr<SNodePanel::SNode>(ChildWidget);
		const FVector2f Pos = NodeWidget->GetPosition2f();
		const FVector2f Size = NodeWidget->GetDesiredSize();

		Min.X = FMath::Min(Min.X, Pos.X);
		Min.Y = FMath::Min(Min.Y, Pos.Y);
		Max.X = FMath::Max(Max.X, Pos.X + Size.X);
		Max.Y = FMath::Max(Max.Y, Pos.Y + Size.Y);
		bAny = true;
	}

	if (!bAny)
	{
		return false;
	}

	OutBounds = FSlateRect(Min.X, Min.Y, Max.X, Max.Y);
	return true;
}

bool FGraphShotCapture::CanCapture()
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	TSharedPtr<SGraphPanel> Panel = FindActiveGraphPanel();
	return Panel.IsValid() && Panel->GetGraphObj() != nullptr && Panel->GetGraphObj()->Nodes.Num() > 0;
}

bool FGraphShotCapture::CaptureToClipboard()
{
	if (!FSlateApplication::IsInitialized())
	{
		Notify(LOCTEXT("NoSlate", "Slate is not initialized."), true);
		return false;
	}

	TSharedPtr<SGraphPanel> LivePanel = FindActiveGraphPanel();
	if (!LivePanel.IsValid())
	{
		Notify(LOCTEXT("NoGraph", "No graph editor is currently focused or open."), true);
		return false;
	}

	UEdGraph* Graph = LivePanel->GetGraphObj();
	if (!Graph || Graph->Nodes.Num() == 0)
	{
		Notify(LOCTEXT("EmptyGraph", "The current graph is empty."), true);
		return false;
	}

	// Build a detached temp panel over the SAME UEdGraph so the live editor is never touched.
	TSharedPtr<SGraphPanel> Panel = BuildTempPanel(Graph);
	if (!Panel.IsValid())
	{
		Notify(LOCTEXT("BuildFail", "Failed to build the capture panel."), true);
		return false;
	}

	// Create + prepass a widget for every node (valid GetDesiredSize).
	Panel->Update();

	FSlateRect Bounds;
	if (!ComputeAllNodeBounds(*Panel, Bounds))
	{
		Notify(LOCTEXT("NoBounds", "Could not compute graph bounds (no node widgets)."), true);
		return false;
	}

	const FVector2f GraphSize = Bounds.GetSize2f();
	const float Pad = 64.0f;
	const float MaxTex = FMath::Min((float)GetMax2DTextureDimension(), 8192.f);

	// Desired scale to fit the whole graph within the render-target limit (never upscale beyond 1:1).
	const float DesiredScale = FMath::Clamp(
		FMath::Min(MaxTex / (GraphSize.X + 2.f * Pad), MaxTex / (GraphSize.Y + 2.f * Pad)),
		0.01f, 1.0f);

	// Zoom is discrete: RestoreViewSettings snaps to the nearest zoom level. Pick the largest level
	// whose amount is <= DesiredScale so the rendered extent never exceeds DrawSize (no clipping)
	// and stays within MaxTex. Level 0 is the most zoomed-out (smallest amount).
	const TSharedPtr<FZoomLevelsContainer>& ZoomLevels = Panel->GetZoomLevels();
	float BestAmount = ZoomLevels->GetZoomAmount(0);
	const int32 NumLevels = ZoomLevels->GetNumZoomLevels();
	for (int32 L = 0; L < NumLevels; ++L)
	{
		const float Amount = ZoomLevels->GetZoomAmount(L);
		if (Amount <= DesiredScale + KINDA_SMALL_NUMBER && Amount >= BestAmount)
		{
			BestAmount = Amount;
		}
	}

	// Apply the view (positive amount => direct zoom, no deferred zoom-to-fit) and read back the
	// real zoom so DrawSize matches it exactly.
	Panel->RestoreViewSettings(FVector2f(Bounds.Left - Pad, Bounds.Top - Pad), BestAmount, FGuid());
	const float ActualZoom = Panel->GetZoomAmount();

	int32 Width = FMath::Clamp(FMath::RoundToInt((GraphSize.X + 2.f * Pad) * ActualZoom), 1, (int32)MaxTex);
	int32 Height = FMath::Clamp(FMath::RoundToInt((GraphSize.Y + 2.f * Pad) * ActualZoom), 1, (int32)MaxTex);

	// Tick the detached panel with a geometry covering the draw size so PopulateVisibleChildren
	// keeps EVERY node (IsNodeCulled returns false within the allotted area). FWidgetRenderer only
	// prepasses; it never ticks, so VisibleChildren must be populated here first.
	const FGeometry TickGeo = FGeometry::MakeRoot(FVector2f((float)Width, (float)Height), FSlateLayoutTransform(1.0f));
	Panel->Tick(TickGeo, FApp::GetCurrentTime(), 0.0f);

	// Render the panel into a render target at the chosen size.
	// The RT sRGB base is selectable via GraphShot.UseGamma (1 = sRGB RT, brighter; 0 = linear RT, darker).
	// See the cvar comments above for why neither is a perfect match and how to tune GraphShot.Gamma.
	const bool bUseGamma = CVarGraphShotUseGamma.GetValueOnGameThread() != 0;
	FWidgetRenderer Renderer(/*bUseGammaCorrection=*/bUseGamma, /*bInClearTarget=*/true);
	UTextureRenderTarget2D* RenderTarget = Renderer.DrawWidget(Panel.ToSharedRef(), FVector2D((double)Width, (double)Height));
	if (!RenderTarget)
	{
		Notify(LOCTEXT("RenderFail", "Failed to render the graph."), true);
		return false;
	}

	FlushRenderingCommands();

	TArray<FColor> Pixels;
	FRenderTarget* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource || !RTResource->ReadPixels(Pixels, FReadSurfaceDataFlags(RCM_UNorm)))
	{
		Notify(LOCTEXT("ReadFail", "Failed to read the rendered pixels."), true);
		RenderTarget->ConditionalBeginDestroy();
		return false;
	}

	// Optional post-readback gamma (GraphShot.Gamma): out = pow(rgb, Gamma). Default 1.0 = off.
	const float GammaPow = CVarGraphShotGamma.GetValueOnGameThread();
	if (!FMath::IsNearlyEqual(GammaPow, 1.0f, KINDA_SMALL_NUMBER))
	{
		for (FColor& Pixel : Pixels)
		{
			auto Apply = [GammaPow](uint8 C) -> uint8
			{
				const float f = FMath::Pow(C / 255.0f, GammaPow);
				return (uint8)FMath::Clamp(FMath::RoundToInt(f * 255.0f), 0, 255);
			};
			Pixel.R = Apply(Pixel.R);
			Pixel.G = Apply(Pixel.G);
			Pixel.B = Apply(Pixel.B);
		}
	}

	const bool bCopied = GraphShotCopyPixelsToClipboard(Pixels, Width, Height);
	RenderTarget->ConditionalBeginDestroy();

	if (bCopied)
	{
		Notify(FText::Format(LOCTEXT("Copied", "Graph screenshot ({0}x{1}) copied to clipboard."), FText::AsNumber(Width), FText::AsNumber(Height)), false);
	}
	else
	{
		Notify(LOCTEXT("CopyFail", "Failed to copy the screenshot to the clipboard."), true);
	}
	return bCopied;
}

#undef LOCTEXT_NAMESPACE
