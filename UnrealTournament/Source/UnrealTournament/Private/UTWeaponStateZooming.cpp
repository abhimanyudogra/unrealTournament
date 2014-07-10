// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#include "UnrealTournament.h"
#include "UTWeaponStateZooming.h"

void FZoomTickFunction::ExecuteTick(float DeltaTime, enum ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	if (ZoomState != NULL && !ZoomState->HasAnyFlags(RF_PendingKill | RF_Unreachable) && ZoomState->GetUTOwner() != NULL && ZoomState->GetUTOwner()->GetWeapon() == ZoomState->GetOuterAUTWeapon())
	{
		ZoomState->TickZoom(DeltaTime);
	}
}
FString FZoomTickFunction::DiagnosticMessage()
{
	return *FString::Printf(TEXT("%s::ZoomTick()"), *GetPathNameSafe(ZoomState));
}

UUTWeaponStateZooming::UUTWeaponStateZooming(const FPostConstructInitializeProperties& PCIP)
: Super(PCIP)
{
	MinFOV = 12.f;
	ZoomTime = 1.0f;
}

void UUTWeaponStateZooming::PendingFireStarted()
{
	if (bIsZoomed)
	{
		bIsZoomed = false;
		if (GetUTOwner()->IsLocallyControlled())
		{
			APlayerCameraManager* Camera = GetUTOwner()->GetPlayerCameraManager();
			if (Camera != NULL)
			{
				Camera->UnlockFOV();
			}
		}
	}
	else
	{
		bIsZoomed = true;
		StartZoomTime = GetWorld()->TimeSeconds;
		ZoomTickHandler.ZoomState = this;
		ZoomTickHandler.RegisterTickFunction(GetOuterAUTWeapon()->GetLevel());
	}
}
void UUTWeaponStateZooming::PendingFireStopped()
{
	ZoomTickHandler.UnRegisterTickFunction();
}

void UUTWeaponStateZooming::BeginFiringSequence(uint8 FireModeNum)
{
	// this isn't actually firing so immediately switch to other fire mode
	if (FireModeNum != GetOuterAUTWeapon()->GetCurrentFireMode() && GetOuterAUTWeapon()->FiringState.IsValidIndex(FireModeNum) && GetOuterAUTWeapon()->HasAmmo(FireModeNum))
	{
		GetOuterAUTWeapon()->GotoFireMode(FireModeNum);
	}
}
void UUTWeaponStateZooming::EndFiringSequence(uint8 FireModeNum)
{
	GetOuterAUTWeapon()->GotoActiveState();
}

void UUTWeaponStateZooming::WeaponBecameInactive()
{
	if (bIsZoomed && GetUTOwner()->IsLocallyControlled())
	{
		bIsZoomed = false;
		APlayerCameraManager* Camera = GetUTOwner()->GetPlayerCameraManager();
		if (Camera != NULL)
		{
			Camera->UnlockFOV();
		}
	}
}

bool UUTWeaponStateZooming::DrawHUD(UUTHUDWidget* WeaponHudWidget)
{
	if (bIsZoomed && OverlayMat != NULL)
	{
		UCanvas* C = WeaponHudWidget->GetCanvas();
		if (OverlayMI == NULL)
		{
			OverlayMI = UMaterialInstanceDynamic::Create(OverlayMat, this);
		}
		AUTPlayerState* PS = Cast<AUTPlayerState>(GetUTOwner()->PlayerState);
		if (PS != NULL && PS->Team != NULL)
		{
			static FName NAME_TeamColor(TEXT("TeamColor"));
			OverlayMI->SetVectorParameterValue(NAME_TeamColor, PS->Team->TeamColor);
		}
		FCanvasTileItem Item(FVector2D(0.0f, 0.0f), OverlayMI->GetRenderProxy(false), FVector2D(C->ClipX, C->ClipY));
		// expand X axis size to be widest supported aspect ratio (16:9)
		float OrigSizeX = Item.Size.X;
		Item.Size.X = FMath::Max<float>(Item.Size.X, Item.Size.Y * 16.0f / 9.0f);
		Item.Position.X -= (Item.Size.X - OrigSizeX) * 0.5f;
		Item.UV0 = FVector2D(0.0f, 0.0f);
		Item.UV1 = FVector2D(1.0f, 1.0f);
		C->DrawItem(Item);

		// temp until there's a decent crosshair in the material
		return true;
	}
	else
	{
		return true;
	}
}

void UUTWeaponStateZooming::TickZoom(float DeltaTime)
{
	// only mess with the FOV on the client; it doesn't matter to networking and is easier to maintain
	if (GetUTOwner()->IsLocallyControlled())
	{
		APlayerCameraManager* Camera = GetUTOwner()->GetPlayerCameraManager();
		if (Camera != NULL)
		{
			Camera->SetFOV(Camera->DefaultFOV - (Camera->DefaultFOV - MinFOV) * FMath::Min<float>((GetWorld()->TimeSeconds - StartZoomTime) / ZoomTime, 1.0f));
		}
	}
}