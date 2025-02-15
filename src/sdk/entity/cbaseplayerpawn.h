#pragma once

class CCSPlayer_WaterServices;
class CPlayer_MovementServices;
class CPlayer_ObserverServices;
class CCSPlayer_ItemServices;

#include "cbasemodelentity.h"

class CBasePlayerPawn : public CBaseModelEntity
{
public:
	DECLARE_SCHEMA_CLASS(CBasePlayerPawn);

	SCHEMA_FIELD(CPlayer_MovementServices *, m_pMovementServices)
	SCHEMA_FIELD(CHandle<CBasePlayerController>, m_hController)
	SCHEMA_FIELD(CCSPlayer_ItemServices *, m_pItemServices)
	SCHEMA_FIELD(CPlayer_ObserverServices *, m_pObserverServices)
	SCHEMA_FIELD(CCSPlayer_WaterServices *, m_pWaterServices)

	void CommitSuicide(bool bExplode, bool bForce)
	{
		this->m_bTakesDamage(true);
		CALL_VIRTUAL(void, g_pGameConfig->GetOffset("CommitSuicide"), this, bExplode, bForce);
		this->m_bTakesDamage(false);
	}

	bool IsBot()
	{
		return !!(this->m_fFlags() & FL_PAWN_FAKECLIENT);
	}
};
