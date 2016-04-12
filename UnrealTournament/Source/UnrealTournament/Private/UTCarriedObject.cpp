// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.
#include "UnrealTournament.h"
#include "UTCarriedObject.h"
#include "UTProjectileMovementComponent.h"
#include "UTCTFScoring.h"
#include "Net/UnrealNetwork.h"
#include "UTPainVolume.h"
#include "UTLift.h"
#include "UTFlagReturnTrail.h"
#include "UTGhostFlag.h"
#include "UTSecurityCameraComponent.h"

AUTCarriedObject::AUTCarriedObject(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
	Collision = ObjectInitializer.CreateDefaultSubobject<UCapsuleComponent>(this, TEXT("Capsule"));
	Collision->InitCapsuleSize(72.0f, 30.0f);
	Collision->OnComponentBeginOverlap.AddDynamic(this, &AUTCarriedObject::OnOverlapBegin);
	Collision->SetCollisionProfileName(FName(TEXT("Pickup")));
	RootComponent = Collision;

	MovementComponent = ObjectInitializer.CreateDefaultSubobject<UUTProjectileMovementComponent>(this, TEXT("MovementComp"));
	MovementComponent->HitZStopSimulatingThreshold = 0.7f;
	MovementComponent->MaxSpeed = 5000.0f; // needed for gravity
	MovementComponent->InitialSpeed = 360.0f;
	MovementComponent->SetIsReplicated(true);
	MovementComponent->OnProjectileStop.AddDynamic(this, &AUTCarriedObject::OnStop);

	SetReplicates(true);
	bReplicateMovement = true;
	NetPriority = 3.0;
	LastGameMessageTime = 0.f;
	AutoReturnTime = 30.0f;
	bGradualAutoReturn = false;
	bMovementEnabled = true;
	LastTeleportedTime = -1000.f;
	bEnemyCanPickup = true;
	bInitialized = false;
	WeightSpeedPctModifier = 1.0f;
	bDisplayHolderTrail = false;
	MinGradualReturnDist = 1400.f;
	bSendHomeOnScore = true;
}

void AUTCarriedObject::Destroyed()
{
	Super::Destroyed();
	if (MyGhostFlag)
	{
		MyGhostFlag->Destroy();
		MyGhostFlag = nullptr;
	}
}

UUTSecurityCameraComponent* AUTCarriedObject::GetDetectingCamera()
{
	return DetectingCamera;
}

void AUTCarriedObject::GetActorEyesViewPoint(FVector& OutLocation, FRotator& OutRotation) const
{
	OutLocation = (GetWorld()->GetTimeSeconds() < LastTeleportedTime + 5.f) ? LastTeleportedLoc : GetActorLocation();
	OutRotation = GetActorRotation();
}

void AUTCarriedObject::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// backwards compatibility; force values on existing instances
	Collision->SetAbsolute(false, false, true);
	if (Role == ROLE_Authority)
	{
		Collision->SetWorldRotation(FRotator(0.0f, 0.f, 0.f));
	}
}

void AUTCarriedObject::Init(AUTGameObjective* NewBase)
{
	// Look up the team for my CarriedObject
	uint8 DesiredTeamNum = NewBase->GetTeamNum();
	if (DesiredTeamNum < 255)
	{
		AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
		if (GameState != NULL && DesiredTeamNum < GameState->Teams.Num())
		{
			Team = GameState->Teams[DesiredTeamNum];
		}
	}

	bInitialized = true;
	HomeBase = NewBase;
	ObjectState = CarriedObjectState::Home;
	HomeBase->ObjectStateWasChanged(ObjectState);
	MoveToHome();
	OnHolderChanged();
}

void AUTCarriedObject::OnStop(const FHitResult& Hit)
{
	if (Hit.Actor.IsValid() && Hit.Component.IsValid() && Cast<AUTLift>(Hit.Actor.Get()))
	{
		AttachRootComponentTo(Hit.Component.Get(), NAME_None, EAttachLocation::KeepWorldPosition);
	}
}

bool AUTCarriedObject::IsHome()
{
	return ObjectState == CarriedObjectState::Home;
}

void AUTCarriedObject::GetLifetimeReplicatedProps(TArray< FLifetimeProperty > & OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AUTCarriedObject, ObjectState);
	DOREPLIFETIME(AUTCarriedObject, Holder);
	DOREPLIFETIME(AUTCarriedObject, Team);
	DOREPLIFETIME(AUTCarriedObject, bMovementEnabled);
	DOREPLIFETIME_CONDITION(AUTCarriedObject, HomeBase, COND_InitialOnly);
	DOREPLIFETIME(AUTCarriedObject, FlagReturnTime);
	DOREPLIFETIME(AUTCarriedObject, bEnemyCanPickup);
	DOREPLIFETIME(AUTCarriedObject, bFriendlyCanPickup);
	DOREPLIFETIME(AUTCarriedObject, bCurrentlyPinged);
	DOREPLIFETIME(AUTCarriedObject, bDisplayHolderTrail);
	DOREPLIFETIME(AUTCarriedObject, bGradualAutoReturn);
	DOREPLIFETIME(AUTCarriedObject, AutoReturnTime);
}

void AUTCarriedObject::AttachTo(USkeletalMeshComponent* AttachToMesh)
{
	if (AttachToMesh != NULL && Collision != NULL)
	{
		Collision->SetRelativeLocation(Holder3PTransform);
		Collision->SetRelativeRotation(Holder3PRotation);
		AttachRootComponentTo(AttachToMesh, Holder3PSocketName);
		ClientUpdateAttachment(true);
	}
}

void AUTCarriedObject::DetachFrom(USkeletalMeshComponent* AttachToMesh)
{
	if (AttachToMesh != NULL && Collision != NULL)
	{
		Collision->SetRelativeLocation(FVector(0,0,0));
		Collision->SetRelativeRotation(FRotator(0,0,0));
		Collision->SetRelativeScale3D(FVector(1.0f,1.0f,1.0f));
		DetachRootComponentFromParent(true);
		ClientUpdateAttachment(false);
	}
}

void AUTCarriedObject::ClientUpdateAttachment(bool bNowAttached)
{
	if (bNowAttached)
	{
		if (bDisplayHolderTrail && (GetNetMode() != NM_DedicatedServer) && RootComponent && RootComponent->AttachParent)
		{
			HolderTrail = NewObject<UParticleSystemComponent>(this);
			if (HolderTrail)
			{
				HolderTrail->bAutoActivate = true;
				HolderTrail->bAutoDestroy = false;
				HolderTrail->SecondsBeforeInactive = 0.0f;
				HolderTrail->SetTemplate(HolderTrailEffect);
				HolderTrail->RegisterComponent();
				HolderTrail->AttachTo(RootComponent->AttachParent, Holder3PSocketName, EAttachLocation::SnapToTarget);
				if (Team)
				{
					HolderTrail->SetColorParameter(FName(TEXT("Color")), Team->TeamColor);
				}
			}
		}
	}
	else
	{
		if (HolderTrail)
		{
			HolderTrail->DeactivateSystem();
			HolderTrail = nullptr;
		}
	}
}

void AUTCarriedObject::OnOverlapBegin(AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (bInitialized && !bIsDropping)
	{
		AUTCharacter* Character = Cast<AUTCharacter>(OtherActor);
		if (Character != NULL)
		{
			if (!GetWorld()->LineTraceTestByChannel(OtherActor->GetActorLocation(), GetActorLocation(), ECC_Pawn, FCollisionQueryParams(), WorldResponseParams))
			{
				TryPickup(Character);
			}
			else
			{
				// it may be possible for the character to get into a valid spot without leaving our collision so keep checking until they're gone
				GetWorldTimerManager().SetTimer(CheckTouchingHandle, this, &AUTCarriedObject::CheckTouching, 0.1f, false);
			}
		}
	}
}

void AUTCarriedObject::TryPickup_Implementation(AUTCharacter* Character)
{
	if (Role == ROLE_Authority && Character->Controller != NULL && ObjectState != CarriedObjectState::Held)
	{
		// A valid character wants to pick this up.  First ask if they can.  

		if ( CanBePickedUpBy(Character) && Character->CanPickupObject(this) )
		{
			SetHolder(Character);
		}
	}
}

void AUTCarriedObject::OnObjectStateChanged()
{
	if (Role == ROLE_Authority)
	{
		// update movement component based on state; note that clients do this via OnRep_Moving() so as not to get physics out of sync if there is an inaccuracy in the location of blocking objects
		if (ObjectState == CarriedObjectState::Held)
		{
			// Turn off movement on this actor
			MovementComponent->StopMovementImmediately();
			MovementComponent->SetUpdatedComponent(NULL);
		}
		else
		{
			MovementComponent->SetUpdatedComponent(Collision);
		}
	}

	OnCarriedObjectStateChangedDelegate.Broadcast(this, ObjectState);
}

void AUTCarriedObject::OnRep_Moving()
{
	if (bMovementEnabled)
	{
		MovementComponent->SetUpdatedComponent(Collision);
	}
	else
	{
		MovementComponent->StopMovementImmediately();
		MovementComponent->SetUpdatedComponent(NULL);
	}
}

void AUTCarriedObject::OnHolderChanged()
{
	if (Holder != NULL)
	{
		Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	
	}
	else
	{
		Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}

	OnCarriedObjectHolderChangedDelegate.Broadcast(this);
}

uint8 AUTCarriedObject::GetTeamNum() const
{
	return (Team != NULL) ? Team->GetTeamNum() : 255;
}

void AUTCarriedObject::ChangeState(FName NewCarriedObjectState)
{
	if (Role == ROLE_Authority)
	{
		ObjectState = NewCarriedObjectState;
		OnObjectStateChanged();
		HomeBase->ObjectStateWasChanged(ObjectState);
	}
}

bool AUTCarriedObject::CanBePickedUpBy(AUTCharacter* Character)
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS != NULL && (!GS->IsMatchInProgress() || GS->IsMatchIntermission()))
	{
		return false;
	}
	else if (Character->IsRagdoll())
	{
		// check again later in case they get up
		GetWorldTimerManager().SetTimer(CheckTouchingHandle, this, &AUTCarriedObject::CheckTouching, 0.1f, false);
		return false;
	}
	else if (bAnyoneCanPickup)
	{
		return true;
	}
	// If this is the NewHolder's objective and bTeamPickupSendsHome is set, then send this home.
	else if (GetTeamNum() == Character->GetTeamNum())
	{
		if (bFriendlyCanPickup)
		{
			return true;
		}
		else if (bTeamPickupSendsHome)
		{
			if (ObjectState == CarriedObjectState::Dropped)
			{
				SendGameMessage(0, Character->PlayerState, NULL);
				Score(FName("SentHome"), Character, Cast<AUTPlayerState>(Character->PlayerState));
			}
			return false;
		}
		else
		{
			AUTPlayerController* PC = Character ? Cast < AUTPlayerController>(Character->GetController()) : NULL;
			if (PC)
			{
				PC->ClientReceiveLocalizedMessage(MessageClass, 13);
			}
			return false;
		}
	}
	else
	{
		if (bEnemyPickupSendsHome)
		{
			if (ObjectState == CarriedObjectState::Dropped)
			{
				SendGameMessage(0, Character->PlayerState, NULL);
				Score(FName("SentHome"), Character, Cast<AUTPlayerState>(Character->PlayerState));
			}
			return false;
		}
		else if (!bEnemyCanPickup)
		{
			AUTPlayerController* PC = Character ? Cast < AUTPlayerController>(Character->GetController()) : NULL;
			if (PC)
			{
				PC->ClientReceiveLocalizedMessage(MessageClass, 13);
			}
			return false;
		}
		return bEnemyCanPickup;
	}
}

void AUTCarriedObject::PickupDenied(AUTCharacter* Character)
{
	// By default do nothing.
}

void AUTCarriedObject::SendGameMessage(uint32 Switch, APlayerState* PS1, APlayerState* PS2, UObject* OptionalObject)
{
	LastGameMessageTime = GetWorld()->GetTimeSeconds();
	AUTGameMode* GM = GetWorld()->GetAuthGameMode<AUTGameMode>();
	if (GM != NULL && MessageClass != NULL)
	{
		GM->BroadcastLocalized(this, MessageClass, Switch, PS1, PS2, Team);
	}
}

void AUTCarriedObject::SetHolder(AUTCharacter* NewHolder)
{
	// Sanity Checks
	if (NewHolder == NULL || NewHolder->IsPendingKillPending() || Cast<AUTPlayerState>(NewHolder->PlayerState) == NULL)
	{
		return;
	}

	UE_LOG(UT, Warning, TEXT("Remove any ghost flag"));
	ClearGhostFlag();
	bool bWasHome = (ObjectState == CarriedObjectState::Home);
	ChangeState(CarriedObjectState::Held);

	// Tell the base it's been picked up
	HomeBase->ObjectWasPickedUp(NewHolder, bWasHome);

	HoldingPawn = NewHolder;
	AttachTo(HoldingPawn->GetMesh());
	Holder = Cast<AUTPlayerState>(HoldingPawn->PlayerState);
	PickedUpTime = GetWorld()->GetTimeSeconds();

	// If we have authority, immediately notify of the holder change, otherwise replication will handle it
	if (Role == ROLE_Authority)
	{
		OnHolderChanged();
		if (Holder && bWasHome)
		{
			LastPingedTime = GetWorld()->GetTimeSeconds() + 2.5f;
			LastPositionUpdateTime = GetWorld()->GetTimeSeconds();
			Holder->ModifyStatsValue(NAME_FlagGrabs, 1);
			if (Holder->Team)
			{
				Holder->Team->ModifyStatsValue(NAME_TeamFlagGrabs, 1);
			}
		}
	}

	// Track the pawns that have held this flag - used for scoring
	int32 AssistIndex = FindAssist(Holder);
	if (AssistIndex < 0)
	{
		FAssistTracker NewAssist;
		NewAssist.Holder = Holder;
		NewAssist.TotalHeldTime = 0.0f;
		AssistIndex = AssistTracking.Add(NewAssist);
	}

	// Remove any spawn protection the pawn might have
	if (HoldingPawn->bSpawnProtectionEligible)
	{
		HoldingPawn->DeactivateSpawnProtection();
	}

	Holder->SetCarriedObject(this);
	Holder->bSpecialTeamPlayer = true;
	UUTGameplayStatics::UTPlaySound(GetWorld(), PickupSound, HoldingPawn);

	SendGameMessage(4, Holder, NULL);

	if (Role == ROLE_Authority)
	{
		if (bWasHome && MessageClass != NULL)
		{
			// the "X flag taken" announcement from home implies the new holder's current location, so update enemy bots
			for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
			{
				AUTBot* B = Cast<AUTBot>(It->Get());
				if (B != NULL && !B->IsTeammate(HoldingPawn))
				{
					B->UpdateEnemyInfo(HoldingPawn, EUT_HeardExact);
				}
			}
		}
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS != NULL)
		{
			for (AUTTeamInfo* TeamIter : GS->Teams)
			{
				TeamIter->NotifyObjectiveEvent(HomeBase, NewHolder->Controller, FName(TEXT("FlagStatusChange")));
			}
		}
	}
}

void AUTCarriedObject::NoLongerHeld(AController* InstigatedBy)
{
	UE_LOG(UT, Warning, TEXT("NO LONGER HELD"));
	// Have the holding pawn drop the object
	if (HoldingPawn != NULL)
	{
		DetachFrom(HoldingPawn->GetMesh());
	}
	LastHolder = Holder;
	if (Holder != NULL)
	{
		Holder->ClearCarriedObject(this);
	}

	LastHoldingPawn = HoldingPawn;
	int32 AssistIndex = FindAssist(Holder);
	float LastHeldTime = GetWorld()->GetTimeSeconds() - PickedUpTime;
	if (AssistIndex >= 0 && AssistIndex < AssistTracking.Num())
	{
		AssistTracking[AssistIndex].TotalHeldTime += LastHeldTime;
	}

	HoldingPawn = NULL;
	Holder = NULL;

	// If we have authority, immediately notify of the holder change, otherwise replication will handle it
	if (Role == ROLE_Authority)
	{
		OnHolderChanged();

		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS != NULL)
		{
			for (AUTTeamInfo* TeamIter : GS->Teams)
			{
				TeamIter->NotifyObjectiveEvent(HomeBase, InstigatedBy, FName(TEXT("FlagStatusChange")));
			}
		}
	}

	AUTPlayerState* LastHoldingPS = LastHoldingPawn ? Cast<AUTPlayerState>(LastHoldingPawn->PlayerState) : nullptr;
	if (LastHoldingPS)
	{
		LastHoldingPS->bSpecialTeamPlayer = false;
	}
}

void AUTCarriedObject::TossObject(AUTCharacter* ObjectHolder)
{
	UE_LOG(UT, Warning, TEXT("TOSS OBJECT"));
	// Throw the object.
	if (ObjectHolder != NULL)
	{
		// prevent touches while dropping... we'll check them again when we're done
		TGuardValue<bool> DropGuard(bIsDropping, true);

		FVector Extra = FVector::ZeroVector;
		if (ObjectHolder->Health > 0)
		{
			Extra = ObjectHolder->GetActorRotation().Vector() * 900.0f;
		}
		FVector Loc = ObjectHolder->GetActorLocation();
		Loc.Z -= ObjectHolder->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
		Loc.Z += Collision->GetUnscaledCapsuleHalfHeight() * 1.1f;
		const FRotator DesiredRot = Extra.IsZero() ? FRotator::ZeroRotator : Extra.Rotation();
		bool bSuccess = false;
		const float AdjustDist = ObjectHolder->GetSimpleCollisionRadius() * 0.5f;
		const FVector TestLocs[] = { Loc, Loc + FVector(AdjustDist, 0.0f, 0.0f), Loc - FVector(AdjustDist, 0.0f, 0.0f), Loc + FVector(0.0f, AdjustDist, 0.0f), Loc - FVector(0.0f, AdjustDist, 0.0f) };
		for (int32 i = 0; i < ARRAY_COUNT(TestLocs); i++)
		{
			FVector AdjustedLoc = TestLocs[i];
			// make sure adjusted teleport location won't push flag through wall
			if ( GetWorld()->FindTeleportSpot(this, AdjustedLoc, DesiredRot) && !GetWorld()->LineTraceTestByChannel(ObjectHolder->GetActorLocation(), AdjustedLoc, ECC_Pawn, FCollisionQueryParams::DefaultQueryParam, WorldResponseParams) &&
				TeleportTo(TestLocs[i], DesiredRot) )
			{
				bSuccess = true;
				break;
			}
		}
		if (bSuccess)
		{
			MovementComponent->Velocity = (0.5f * ObjectHolder->GetMovementComponent()->Velocity) + Extra + (218.0f * (0.5f + FMath::FRand()) * FMath::VRand() + FVector(0.0f, 0.0f, 100.0f));
		}
		else
		{
			// put the flag exactly in the desired location without encroachment checks, then disable movement so it stays exactly there
			// since the prior holder is there, it should be still possible to touch it to acquire/return, but movement might cause the penetration resolution to put it on the wrong side of walls, etc
			TeleportTo(Loc, DesiredRot, false, true);
			MovementComponent->StopMovementImmediately();
			MovementComponent->SetUpdatedComponent(NULL);
		}
	}
	if (ObjectState == CarriedObjectState::Dropped)
	{
		// make sure no other players are touching the flag and should cause it to change state immediately
		Collision->UpdateOverlaps();
		CheckTouching();
	}
}

void AUTCarriedObject::CheckTouching()
{
	if (ObjectState != CarriedObjectState::Held)
	{
		const FName PrevState = ObjectState;
		TArray<AActor*> Touching;
		GetOverlappingActors(Touching, APawn::StaticClass());
		for (AActor* Touched : Touching)
		{
			if (Touched != LastHoldingPawn)
			{
				OnOverlapBegin(Touched, Cast<UPrimitiveComponent>(Touched->GetRootComponent()), INDEX_NONE, false, FHitResult(this, Collision, GetActorLocation(), FVector(0.0f, 0.0f, 1.0f)));
				if (ObjectState != PrevState)
				{
					break;
				}
			}
		}
	}
}

bool AUTCarriedObject::TeleportTo(const FVector& DestLocation, const FRotator& DestRotation, bool bIsATest, bool bNoCheck)
{
	if (bNoCheck)
	{
		return Super::TeleportTo(DestLocation, DestRotation, bIsATest, bNoCheck);
	}

	// during teleportation, we need to change our collision to overlap potential telefrag targets instead of block
	// however, EncroachingBlockingGeometry() doesn't handle reflexivity correctly so we can't get anywhere changing our collision responses
	// instead, we must change our object type to adjust the query
	FVector TeleportStart = GetActorLocation();
	ECollisionChannel SavedObjectType = Collision->GetCollisionObjectType();
	Collision->SetCollisionObjectType(COLLISION_TELEPORTING_OBJECT);
	bool bResult = Super::TeleportTo(DestLocation, DestRotation, bIsATest, bNoCheck);
	Collision->SetCollisionObjectType(SavedObjectType);
	Collision->UpdateOverlaps(); // make sure collision object type changes didn't mess with our overlaps
	return bResult;
}

void AUTCarriedObject::Drop(AController* Killer)
{
	UE_LOG(UT, Warning, TEXT("DROP"));
	UUTGameplayStatics::UTPlaySound(GetWorld(), DropSound, (HoldingPawn != NULL) ? (AActor*)HoldingPawn : (AActor*)this);

	SendGameMessage(3, Holder, NULL);
	{
		// NoLongerHeld() results in collision being enabled, but we will check for new touches in TossObject()
		TGuardValue<bool> DropGuard(bIsDropping, true);
		NoLongerHeld(Killer);
	}

	// Toss is out
	TossObject(LastHoldingPawn);

	if (HomeBase != NULL)
	{
		HomeBase->ObjectWasDropped(LastHoldingPawn);
	}
	ChangeState(CarriedObjectState::Dropped);
	if (bGradualAutoReturn && (PastPositions.Num() > 0))
	{
		if ((GetActorLocation() - PastPositions[PastPositions.Num() - 1]).Size() < MinGradualReturnDist)
		{
			PastPositions.RemoveAt(PastPositions.Num() - 1);
		}
		if (PastPositions.Num() > 0)
		{
			PutGhostFlagAt(PastPositions[PastPositions.Num() - 1]);
			PastPositions.RemoveAt(PastPositions.Num() - 1);
		}
	}
}

bool AUTCarriedObject::SetDetectingCamera(class UUTSecurityCameraComponent* NewDetectingCamera)
{
	if (DetectingCamera && NewDetectingCamera && ((GetActorLocation() - DetectingCamera->K2_GetComponentLocation()).SizeSquared() < (GetActorLocation() - NewDetectingCamera->K2_GetComponentLocation()).SizeSquared()))
	{
		// change only if new one is closer
		return false;
	}
	DetectingCamera = NewDetectingCamera;
	return true;
}

void AUTCarriedObject::Use()
{
	// Use should be managed in child classes.
}

void AUTCarriedObject::SendHomeWithNotify()
{
	SendHome();
}

void AUTCarriedObject::ClearGhostFlag()
{
	UE_LOG(UT, Warning, TEXT("%s Clear ghost flag %s"), *GetName(), MyGhostFlag ? *MyGhostFlag->GetName() :TEXT("NONE"));
	if (MyGhostFlag != nullptr)
	{
		MyGhostFlag->MyCarriedObject = nullptr;
		MyGhostFlag->Destroy();
		MyGhostFlag = nullptr;
	}
}

void AUTCarriedObject::PutGhostFlagAt(const FVector NewGhostLocation)
{
	if (GhostFlagClass)
	{
		if ((MyGhostFlag == nullptr) || MyGhostFlag->IsPendingKillPending())
		{
			FActorSpawnParameters Params;
			Params.Owner = this;
			MyGhostFlag = GetWorld()->SpawnActor<AUTGhostFlag>(GhostFlagClass, NewGhostLocation, GetActorRotation(), Params);
			if (MyGhostFlag)
			{
				MyGhostFlag->SetCarriedObject(this);
			}
			UE_LOG(UT, Warning, TEXT("%s ADD ghost flag %s "), *GetName(), MyGhostFlag ? *MyGhostFlag->GetName() : TEXT("NONE"));
		}
		else
		{
			MyGhostFlag->MoveTo(NewGhostLocation);
			UE_LOG(UT, Warning, TEXT("%s MOVE ghost flag %s "), *GetName(), MyGhostFlag ? *MyGhostFlag->GetName() : TEXT("NONE"));
		}
	}
}

void AUTCarriedObject::SendHome()
{
	LastTeleportedTime = GetWorld()->GetTimeSeconds();
	LastTeleportedLoc = GetActorLocation();

	DetachRootComponentFromParent(true);
	if (ObjectState == CarriedObjectState::Home) return;	// Don't both if we are already home
	UE_LOG(UT, Warning, TEXT("SEND HOME gradual %d"), bGradualAutoReturn);

	NoLongerHeld();
	if (bGradualAutoReturn && (PastPositions.Num() > 0) && (Role == ROLE_Authority))
	{
		if ((GetActorLocation() - PastPositions[PastPositions.Num() - 1]).Size() < MinGradualReturnDist)
		{
			PastPositions.RemoveAt(PastPositions.Num() - 1);
		}
		if (PastPositions.Num() > 0)
		{
			bool bWantsGhostFlag = false;
			DetachRootComponentFromParent(true);
			MovementComponent->Velocity = FVector(0.0f, 0.0f, 0.0f);
			Collision->SetRelativeRotation(FRotator(0, 0, 0));
			SetActorLocationAndRotation(PastPositions[PastPositions.Num() - 1], GetActorRotation());
			PastPositions.RemoveAt(PastPositions.Num() - 1);
			if (PastPositions.Num() > 0)
			{
				if ((GetActorLocation() - PastPositions[PastPositions.Num() - 1]).Size() < MinGradualReturnDist)
				{
					PastPositions.RemoveAt(PastPositions.Num() - 1);
				}
				if (PastPositions.Num() > 0)
				{
					PutGhostFlagAt(PastPositions[PastPositions.Num() - 1]);
					bWantsGhostFlag = true;
				}
			}
			OnObjectStateChanged();
			ForceNetUpdate();
			if (!bWantsGhostFlag)
			{
				ClearGhostFlag();
			}
			return;
		}
	}
	ClearGhostFlag();
	ChangeState(CarriedObjectState::Home);
	HomeBase->ObjectReturnedHome(LastHoldingPawn);
	MoveToHome();
}

FVector AUTCarriedObject::GetHomeLocation() const
{
	if (HomeBase == NULL)
	{
		UE_LOG(UT, Warning, TEXT("Carried object querying home location with no home"), *GetName());
		return GetActorLocation();
	}
	else
	{
		return HomeBase->GetActorLocation() + HomeBase->GetActorRotation().RotateVector(HomeBaseOffset) + FVector(0.f, 0.f, Collision->GetScaledCapsuleHalfHeight());
	}
}
FRotator AUTCarriedObject::GetHomeRotation() const
{
	if (HomeBase == NULL)
	{
		UE_LOG(UT, Warning, TEXT("Carried object querying home rotation with no home"), *GetName());
		return GetActorRotation();
	}
	else
	{
		return HomeBase->GetActorRotation() + HomeBaseRotOffset;
	}
}

void AUTCarriedObject::MoveToHome()
{
	DetachRootComponentFromParent(true);
	AssistTracking.Empty();
	HolderRescuers.Empty();
	PastPositions.Empty();
	if (HomeBase != NULL)
	{
		MovementComponent->Velocity = FVector(0.0f,0.0f,0.0f);
		Collision->SetRelativeRotation(FRotator(0, 0, 0));
		SetActorLocationAndRotation(GetHomeLocation(), GetHomeRotation());
		ForceNetUpdate();
	}
}

void AUTCarriedObject::Score_Implementation(FName Reason, AUTCharacter* ScoringPawn, AUTPlayerState* ScoringPS)
{
	if (Role == ROLE_Authority)
	{
		LastGameMessageTime = GetWorld()->GetTimeSeconds();
		AUTGameMode* Game = GetWorld()->GetAuthGameMode<AUTGameMode>();
		if (Game != NULL)
		{
			Game->ScoreObject(this, ScoringPawn, ScoringPS, Reason);
		}
		if (bSendHomeOnScore)
		{
			SendHome();
		}
	}
}

void AUTCarriedObject::SetTeam(AUTTeamInfo* NewTeam)
{
	Team = NewTeam;
	ForceNetUpdate();
}

void AUTCarriedObject::FellOutOfWorld(const UDamageType& dmgType)
{
	if (Role == ROLE_Authority)
	{
		SendHomeWithNotify();
	}
	else
	{
		// Force it back to the last replicated location
		SetActorLocation(ReplicatedMovement.Location);
	}
}

void AUTCarriedObject::EnteredPainVolume(AUTPainVolume* PainVolume)
{
	if (Holder)
	{
		return;
	}
	if (Role == ROLE_Authority)
	{
		SendHomeWithNotify();
	}
	else
	{
		// Force it back to the last replicated location
		SetActorLocation(ReplicatedMovement.Location);
	}
}


// workaround for bug in AActor implementation
void AUTCarriedObject::OnRep_AttachmentReplication()
{
	if (AttachmentReplication.AttachParent)
	{
		if (RootComponent)
		{
			USceneComponent* ParentComponent = AttachmentReplication.AttachParent->GetRootComponent();

			if (AttachmentReplication.AttachComponent != NULL)
			{
				ParentComponent = AttachmentReplication.AttachComponent;
			}

			if (ParentComponent)
			{
				// Calculate scale before attachment as ComponentToWorld will be modified after AttachTo()
				FVector NewRelativeScale3D = RootComponent->RelativeScale3D;
				if (!RootComponent->bAbsoluteScale)
				{
					FTransform ParentToWorld = ParentComponent->GetSocketTransform(AttachmentReplication.AttachSocket);
					FTransform RelativeTM = RootComponent->ComponentToWorld.GetRelativeTransform(ParentToWorld);
					NewRelativeScale3D = RelativeTM.GetScale3D();
				}

				RootComponent->AttachTo(ParentComponent, AttachmentReplication.AttachSocket);
				RootComponent->RelativeLocation = AttachmentReplication.LocationOffset;
				RootComponent->RelativeRotation = AttachmentReplication.RotationOffset;
				RootComponent->RelativeScale3D = NewRelativeScale3D;

				RootComponent->UpdateComponentToWorld();
				ClientUpdateAttachment(Cast<APawn>(AttachmentReplication.AttachParent) != nullptr);
			}
		}
	}
	else
	{
		DetachRootComponentFromParent();
		OnRep_ReplicatedMovement(); // make sure this was applied, since it might be ignored if it arrived in a seperate packet to AttachmentReplication
		ClientUpdateAttachment(false);
	}
}

// HACK: workaround for engine bug with transform replication when attaching/detaching things
void AUTCarriedObject::OnRep_ReplicatedMovement()
{
	// ignore the redundant ReplicatedMovement we replicated below
	if (AttachmentReplication.AttachParent == NULL)
	{
		Super::OnRep_ReplicatedMovement();
		if (ObjectState == CarriedObjectState::Home && HomeBase != NULL)
		{
			const FVector BaseLocation = GetHomeLocation();
			MovementComponent->Velocity = FVector(0.0f, 0.0f, 0.0f);
			if ((BaseLocation - GetActorLocation()).SizeSquared() > 1.f)
			{
				SetActorLocationAndRotation(BaseLocation, HomeBase->GetActorRotation());
			}
		}
	}
}
void AUTCarriedObject::GatherCurrentMovement()
{
	Super::GatherCurrentMovement();
	// force ReplicatedMovement to be replicated even when attached, which is a hack to force the last replicated data to be different when the flag is eventually returned
	// otherwise an untouched flag capture results in replication fail because this value has never changed in between times it was replicated (only attachment was)
	// leaving the flag floating at the enemy flag base on clients
	if (RootComponent != NULL && RootComponent->AttachParent != NULL)
	{
		ReplicatedMovement.Location = RootComponent->GetComponentLocation();
		ReplicatedMovement.Rotation = RootComponent->GetComponentRotation();
		ReplicatedMovement.bRepPhysics = false;
	}
}

float AUTCarriedObject::GetHeldTime(AUTPlayerState* TestHolder)
{
	float CurrentHeldTime = (Holder && (TestHolder == Holder)) ? GetWorld()->GetTimeSeconds() - PickedUpTime : 0.f;
	int32 AssistIndex = FindAssist(TestHolder);
	if (AssistIndex >= 0 && AssistIndex < AssistTracking.Num())
	{
		return CurrentHeldTime + AssistTracking[AssistIndex].TotalHeldTime;
	}
	return CurrentHeldTime;
}
