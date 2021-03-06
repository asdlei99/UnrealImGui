// Distributed under the MIT License (MIT) (see accompanying LICENSE file)

#include "ImGuiPrivatePCH.h"

#include "ImGuiContextProxy.h"

#include "ImGuiImplementation.h"
#include "ImGuiInteroperability.h"

#include <Runtime/Launch/Resources/Version.h>


static constexpr float DEFAULT_CANVAS_WIDTH = 3840.f;
static constexpr float DEFAULT_CANVAS_HEIGHT = 2160.f;


namespace CVars
{
	extern TAutoConsoleVariable<int> DebugDrawOnWorldTick;
}

namespace
{
	FString GetSaveDirectory()
	{
#if (ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION >= 18))
		const FString SavedDir = FPaths::ProjectSavedDir();
#else
		const FString SavedDir = FPaths::GameSavedDir();
#endif

		FString Directory = FPaths::Combine(*SavedDir, TEXT("ImGui"));

		// Make sure that directory is created.
		IPlatformFile::GetPlatformPhysical().CreateDirectory(*Directory);

		return Directory;
	}

	FString GetIniFile(const FString& Name)
	{
		static FString SaveDirectory = GetSaveDirectory();
		return FPaths::Combine(SaveDirectory, Name + TEXT(".ini"));
	}
}

FImGuiContextProxy::FImGuiContextProxy(const FString& InName, FSimpleMulticastDelegate* InSharedDrawEvent, ImFontAtlas* InFontAtlas)
	: Name(InName)
	, SharedDrawEvent(InSharedDrawEvent)
	, IniFilename(TCHAR_TO_ANSI(*GetIniFile(InName)))
{
	// Create context.
	Context = TUniquePtr<ImGuiContext>(ImGui::CreateContext(InFontAtlas));

	// Set this context in ImGui for initialization (any allocations will be tracked in this context).
	SetAsCurrent();

	// Start initialization.
	ImGuiIO& IO = ImGui::GetIO();

	// Set session data storage.
	IO.IniFilename = IniFilename.c_str();

	// Use pre-defined canvas size.
	IO.DisplaySize = { DEFAULT_CANVAS_WIDTH, DEFAULT_CANVAS_HEIGHT };
	DisplaySize = ImGuiInterops::ToVector2D(IO.DisplaySize);

	// Initialize key mapping, so context can correctly interpret input state.
	ImGuiInterops::SetUnrealKeyMap(IO);

	// Begin frame to complete context initialization (this is to avoid problems with other systems calling to ImGui
	// during startup).
	BeginFrame();
}

FImGuiContextProxy::~FImGuiContextProxy()
{
	if (Context)
	{
		// Setting this as a current context is still required in the current framework version to properly shutdown
		// and save data.
		SetAsCurrent();

		// Save context data and destroy.
		ImGui::DestroyContext(Context.Release());
	}
}

void FImGuiContextProxy::Draw()
{
	if (bIsFrameStarted && !bIsDrawCalled)
	{
		bIsDrawCalled = true;

		SetAsCurrent();

		const bool bSharedFirst = (CVars::DebugDrawOnWorldTick.GetValueOnGameThread() > 0);

		// Broadcast draw event to allow listeners to draw their controls to this context.
		if (bSharedFirst && SharedDrawEvent && SharedDrawEvent->IsBound())
		{
			SharedDrawEvent->Broadcast();
		}
		if (DrawEvent.IsBound())
		{
			DrawEvent.Broadcast();
		}
		if (!bSharedFirst && SharedDrawEvent && SharedDrawEvent->IsBound())
		{
			SharedDrawEvent->Broadcast();
		}
	}
}

void FImGuiContextProxy::Tick(float DeltaSeconds)
{
	// Making sure that we tick only once per frame.
	if (LastFrameNumber < GFrameNumber)
	{
		LastFrameNumber = GFrameNumber;

		SetAsCurrent();

		if (bIsFrameStarted)
		{
			// Make sure that draw events are called before the end of the frame.
			Draw();

			// Ending frame will produce render output that we capture and store for later use. This also puts context to
			// state in which it does not allow to draw controls, so we want to immediately start a new frame.
			EndFrame();
		}

		// Update context information (some data, like mouse cursor, may be cleaned in new frame, so we should collect it
		// beforehand).
		bHasActiveItem = ImGui::IsAnyItemActive();
		MouseCursor = ImGuiInterops::ToSlateMouseCursor(ImGui::GetMouseCursor());
		DisplaySize = ImGuiInterops::ToVector2D(ImGui::GetIO().DisplaySize);

		// Begin a new frame and set the context back to a state in which it allows to draw controls.
		BeginFrame(DeltaSeconds);
	}
}

void FImGuiContextProxy::BeginFrame(float DeltaTime)
{
	if (!bIsFrameStarted)
	{
		ImGuiIO& IO = ImGui::GetIO();
		IO.DeltaTime = DeltaTime;

		if (InputState)
		{
			ImGuiInterops::CopyInput(IO, *InputState);
		}

		ImGui::NewFrame();

		bIsFrameStarted = true;
		bIsDrawCalled = false;
	}
}

void FImGuiContextProxy::EndFrame()
{
	if (bIsFrameStarted)
	{
		// Prepare draw data (after this call we cannot draw to this context until we start a new frame).
		ImGui::Render();

		// Update our draw data, so we can use them later during Slate rendering while ImGui is in the middle of the
		// next frame.
		UpdateDrawData(ImGui::GetDrawData());

		bIsFrameStarted = false;
	}
}

void FImGuiContextProxy::UpdateDrawData(ImDrawData* DrawData)
{
	if (DrawData && DrawData->CmdListsCount > 0)
	{
		DrawLists.SetNum(DrawData->CmdListsCount, false);

		for (int Index = 0; Index < DrawData->CmdListsCount; Index++)
		{
			DrawLists[Index].TransferDrawData(*DrawData->CmdLists[Index]);
		}
	}
	else
	{
		// If we are not rendering then this might be a good moment to empty the array.
		DrawLists.Empty();
	}
}
