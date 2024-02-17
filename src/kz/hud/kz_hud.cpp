#include "kz_hud.h"
#include "sdk/datatypes.h"
#include "utils/utils.h"

#include "tier0/memdbgon.h"

#include "checkpoint/kz_checkpoint.h"

internal void AddSpeedText(KZPlayer *player, char *buffer, int size)
{
	char speed[128];
	speed[0] = 0;
	Vector velocity;
	player->GetVelocity(&velocity);
	// Keep the takeoff velocity on for a while after landing so the speed values flicker less.
	if ((player->GetPawn()->m_fFlags & FL_ONGROUND && g_pKZUtils->GetServerGlobals()->curtime - player->landingTime > 0.07)
		|| (player->GetPawn()->m_MoveType == MOVETYPE_LADDER && !player->IsButtonPressed(IN_JUMP)))
	{
		snprintf(speed, sizeof(speed), "Speed: %.0f", velocity.Length2D());
	}
	else
	{
		snprintf(speed, sizeof(speed), "Speed: %.0f (%.0f)", velocity.Length2D(), player->takeoffVelocity.Length2D());
	}
	V_strncat(buffer, speed, size);
}

internal void AddKeyText(KZPlayer *player, char *buffer, int size)
{
	char keys[128];
	keys[0] = 0;
	snprintf(keys, sizeof(keys), "Keys: %s %s %s %s %s %s",
		player->IsButtonPressed(IN_MOVELEFT) ? "A" : "_",
		player->IsButtonPressed(IN_FORWARD) ? "W" : "_",
		player->IsButtonPressed(IN_BACK) ? "S" : "_",
		player->IsButtonPressed(IN_MOVERIGHT) ? "D" : "_",
		player->IsButtonPressed(IN_DUCK) ? "C" : "_",
		player->IsButtonPressed(IN_JUMP) ? "J" : "_");
	V_strncat(buffer, keys, size);
}

internal void AddTeleText(KZPlayer *player, char *buffer, int size)
{
	char Tele[128];
	Tele[0] = 0;
	snprintf(Tele, sizeof(Tele), "CP: %i/%i TPs: %i",
		player->checkpointService->GetCurrentCpIndex(),
		player->checkpointService->GetCheckPointsCount(),
		player->checkpointService->GetTeleportCount());
	V_strncat(buffer, Tele, size);
}

void KZHUDService::DrawSpeedPanel()
{
	if(!player->IsShowPanel())
		return;

	char buffer[1024];
	buffer[0] = 0;
	if (player->timerIsRunning)
	{
		i32 ticks = player->tickCount - player->timerStartTick;
		utils::FormatTimerText(ticks, buffer, sizeof(buffer));
		utils::PrintCentre(this->player->GetController(), "%s", buffer);
		buffer[0] = 0;
	}

	AddSpeedText(player, buffer, sizeof(buffer));
	strcat(buffer, "\n");
	AddKeyText(player, buffer, sizeof(buffer));
	strcat(buffer, "\n");
	AddTeleText(player, buffer, sizeof(buffer));

	utils::PrintAlert(this->player->GetController(), "%s", buffer);
}
