// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UnrealTemplate.h"
#include "UTAnalyticParams.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUTAnalytics, Display, All);

class IAnalyticsProvider;

class FUTAnalytics : FNoncopyable
{
public:
	static UNREALTOURNAMENT_API IAnalyticsProvider& GetProvider();

	static UNREALTOURNAMENT_API TSharedPtr<IAnalyticsProvider> GetProviderPtr();
	
	/** Helper function to determine if the provider is valid. */
	static UNREALTOURNAMENT_API bool IsAvailable() 
	{ 
		return Analytics.IsValid(); 
	}
	
	/** Called to initialize the singleton. */
	static void Initialize();
	
	/** Called to shut down the singleton */
	static void Shutdown();

	/** 
	 * Called when the login status has changed. Checks IsAvailable() internally, so external code doesn't need to.
	 * @param NewAccountID The new AccountID of the user, or empty if the user logged out.
	 */
	static void LoginStatusChanged(FString NewAccountID);

/** Analytics events*/
public:
	/* Server metrics */
	static void FireEvent_ServerUnplayableCondition(AUTGameMode* UTGM, double HitchThresholdInMs, int32 NumHitchesAboveThreshold, double TotalUnplayableTimeInMs);
	static void FireEvent_PlayerContextLocationPerMinute(AUTPlayerController* UTPC, FString& PlayerContextLocation, const int32 NumSocialPartyMembers);


	//Param name generalizer
	static FString GetGenericParamName(EGenericAnalyticParam::Type InGenericParam);

	static void SetMatchInitialParameters(AUTGameMode* UTGM, TArray<FAnalyticsEventAttribute>& ParamArray, bool bNeedMatchTime);
	static void SetServerInitialParameters(TArray<FAnalyticsEventAttribute>& ParamArray);

private:
	/** Initialize the FString Array of Analytic Parameters */
	static void InitializeAnalyticParameterNames();

/** Analytics Helper Functions */
public:
	static FString GetPlatform();
	static int32 GetMatchTime(AUTPlayerController* UTPC);
	static int32 GetMatchTime(AUTGameMode* UTGM);
	static FString GetMapName(AUTPlayerController* UTPC);
	static FString GetMapName(AUTGameMode* UTGM);

private:
	enum class EAccountSource
	{
		EFromRegistry,
		EFromOnlineSubsystem,
	};

	/** Private helper for setting the UserID. Assumes the instance is valid. Not to be used by external code. */
	static void PrivateSetUserID(const FString& AccountID, EAccountSource AccountSource);

	static bool bIsInitialized;
	static TSharedPtr<IAnalyticsProvider> Analytics;
	static FString CurrentAccountID;
	static EAccountSource CurrentAccountSource;

	static TArray<FString> GenericParamNames;
};