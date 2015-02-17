// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "SlateBasics.h"
#include "Slate/SlateGameResources.h"
#include "SUTMenuBase.h"

#if !UE_SERVER
class SUTInGameMenu : public SUTMenuBase
{
protected:

	virtual void BuildLeftMenuBar();
	virtual void BuildExitMenu(TSharedPtr <SComboButton> ExitButton, TSharedPtr<SVerticalBox> MenuSpace);

	virtual FReply OnCloseMenu(TSharedPtr<SComboButton> MenuButton);
	virtual FReply OnReturnToLobby(TSharedPtr<SComboButton> MenuButton);
	virtual FReply OnReturnToMainMenu(TSharedPtr<SComboButton> MenuButton);

	virtual FReply OnTeamChangeClick();
	virtual FReply OnSpectateClick();
	virtual void SetInitialPanel();


};
#endif
