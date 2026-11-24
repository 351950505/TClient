/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "hud.h"

#include "binds.h"
#include "camera.h"
#include "controls.h"
#include "voting.h"

#include <base/color.h>
#include <base/time.h>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/tclient/gradient.h>
#include <game/client/components/tclient/modern_ui.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/layers.h>
#include <game/localization.h>

#include <algorithm>
#include <cmath>
#include <vector>

CHud::CHud()
{
	m_FPSTextContainerIndex.Reset();
	m_DDRaceEffectsTextContainerIndex.Reset();
	m_PlayerAngleTextContainerIndex.Reset();
	m_PlayerPrevAngle = -INFINITY;

	for(int i = 0; i < 2; i++)
	{
		m_aPlayerSpeedTextContainers[i].Reset();
		m_aPlayerPrevSpeed[i] = -INFINITY;
		m_aPlayerPositionContainers[i].Reset();
		m_aPlayerPrevPosition[i] = -INFINITY;
	}
}

void CHud::ResetHudContainers()
{
	m_GameTimerText.Reset(TextRender());
	m_PauseNotificationText.Reset(TextRender());
	m_SuddenDeathText.Reset(TextRender());
	for(auto &ScoreInfo : m_aScoreInfo)
	{
		TextRender()->DeleteTextContainer(ScoreInfo.m_OptionalNameTextContainerIndex);
		TextRender()->DeleteTextContainer(ScoreInfo.m_TextRankContainerIndex);
		TextRender()->DeleteTextContainer(ScoreInfo.m_TextScoreContainerIndex);
		Graphics()->DeleteQuadContainer(ScoreInfo.m_RoundRectQuadContainerIndex);

		ScoreInfo.Reset();
	}

	TextRender()->DeleteTextContainer(m_FPSTextContainerIndex);
	TextRender()->DeleteTextContainer(m_DDRaceEffectsTextContainerIndex);
	TextRender()->DeleteTextContainer(m_PlayerAngleTextContainerIndex);
	m_PlayerPrevAngle = -INFINITY;
	for(int i = 0; i < 2; i++)
	{
		TextRender()->DeleteTextContainer(m_aPlayerSpeedTextContainers[i]);
		m_aPlayerPrevSpeed[i] = -INFINITY;
		TextRender()->DeleteTextContainer(m_aPlayerPositionContainers[i]);
	}
}

void CHud::OnWindowResize()
{
	ResetHudContainers();
}

void CHud::OnReset()
{
	m_FrozenHudPending = false;
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		m_aMapProgressState[Dummy].Reset();
		m_aMapProgressValid[Dummy] = false;
		m_aMapProgressDisplayed[Dummy] = 0.0f;
	}
	m_TimeCpDiff = 0.0f;
	m_DDRaceTime = 0;
	m_FinishTimeLastReceivedTick = 0;
	m_TimeCpLastReceivedTick = 0;
	m_ShowFinishTime = false;
	m_aPlayerRecord[0] = -1.0f;
	m_aPlayerRecord[1] = -1.0f;
	m_aPlayerSpeed[0] = 0;
	m_aPlayerSpeed[1] = 0;
	m_aLastPlayerSpeedChange[0] = ESpeedChange::NONE;
	m_aLastPlayerSpeedChange[1] = ESpeedChange::NONE;
	m_LastSpectatorCountTick = 0;

	ResetHudContainers();
}

void CHud::OnInit()
{
	OnReset();

	Graphics()->SetColor(1.0, 1.0, 1.0, 1.0);

	m_HudQuadContainerIndex = Graphics()->CreateQuadContainer(false);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	PrepareAmmoHealthAndArmorQuads();

	// all cursors for the different weapons
	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		float ScaleX, ScaleY;
		Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[i].m_pSpriteCursor, ScaleX, ScaleY);
		m_aCursorOffset[i] = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 64.f * ScaleX, 64.f * ScaleY);
	}

	// the flags
	m_FlagOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 8.f, 16.f);

	PreparePlayerStateQuads();

	Graphics()->QuadContainerUpload(m_HudQuadContainerIndex);
}

void CHud::RenderGameTimer()
{
	float Half = m_Width / 2.0f;

	if(!(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_SUDDENDEATH))
	{
		char aBuf[32];
		int Time = 0;
		if(GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit && (GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer <= 0))
		{
			Time = GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit * 60 - ((Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / Client()->GameTickSpeed());

			if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER)
				Time = 0;
		}
		else if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME)
		{
			// The Warmup timer is negative in this case to make sure that incompatible clients will not see a warmup timer
			Time = (Client()->GameTick(g_Config.m_ClDummy) + GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer) / Client()->GameTickSpeed();
		}
		else
		{
			Time = (Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / Client()->GameTickSpeed();
		}

		str_time((int64_t)Time * 100, ETimeFormat::DAYS, aBuf, sizeof(aBuf));
		float FontSize = 10.0f;
		static float s_TextWidthM = TextRender()->TextWidth(FontSize, "00:00", -1, -1.0f);
		static float s_TextWidthH = TextRender()->TextWidth(FontSize, "00:00:00", -1, -1.0f);
		static float s_TextWidth0D = TextRender()->TextWidth(FontSize, "0d 00:00:00", -1, -1.0f);
		static float s_TextWidth00D = TextRender()->TextWidth(FontSize, "00d 00:00:00", -1, -1.0f);
		static float s_TextWidth000D = TextRender()->TextWidth(FontSize, "000d 00:00:00", -1, -1.0f);
		float w = Time >= 3600 * 24 * 100 ? s_TextWidth000D : (Time >= 3600 * 24 * 10 ? s_TextWidth00D : (Time >= 3600 * 24 ? s_TextWidth0D : (Time >= 3600 ? s_TextWidthH : s_TextWidthM)));
		// last 60 sec red, last 10 sec blink
		if(GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit && Time <= 60 && (GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer <= 0))
		{
			float Alpha = Time <= 10 && (2 * time() / time_freq()) % 2 ? 0.5f : 1.0f;
			TextRender()->TextColor(1.0f, 0.25f, 0.25f, Alpha);
		}
		m_GameTimerText.Update(TextRender(), aBuf, FontSize);
		m_GameTimerText.Render(TextRender(), vec2(Half - w / 2, 2), TextRender()->GetTextColor());
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
}

void CHud::RenderPauseNotification()
{
	if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED &&
		!(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
	{
		const char *pText = Localize("Game paused");
		float FontSize = 20.0f;
		m_PauseNotificationText.Update(TextRender(), pText, FontSize);
		float w = m_PauseNotificationText.Width();
		m_PauseNotificationText.Render(TextRender(), vec2(150.0f * Graphics()->ScreenAspect() + -w / 2.0f, 50.0f), TextRender()->GetTextColor());
	}
}

void CHud::RenderSuddenDeath()
{
	if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_SUDDENDEATH)
	{
		float Half = m_Width / 2.0f;
		const char *pText = Localize("Sudden Death");
		float FontSize = 12.0f;
		m_SuddenDeathText.Update(TextRender(), pText, FontSize);
		float w = m_SuddenDeathText.Width();
		m_SuddenDeathText.Render(TextRender(), vec2(Half - w / 2, 2), TextRender()->GetTextColor());
	}
}

void CHud::RenderScoreHud()
{
	// render small score hud
	if(!(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
	{
		float StartY = 229.0f; // the height of this display is 56, so EndY is 285

		const float ScoreSingleBoxHeight = 18.0f;

		const auto RenderGradientPlayerName = [&](int ClientId, const char *pName, vec2 Position, float FontSize) {
			if(ClientId < 0 || ClientId >= MAX_CLIENTS || !g_Config.m_TcGradientName || !CTClientGradient::AppliesTo(ClientId, GameClient()))
				return false;
			CTextCursor Cursor;
			Cursor.SetPosition(Position);
			Cursor.m_FontSize = FontSize;
			const float Phase = CTClientGradient::AnimatePhase(Client()->GlobalTime());
			Cursor.m_vColorSplits = CTClientGradient::BuildAnimatedTextSplits(pName, ClientId, GameClient(), Phase);
			TextRender()->TextEx(&Cursor, pName);
			return true;
		};

		bool ForceScoreInfoInit = !m_aScoreInfo[0].m_Initialized || !m_aScoreInfo[1].m_Initialized;
		m_aScoreInfo[0].m_Initialized = m_aScoreInfo[1].m_Initialized = true;

		if(GameClient()->IsTeamPlay() && GameClient()->m_Snap.m_pGameDataObj)
		{
			char aScoreTeam[2][16];
			str_format(aScoreTeam[TEAM_RED], sizeof(aScoreTeam[TEAM_RED]), "%d", GameClient()->m_Snap.m_pGameDataObj->m_TeamscoreRed);
			str_format(aScoreTeam[TEAM_BLUE], sizeof(aScoreTeam[TEAM_BLUE]), "%d", GameClient()->m_Snap.m_pGameDataObj->m_TeamscoreBlue);

			bool aRecreateTeamScore[2] = {str_comp(aScoreTeam[0], m_aScoreInfo[0].m_aScoreText) != 0, str_comp(aScoreTeam[1], m_aScoreInfo[1].m_aScoreText) != 0};

			const int aFlagCarrier[2] = {
				GameClient()->m_Snap.m_pGameDataObj->m_FlagCarrierRed,
				GameClient()->m_Snap.m_pGameDataObj->m_FlagCarrierBlue};

			bool RecreateRect = ForceScoreInfoInit;
			for(int t = 0; t < 2; t++)
			{
				if(aRecreateTeamScore[t])
				{
					m_aScoreInfo[t].m_ScoreTextWidth = TextRender()->TextWidth(14.0f, aScoreTeam[t == 0 ? TEAM_RED : TEAM_BLUE], -1, -1.0f);
					str_copy(m_aScoreInfo[t].m_aScoreText, aScoreTeam[t == 0 ? TEAM_RED : TEAM_BLUE]);
					RecreateRect = true;
				}
			}

			static float s_TextWidth100 = TextRender()->TextWidth(14.0f, "100", -1, -1.0f);
			float ScoreWidthMax = std::max({m_aScoreInfo[0].m_ScoreTextWidth, m_aScoreInfo[1].m_ScoreTextWidth, s_TextWidth100});
			float Split = 3.0f;
			float ImageSize = (GameClient()->m_Snap.m_pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS) ? 16.0f : Split;
			for(int t = 0; t < 2; t++)
			{
				// draw box
				if(RecreateRect)
				{
					Graphics()->DeleteQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex);

					if(t == 0)
						Graphics()->SetColor(0.975f, 0.17f, 0.17f, 0.3f);
					else
						Graphics()->SetColor(0.17f, 0.46f, 0.975f, 0.3f);
					m_aScoreInfo[t].m_RoundRectQuadContainerIndex = Graphics()->CreateRectQuadContainer(m_Width - ScoreWidthMax - ImageSize - 2 * Split, StartY + t * 20, ScoreWidthMax + ImageSize + 2 * Split, ScoreSingleBoxHeight, 5.0f, IGraphics::CORNER_L);
				}
				Graphics()->TextureClear();
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
				if(m_aScoreInfo[t].m_RoundRectQuadContainerIndex != -1)
					Graphics()->RenderQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex, -1);

				// draw score
				if(aRecreateTeamScore[t])
				{
					CTextCursor Cursor;
					Cursor.SetPosition(vec2(m_Width - ScoreWidthMax + (ScoreWidthMax - m_aScoreInfo[t].m_ScoreTextWidth) / 2 - Split, StartY + t * 20 + (18.f - 14.f) / 2.f));
					Cursor.m_FontSize = 14.0f;
					TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, &Cursor, aScoreTeam[t]);
				}
				if(m_aScoreInfo[t].m_TextScoreContainerIndex.Valid())
				{
					ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
					ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
					TextRender()->RenderTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, TColor, TOutlineColor);
				}

				if(GameClient()->m_Snap.m_pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS)
				{
					int BlinkTimer = (GameClient()->m_aFlagDropTick[t] != 0 &&
								 (Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_aFlagDropTick[t]) / Client()->GameTickSpeed() >= 25) ?
								 10 :
								 20;
					if(aFlagCarrier[t] == FLAG_ATSTAND || (aFlagCarrier[t] == FLAG_TAKEN && ((Client()->GameTick(g_Config.m_ClDummy) / BlinkTimer) & 1)))
					{
						// draw flag
						Graphics()->TextureSet(t == 0 ? GameClient()->m_GameSkin.m_SpriteFlagRed : GameClient()->m_GameSkin.m_SpriteFlagBlue);
						Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
						Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_FlagOffset, m_Width - ScoreWidthMax - ImageSize, StartY + 1.0f + t * 20);
					}
					else if(aFlagCarrier[t] >= 0)
					{
						// draw name of the flag holder
						int Id = aFlagCarrier[t] % MAX_CLIENTS;
						char aNameBuf[MAX_NAME_LENGTH];
						GameClient()->StreamerName(Id, aNameBuf, sizeof(aNameBuf));
						const char *pName = aNameBuf;
						const float NameWidth = TextRender()->TextWidth(8.0f, pName, -1, -1.0f);
						const vec2 NamePosition(std::min(m_Width - NameWidth - 1.0f, m_Width - ScoreWidthMax - ImageSize - 2 * Split), StartY + (t + 1) * 20.0f - 2.0f);
						// 审计修复（2026-11-20 P3）：渐变开/关切换时强制重建普通文本容器
						const bool NameGradient = RenderGradientPlayerName(Id, pName, NamePosition, 8.0f);
						const bool NameWasGradient = m_aScoreInfo[t].m_NameWasGradient;
						m_aScoreInfo[t].m_NameWasGradient = NameGradient;
						if(NameGradient)
						{
							str_copy(m_aScoreInfo[t].m_aPlayerNameText, pName);
						}
						else
						{
							if(str_comp(pName, m_aScoreInfo[t].m_aPlayerNameText) != 0 || RecreateRect || NameWasGradient)
							{
								str_copy(m_aScoreInfo[t].m_aPlayerNameText, pName);
								CTextCursor Cursor;
								Cursor.SetPosition(NamePosition);
								Cursor.m_FontSize = 8.0f;
								TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, &Cursor, pName);
							}
							if(m_aScoreInfo[t].m_OptionalNameTextContainerIndex.Valid())
							{
								const ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
								const ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
								TextRender()->RenderTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, TColor, TOutlineColor);
							}
						}

						// draw tee of the flag holder
						CTeeRenderInfo TeeInfo = GameClient()->m_aClients[Id].m_RenderInfo;
						TeeInfo.m_Size = ScoreSingleBoxHeight;

						const CAnimState *pIdleState = CAnimState::GetIdle();
						vec2 OffsetToMid;
						CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
						vec2 TeeRenderPos(m_Width - ScoreWidthMax - TeeInfo.m_Size / 2 - Split, StartY + (t * 20) + ScoreSingleBoxHeight / 2.0f + OffsetToMid.y);

						RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), TeeRenderPos);
					}
				}
				StartY += 8.0f;
			}
		}
		else
		{
			int Local = -1;
			int aPos[2] = {1, 2};
			const CNetObj_PlayerInfo *apPlayerInfo[2] = {nullptr, nullptr};
			int i = 0;
			for(int t = 0; t < 2 && i < MAX_CLIENTS && GameClient()->m_Snap.m_apInfoByScore[i]; ++i)
			{
				if(GameClient()->m_Snap.m_apInfoByScore[i]->m_Team != TEAM_SPECTATORS)
				{
					apPlayerInfo[t] = GameClient()->m_Snap.m_apInfoByScore[i];
					if(apPlayerInfo[t]->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
						Local = t;
					++t;
				}
			}
			// search local player info if not a spectator, nor within top2 scores
			if(Local == -1 && GameClient()->m_Snap.m_pLocalInfo && GameClient()->m_Snap.m_pLocalInfo->m_Team != TEAM_SPECTATORS)
			{
				for(; i < MAX_CLIENTS && GameClient()->m_Snap.m_apInfoByScore[i]; ++i)
				{
					if(GameClient()->m_Snap.m_apInfoByScore[i]->m_Team != TEAM_SPECTATORS)
						++aPos[1];
					if(GameClient()->m_Snap.m_apInfoByScore[i]->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
					{
						apPlayerInfo[1] = GameClient()->m_Snap.m_apInfoByScore[i];
						Local = 1;
						break;
					}
				}
			}
			char aScore[2][16];
			for(int t = 0; t < 2; ++t)
			{
				if(apPlayerInfo[t])
				{
					if(Client()->IsSixup() && GameClient()->m_Snap.m_pGameInfoObj->m_GameFlags & protocol7::GAMEFLAG_RACE)
					{
						str_time(absolute(static_cast<int64_t>(apPlayerInfo[t]->m_Score)) / 10, ETimeFormat::MINS_CENTISECS, aScore[t], sizeof(aScore[t]));
					}
					else if(GameClient()->m_GameInfo.m_TimeScore)
					{
						const CGameClient::CClientData &ClientData = GameClient()->m_aClients[apPlayerInfo[t]->m_ClientId];
						if(GameClient()->m_ReceivedDDNetPlayerFinishTimes && ClientData.m_FinishTimeSeconds != FinishTime::NOT_FINISHED_MILLIS)
						{
							const int64_t TimeMillis = static_cast<int64_t>(ClientData.m_FinishTimeSeconds) * 1000 + ClientData.m_FinishTimeMillis % 1000;
							str_time(TimeMillis / 10, ETimeFormat::HOURS, aScore[t], sizeof(aScore[t]));
						}
						else if(apPlayerInfo[t]->m_Score != FinishTime::NOT_FINISHED_TIMESCORE)
						{
							str_time(absolute(static_cast<int64_t>(apPlayerInfo[t]->m_Score)) * 100, ETimeFormat::HOURS, aScore[t], sizeof(aScore[t]));
						}
						else
						{
							aScore[t][0] = 0;
						}
					}
					else
					{
						str_format(aScore[t], sizeof(aScore[t]), "%d", apPlayerInfo[t]->m_Score);
					}
				}
				else
				{
					aScore[t][0] = 0;
				}
			}

			bool RecreateScores = str_comp(aScore[0], m_aScoreInfo[0].m_aScoreText) != 0 || str_comp(aScore[1], m_aScoreInfo[1].m_aScoreText) != 0 || m_LastLocalClientId != GameClient()->m_Snap.m_LocalClientId;
			m_LastLocalClientId = GameClient()->m_Snap.m_LocalClientId;

			bool RecreateRect = ForceScoreInfoInit;
			for(int t = 0; t < 2; t++)
			{
				if(RecreateScores)
				{
					m_aScoreInfo[t].m_ScoreTextWidth = TextRender()->TextWidth(14.0f, aScore[t], -1, -1.0f);
					str_copy(m_aScoreInfo[t].m_aScoreText, aScore[t]);
					RecreateRect = true;
				}

				if(apPlayerInfo[t])
				{
					int Id = apPlayerInfo[t]->m_ClientId;
					if(Id >= 0 && Id < MAX_CLIENTS)
					{
						char aNameBuf[MAX_NAME_LENGTH];
						GameClient()->StreamerName(Id, aNameBuf, sizeof(aNameBuf));
						const char *pName = aNameBuf;
						if(str_comp(pName, m_aScoreInfo[t].m_aPlayerNameText) != 0)
							RecreateRect = true;
					}
				}
				else
				{
					if(m_aScoreInfo[t].m_aPlayerNameText[0] != 0)
						RecreateRect = true;
				}

				char aBuf[16];
				str_format(aBuf, sizeof(aBuf), "%d.", aPos[t]);
				if(str_comp(aBuf, m_aScoreInfo[t].m_aRankText) != 0)
					RecreateRect = true;
			}

			static float s_TextWidth10 = TextRender()->TextWidth(14.0f, "10", -1, -1.0f);
			float ScoreWidthMax = std::max({m_aScoreInfo[0].m_ScoreTextWidth, m_aScoreInfo[1].m_ScoreTextWidth, s_TextWidth10});
			float Split = 3.0f, ImageSize = 16.0f, PosSize = 16.0f;

			for(int t = 0; t < 2; t++)
			{
				// draw box
				if(RecreateRect)
				{
					Graphics()->DeleteQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex);

					if(t == Local)
						Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.25f);
					else
						Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.25f);
					m_aScoreInfo[t].m_RoundRectQuadContainerIndex = Graphics()->CreateRectQuadContainer(m_Width - ScoreWidthMax - ImageSize - 2 * Split - PosSize, StartY + t * 20, ScoreWidthMax + ImageSize + 2 * Split + PosSize, ScoreSingleBoxHeight, 5.0f, IGraphics::CORNER_L);
				}
				Graphics()->TextureClear();
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
				if(m_aScoreInfo[t].m_RoundRectQuadContainerIndex != -1)
					Graphics()->RenderQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex, -1);

				if(RecreateScores)
				{
					CTextCursor Cursor;
					Cursor.SetPosition(vec2(m_Width - ScoreWidthMax + (ScoreWidthMax - m_aScoreInfo[t].m_ScoreTextWidth) - Split, StartY + t * 20 + (18.f - 14.f) / 2.f));
					Cursor.m_FontSize = 14.0f;
					TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, &Cursor, aScore[t]);
				}
				// draw score
				if(m_aScoreInfo[t].m_TextScoreContainerIndex.Valid())
				{
					ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
					ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
					TextRender()->RenderTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, TColor, TOutlineColor);
				}

				if(apPlayerInfo[t])
				{
					// draw name
					int Id = apPlayerInfo[t]->m_ClientId;
					if(Id >= 0 && Id < MAX_CLIENTS)
					{
						char aNameBuf[MAX_NAME_LENGTH];
						GameClient()->StreamerName(Id, aNameBuf, sizeof(aNameBuf));
						const char *pName = aNameBuf;
						const vec2 NamePosition(std::min(m_Width - TextRender()->TextWidth(8.0f, pName) - 1.0f, m_Width - ScoreWidthMax - ImageSize - 2 * Split - PosSize), StartY + (t + 1) * 20.0f - 2.0f);
						const bool NameGradient = RenderGradientPlayerName(Id, pName, NamePosition, 8.0f);
						const bool NameWasGradient = m_aScoreInfo[t].m_NameWasGradient;
						m_aScoreInfo[t].m_NameWasGradient = NameGradient;
						if(NameGradient)
						{
							str_copy(m_aScoreInfo[t].m_aPlayerNameText, pName);
						}
						else
						{
							if(RecreateRect || NameWasGradient)
							{
								str_copy(m_aScoreInfo[t].m_aPlayerNameText, pName);
								CTextCursor Cursor;
								Cursor.SetPosition(NamePosition);
								Cursor.m_FontSize = 8.0f;
								TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, &Cursor, pName);
							}
							if(m_aScoreInfo[t].m_OptionalNameTextContainerIndex.Valid())
							{
								const ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
								const ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
								TextRender()->RenderTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, TColor, TOutlineColor);
							}
						}

						// draw tee
						CTeeRenderInfo TeeInfo = GameClient()->m_aClients[Id].m_RenderInfo;
						TeeInfo.m_Size = ScoreSingleBoxHeight;

						const CAnimState *pIdleState = CAnimState::GetIdle();
						vec2 OffsetToMid;
						CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
						vec2 TeeRenderPos(m_Width - ScoreWidthMax - TeeInfo.m_Size / 2 - Split, StartY + (t * 20) + ScoreSingleBoxHeight / 2.0f + OffsetToMid.y);

						RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), TeeRenderPos);
					}
				}
				else
				{
					m_aScoreInfo[t].m_aPlayerNameText[0] = 0;
				}

				// draw position
				char aBuf[16];
				str_format(aBuf, sizeof(aBuf), "%d.", aPos[t]);
				if(RecreateRect)
				{
					str_copy(m_aScoreInfo[t].m_aRankText, aBuf);

					CTextCursor Cursor;
					Cursor.SetPosition(vec2(m_Width - ScoreWidthMax - ImageSize - Split - PosSize, StartY + t * 20 + (18.f - 10.f) / 2.f));
					Cursor.m_FontSize = 10.0f;
					TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_TextRankContainerIndex, &Cursor, aBuf);
				}
				if(m_aScoreInfo[t].m_TextRankContainerIndex.Valid())
				{
					ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
					ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
					TextRender()->RenderTextContainer(m_aScoreInfo[t].m_TextRankContainerIndex, TColor, TOutlineColor);
				}

				StartY += 8.0f;
			}
		}
	}
}

void CHud::RenderWarmupTimer()
{
	if(GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer <= 0 ||
		(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME) != 0)
	{
		return;
	}

	const float FontSize = 20.0f;
	const char *pTitle = Localize("Warmup");
	TextRender()->Text(150.0f * Graphics()->ScreenAspect() - TextRender()->TextWidth(FontSize, pTitle) / 2.0f, 50.0f, FontSize, pTitle);

	const int Seconds = GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer / Client()->GameTickSpeed();
	char aWarmupTime[16];
	float TextWidth;
	if(Seconds < 5)
	{
		str_format(aWarmupTime, sizeof(aWarmupTime), "%d.%d", Seconds, (GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer * 10 / Client()->GameTickSpeed()) % 10);
		TextWidth = TextRender()->TextWidth(FontSize, "0.0"); // Calculate width with fixed string to avoid slight changes when using aWarmupTime
	}
	else
	{
		str_format(aWarmupTime, sizeof(aWarmupTime), "%d", Seconds);
		TextWidth = TextRender()->TextWidth(FontSize, aWarmupTime);
	}
	TextRender()->Text(150.0f * Graphics()->ScreenAspect() - TextWidth / 2.0f, 75.0f, FontSize, aWarmupTime);
}

void CHud::RenderTextInfo()
{
	int Showfps = g_Config.m_ClShowfps;
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current())
		Showfps = 0;
#endif
	if(Showfps)
	{
		char aBuf[16];
		const int FramesPerSecond = round_to_int(1.0f / Client()->FrameTimeAverage());
		str_format(aBuf, sizeof(aBuf), "%d", FramesPerSecond);

		static float s_TextWidth0 = TextRender()->TextWidth(12.f, "0", -1, -1.0f);
		static float s_TextWidth00 = TextRender()->TextWidth(12.f, "00", -1, -1.0f);
		static float s_TextWidth000 = TextRender()->TextWidth(12.f, "000", -1, -1.0f);
		static float s_TextWidth0000 = TextRender()->TextWidth(12.f, "0000", -1, -1.0f);
		static float s_TextWidth00000 = TextRender()->TextWidth(12.f, "00000", -1, -1.0f);
		static const float s_aTextWidth[5] = {s_TextWidth0, s_TextWidth00, s_TextWidth000, s_TextWidth0000, s_TextWidth00000};

		int DigitIndex = GetDigitsIndex(FramesPerSecond, 4);

		CTextCursor Cursor;
		Cursor.SetPosition(vec2(m_Width - 10 - s_aTextWidth[DigitIndex], 5));
		Cursor.m_FontSize = 12.0f;
		auto OldFlags = TextRender()->GetRenderFlags();
		TextRender()->SetRenderFlags(OldFlags | TEXT_RENDER_FLAG_ONE_TIME_USE);
		if(m_FPSTextContainerIndex.Valid())
			TextRender()->RecreateTextContainerSoft(m_FPSTextContainerIndex, &Cursor, aBuf);
		else
			TextRender()->CreateTextContainer(m_FPSTextContainerIndex, &Cursor, "0");
		TextRender()->SetRenderFlags(OldFlags);
		if(m_FPSTextContainerIndex.Valid())
		{
			TextRender()->RenderTextContainer(m_FPSTextContainerIndex, TextRender()->DefaultTextColor(), TextRender()->DefaultTextOutlineColor());
		}
	}
	if(g_Config.m_ClShowpred && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "%d", Client()->GetPredictionTime());
		TextRender()->Text(m_Width - 10 - TextRender()->TextWidth(12, aBuf, -1, -1.0f), Showfps ? 20 : 5, 12, aBuf, -1.0f);
	}

	if(g_Config.m_TcMiniDebug)
	{
		float FontSize = 8.0f;
		float TextHeight = 11.0f;
		char aBuf[64];
		float OffsetY = 3.0f;

		int PlayerId = GameClient()->m_Snap.m_LocalClientId;
		if(GameClient()->m_Snap.m_SpecInfo.m_Active)
			PlayerId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;

		// [TClient] 审计修复（2026-11-20 P3）：自由视角/未入场时 PlayerId=-1，
		// 直接索引 m_aCharacters[-1]/m_aClients[-1] 会越界读相邻成员
		const bool ValidPlayerId = PlayerId >= 0 && PlayerId < MAX_CLIENTS;
		if(g_Config.m_ClShowhudDDRace && ValidPlayerId && GameClient()->m_Snap.m_aCharacters[PlayerId].m_HasExtendedData && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
			OffsetY += 50.0f;
		else if(g_Config.m_ClShowhudHealthAmmo && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
			OffsetY += 27.0f;

		vec2 Pos;
		if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW || !ValidPlayerId)
			Pos = vec2(GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy].x, GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy].y);
		else
			Pos = GameClient()->m_aClients[PlayerId].m_RenderPos;

		str_format(aBuf, sizeof(aBuf), "X: %.2f", Pos.x / 32.0f);
		TextRender()->Text(4, OffsetY, FontSize, aBuf, -1.0f);

		OffsetY += TextHeight;
		str_format(aBuf, sizeof(aBuf), "Y: %.2f", Pos.y / 32.0f);
		TextRender()->Text(4, OffsetY, FontSize, aBuf, -1.0f);
		if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW && ValidPlayerId)
		{
			OffsetY += TextHeight;
			str_format(aBuf, sizeof(aBuf), "Angle: %d", GameClient()->m_aClients[PlayerId].m_RenderCur.m_Angle);
			TextRender()->Text(4.0f, OffsetY, FontSize, aBuf, -1.0f);

			OffsetY += TextHeight;
			str_format(aBuf, sizeof(aBuf), "VelY: %.2f", GameClient()->m_Snap.m_aCharacters[PlayerId].m_Cur.m_VelY / 256.0f * 50.0f / 32.0f);
			TextRender()->Text(4.0f, OffsetY, FontSize, aBuf, -1.0f);

			OffsetY += TextHeight;

			str_format(aBuf, sizeof(aBuf), "VelX: %.2f", GameClient()->m_Snap.m_aCharacters[PlayerId].m_Cur.m_VelX / 256.0f * 50.0f / 32.0f);
			TextRender()->Text(4.0f, OffsetY, FontSize, aBuf, -1.0f);
		}
	}
	if(g_Config.m_TcRenderCursorSpec && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW)
	{
		int CurWeapon = 1;
		Graphics()->SetColor(1.f, 1.f, 1.f, g_Config.m_TcRenderCursorSpecAlpha / 100.0f);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponCursors[CurWeapon]);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_aCursorOffset[CurWeapon], m_Width / 2.0f, m_Height / 2.0f, 0.36f, 0.36f);
	}
	// render team in freeze text and last notify
	const bool ShowFrozenHud = g_Config.m_TcShowFrozenHud > 0;
	if((ShowFrozenHud || g_Config.m_TcNotifyWhenLast) && GameClient()->m_GameInfo.m_EntitiesDDRace)
	{
		STcFrozenHudFrame<MAX_CLIENTS> Frame;
		const int TeamClientId = TcFrozenHudTeamClientId(GameClient()->m_Snap.m_LocalClientId,
			GameClient()->m_Snap.m_SpecInfo.m_Active, GameClient()->m_Snap.m_SpecInfo.m_SpectatorId, MAX_CLIENTS);
		const int LocalTeamID = TeamClientId >= 0 ? GameClient()->m_Teams.Team(TeamClientId) : 0;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			// Preserve the existing PlayerInfo/team list, including spectators.
			if(!GameClient()->m_Snap.m_apPlayerInfos[i] || GameClient()->m_Teams.Team(i) != LocalTeamID)
				continue;

			const bool HasFreshCharacter = Client()->SnapFindItem(IClient::SNAP_CURRENT, NETOBJTYPE_CHARACTER, i) != nullptr;
			const auto &Character = GameClient()->m_Snap.m_aCharacters[i];
			Frame.AddPlayer(i, TcFrozenHudState(HasFreshCharacter, Character.m_HasExtendedData, Character.m_ExtendedData.m_FreezeEnd));
		}
		const auto &Counts = Frame.m_Counts;

		// Notify when last
		if(g_Config.m_TcNotifyWhenLast)
		{
			if(Counts.NotifyWhenLast())
			{
				TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcNotifyWhenLastColor)));
				float FontSize = g_Config.m_TcNotifyWhenLastSize;
				float XPos = std::clamp((g_Config.m_TcNotifyWhenLastX / 100.0f) * m_Width, 1.0f, m_Width - FontSize);
				float YPos = std::clamp((g_Config.m_TcNotifyWhenLastY / 100.0f) * m_Height, 1.0f, m_Height - FontSize);

				TextRender()->Text(XPos, YPos, FontSize, g_Config.m_TcNotifyWhenLastText, -1.0f);
				TextRender()->TextColor(TextRender()->DefaultTextColor());
			}
		}
		if(ShowFrozenHud)
		{
			m_FrozenHudPending = true;
			m_FrozenHudTeam = LocalTeamID;
			m_FrozenHudFrame = Frame;
			m_FrozenHudTextColor = TextRender()->GetTextColor();
			m_FrozenHudTextOutlineColor = TextRender()->GetTextOutlineColor();
			m_FrozenHudTextFlags = TextRender()->GetRenderFlags();
			if(!g_Config.m_TcIslandEnable)
				RenderFrozenHud({});
		}
	}
}

float CHud::FrozenHudRowY(const CUIRect &Row, const CUIRect &Island, float ScreenHeight)
{
	if(Row.w <= 0.0f || Row.h <= 0.0f || Island.w <= 0.0f || Island.h <= 0.0f ||
		Row.x >= Island.x + Island.w || Row.x + Row.w <= Island.x ||
		Row.y >= Island.y + Island.h || Row.y + Row.h <= Island.y)
		return Row.y;

	constexpr float Gap = 2.0f;
	const float Below = Island.y + Island.h + Gap;
	const float MaxY = std::max(0.0f, ScreenHeight - Row.h);
	if(Below <= MaxY)
		return std::max(0.0f, Below);
	// An island near the bottom may leave room only above it.
	const float Above = Island.y - Gap - Row.h;
	if(Above >= 0.0f)
		return std::min(Above, MaxY);
	// No vertical slot fits: keep the row on-screen rather than hiding it.
	return std::clamp(Below, 0.0f, MaxY);
}

CUIRect CHud::FrozenHudCapsulePlacement(CUIRect Capsule, const CUIRect &Island, float ScreenWidth, float ScreenHeight, float OffsetX, float OffsetY)
{
	if(Capsule.w <= 0.0f || Capsule.h <= 0.0f)
		return {};
	const bool HasIsland = Island.w > 0.0f && Island.h > 0.0f;
	if(HasIsland)
	{
		Capsule.x = Island.x + Island.w + 2.0f;
		Capsule.y = Island.y;
	}
	Capsule.x += OffsetX;
	Capsule.y += OffsetY;
	Capsule.x = std::clamp(Capsule.x, 8.0f, std::max(8.0f, ScreenWidth - 8.0f));
	if(HasIsland)
	{
		const float AvailableWidth = std::max(0.0f, ScreenWidth - 8.0f - Capsule.x);
		const float Fit = std::min(1.0f, AvailableWidth / Capsule.w);
		if(Fit <= 0.0f)
			return {};
		Capsule.w *= Fit;
		Capsule.h *= Fit;
	}
	else
		Capsule.x = std::clamp(Capsule.x, 8.0f, std::max(8.0f, ScreenWidth - 8.0f - Capsule.w));
	Capsule.y = std::clamp(Capsule.y, 0.0f, std::max(0.0f, ScreenHeight - 8.0f - Capsule.h));
	return Capsule;
}

namespace
{
	// 与灵动岛 EnsureShapeTexture 同款剪影判定：按掩码逐角匹配、圆心跟着角走
	// （W==R 的退化几何会让"Left&&Top 抢先"把另一端判成直角）。
	bool CapsuleShapeCovers(float Px, float Py, float W, float H, float R, int Corners)
	{
		if(Px < 0.0f || Py < 0.0f || Px > W || Py > H)
			return false;
		if(R <= 0.0f)
			return true;
		const bool Left = Px < R;
		const bool Right = Px > W - R;
		const bool Top = Py < R;
		const bool Bottom = Py > H - R;
		float CX, CY;
		if(Left && Top && (Corners & IGraphics::CORNER_TL))
		{
			CX = R;
			CY = R;
		}
		else if(Right && Top && (Corners & IGraphics::CORNER_TR))
		{
			CX = W - R;
			CY = R;
		}
		else if(Left && Bottom && (Corners & IGraphics::CORNER_BL))
		{
			CX = R;
			CY = H - R;
		}
		else if(Right && Bottom && (Corners & IGraphics::CORNER_BR))
		{
			CX = W - R;
			CY = H - R;
		}
		else
			return true; // 直角区/直边区：矩形范围内即覆盖
		const float DX = Px - CX;
		const float DY = Py - CY;
		return DX * DX + DY * DY <= R * R;
	}
}

// 白 RGB + 覆盖率 alpha 的圆角遮罩，2x 烘焙 1:1 显示（调用方传入已是 2 倍尺寸）。
// Sheen 变体把顶部高光的纵向渐变（alpha 0.05→0）烤进 alpha 通道，顶点色恒为白。
IGraphics::CTextureHandle CHud::EnsureCapsuleMask(int W, int H, float Rounding, bool Sheen)
{
	if(W <= 0 || H <= 0)
		return IGraphics::CTextureHandle();
	const float R = std::clamp(Rounding, 0.0f, (float)std::min(W, H));
	const SCapsuleMaskKey Key{W, H, (int)std::lround(R * 100.0f), Sheen};
	auto It = m_aCapsuleMaskCache.find(Key);
	if(It != m_aCapsuleMaskCache.end())
		return It->second.m_Tex;

	// 长会话防积累：冲掉重来，代价只是下一帧重烤一次。
	// 本 tick 内插入的条目不冲（见 hud.h SCapsuleMaskEntry 注释），其余照常
	// 卸载——跨 tick 的句柄其绘制早已提交，按既有行为失效是安全的。
	if(m_aCapsuleMaskCache.size() > 64)
	{
		const int CurTick = Client()->GameTick(g_Config.m_ClDummy);
		for(auto It = m_aCapsuleMaskCache.begin(); It != m_aCapsuleMaskCache.end();)
		{
			if(It->second.m_Tick != CurTick)
			{
				if(It->second.m_Tex.IsValid())
					Graphics()->UnloadTexture(&It->second.m_Tex);
				It = m_aCapsuleMaskCache.erase(It);
			}
			else
				++It;
		}
	}

	CImageInfo Image;
	Image.m_Width = (size_t)W;
	Image.m_Height = (size_t)H;
	Image.m_Format = CImageInfo::FORMAT_RGBA;
	Image.Allocate();
	if(!Image.m_pData)
		return IGraphics::CTextureHandle();

	// 边缘 8x8 超采样（灰度 64 级），内部/外部像素走快路径
	const int Samples = 8;
	const float Step = 1.0f / (float)Samples;
	for(int y = 0; y < H; y++)
	{
		for(int x = 0; x < W; x++)
		{
			uint8_t *p = Image.m_pData + ((size_t)y * (size_t)W + (size_t)x) * 4;
			const float Cx = (float)x + 0.5f;
			const float Cy = (float)y + 0.5f;
			const bool OuterIn = CapsuleShapeCovers(Cx - 1.0f, Cy - 1.0f, (float)W, (float)H, R, IGraphics::CORNER_ALL) &&
					     CapsuleShapeCovers(Cx + 1.0f, Cy - 1.0f, (float)W, (float)H, R, IGraphics::CORNER_ALL) &&
					     CapsuleShapeCovers(Cx - 1.0f, Cy + 1.0f, (float)W, (float)H, R, IGraphics::CORNER_ALL) &&
					     CapsuleShapeCovers(Cx + 1.0f, Cy + 1.0f, (float)W, (float)H, R, IGraphics::CORNER_ALL);
			if(OuterIn)
			{
				p[0] = p[1] = p[2] = 255;
				p[3] = Sheen ? (uint8_t)std::lround(0.05f * (1.0f - Cy / (float)H) * 255.0f) : 255;
				continue;
			}
			const bool AnyOuter = CapsuleShapeCovers(Cx - 0.5f, Cy - 0.5f, (float)W, (float)H, R, IGraphics::CORNER_ALL) ||
					      CapsuleShapeCovers(Cx + 0.5f, Cy - 0.5f, (float)W, (float)H, R, IGraphics::CORNER_ALL) ||
					      CapsuleShapeCovers(Cx - 0.5f, Cy + 0.5f, (float)W, (float)H, R, IGraphics::CORNER_ALL) ||
					      CapsuleShapeCovers(Cx + 0.5f, Cy + 0.5f, (float)W, (float)H, R, IGraphics::CORNER_ALL);
			if(!AnyOuter)
			{
				p[0] = p[1] = p[2] = p[3] = 0;
				continue;
			}
			int Hits = 0;
			for(int sy = 0; sy < Samples; sy++)
			{
				for(int sx = 0; sx < Samples; sx++)
				{
					const float Px = (float)x + ((float)sx + 0.5f) * Step;
					const float Py = (float)y + ((float)sy + 0.5f) * Step;
					if(CapsuleShapeCovers(Px, Py, (float)W, (float)H, R, IGraphics::CORNER_ALL))
						Hits++;
				}
			}
			p[0] = p[1] = p[2] = 255;
			p[3] = (uint8_t)std::lround((float)Hits / (float)(Samples * Samples) * (Sheen ? 0.05f * (1.0f - Cy / (float)H) : 1.0f) * 255.0f);
		}
	}

	const IGraphics::CTextureHandle Tex = Graphics()->LoadTextureRawMove(Image, 0, "capsule-shape");
	if(Tex.IsValid())
		m_aCapsuleMaskCache[Key] = {Tex, Client()->GameTick(g_Config.m_ClDummy)};
	return Tex;
}

void CHud::RenderFrozenHud(const CUIRect &Island)
{
	if(!m_FrozenHudPending)
		return;
	m_FrozenHudPending = false;
	if(!g_Config.m_TcShowFrozenHud)
		return;
	const int LocalTeamID = m_FrozenHudTeam;
	const auto &Counts = m_FrozenHudFrame.m_Counts;
	const int NumInTeam = Counts.m_NumInTeam;
	const bool AliveOnly = g_Config.m_TcFrozenTeesAliveOnly != 0;
	const int NumDrawn = AliveOnly ? m_FrozenHudFrame.m_Counts.m_KnownUnfrozen : NumInTeam;
	const bool ShowFrozenText = g_Config.m_TcShowFrozenText > 0;
	const bool ShowFrozenHud = g_Config.m_TcShowFrozenHud > 0;
	const CScreenRect OldScreen = Graphics()->GetScreen();
	const ColorRGBA OldTextColor = TextRender()->GetTextColor();
	const ColorRGBA OldTextOutlineColor = TextRender()->GetTextOutlineColor();
	const unsigned OldTextFlags = TextRender()->GetRenderFlags();
	Graphics()->MapScreenToSize(m_Width, m_Height);
	TextRender()->SetRenderFlags(m_FrozenHudTextFlags);
	TextRender()->TextColor(m_FrozenHudTextColor);
	TextRender()->TextOutlineColor(m_FrozenHudTextOutlineColor);

	const int NumShown = g_Config.m_TcShowFrozenText == 1 ? Counts.m_KnownUnfrozen : Counts.m_Frozen;
	const bool CapsuleShown = ShowFrozenHud && !GameClient()->m_Scoreboard.IsActive() && !(LocalTeamID == 0 && g_Config.m_TcFrozenHudTeamOnly);

	if(CapsuleShown && (NumDrawn > 0 || ShowFrozenText))
	{
		CTeeRenderInfo FreezeInfo;
		const CSkin *pSkin = GameClient()->m_Skins.Find("x_ninja");
		FreezeInfo.m_OriginalRenderSkin = pSkin->m_OriginalSkin;
		FreezeInfo.m_ColorableRenderSkin = pSkin->m_ColorableSkin;
		FreezeInfo.m_BloodColor = pSkin->m_BloodColor;
		FreezeInfo.m_SkinMetrics = pSkin->m_Metrics;
		FreezeInfo.m_ColorBody = ColorRGBA(1.0f, 1.0f, 1.0f);
		FreezeInfo.m_ColorFeet = ColorRGBA(1.0f, 1.0f, 1.0f);
		FreezeInfo.m_CustomColoredSkin = false;

		// Redesign: tc_frozen_tees_scale (60-200 %) scales the tee size, the
		// spacing, the capsule padding and the pixel offsets together, so every
		// part of the capsule grows as one unit.
		const float Scale = std::clamp(g_Config.m_TcFrozenTeesScale / 100.0f, 0.6f, 2.0f);
		const float OffsetX = g_Config.m_TcFrozenTeesOffsetX * Scale;
		const float OffsetY = g_Config.m_TcFrozenTeesOffsetY * Scale;
		const ColorRGBA AccentColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcFrozenTeesAccentColor));
		float TeeSize = g_Config.m_TcFrozenHudTeeSize * Scale;
		const float TeeGap = 3.0f * Scale;
		const float Pad = 2.0f * Scale;

		// The display limit applies to the whole capsule, not each row.
		const int MaxDisplayLimit = std::clamp(g_Config.m_TcFrozenTeesMaxDisplay, 1, 10);
		const int MaxDisplayed = std::min(NumDrawn, MaxDisplayLimit);
		const int MaxRows = std::clamp(g_Config.m_TcFrozenMaxRows, 1, 6);
		const int MaxTees = std::max(1, (MaxDisplayed + MaxRows - 1) / MaxRows);
		float StartPos = m_Width / 2.0f + 38.0f * (m_Width / m_Height) / 1.78f;

		int TotalRows = std::min(MaxRows, (MaxDisplayed + MaxTees - 1) / MaxTees);
		const int TeesInRow = std::min(MaxDisplayed, MaxTees);
		// Feet hang below the body: the old +3 px bottom margin, scaled.
		// [TClient] 审计修复：alive-only 且全队冻结时 NumDrawn==0（TeesInRow==0），胶囊只画计数
		const bool HasTees = TeesInRow > 0;
		const float ContentW = HasTees ? (TeesInRow * TeeSize + (TeesInRow - 1) * TeeGap) : 0.0f;
		const float ContentH = HasTees ? (TotalRows * TeeSize + (TotalRows - 1) * TeeGap + 3.0f * Scale) : TeeSize;

		// Count text merges into the capsule's left side; the "? K" part only
		// appears while unknown teammates are present and renders dimmer.
		char aCountBuf[32] = {};
		char aUnknownBuf[16] = {};
		str_format(aCountBuf, sizeof(aCountBuf), "%d / %d", NumShown, NumInTeam);
		if(Counts.m_Unknown > 0 && false) // "? N" hidden per user request (2026-11-16)
			str_format(aUnknownBuf, sizeof(aUnknownBuf), "  ? %d", Counts.m_Unknown);
		const float CountFontSize = TeeSize * 0.7f / std::max(1.0f, g_Config.m_TcFontSize / 100.0f);
		const float CountMainW = ShowFrozenText ? TextRender()->TextWidth(CountFontSize, aCountBuf, -1, -1.0f) : 0.0f;
		const float CountUnknownW = ShowFrozenText && Counts.m_Unknown > 0 ? TextRender()->TextWidth(CountFontSize, aUnknownBuf, -1, -1.0f) : 0.0f;
		const float CountW = CountMainW + CountUnknownW;

		// 计数胶囊宽度：没有 tee 时不再预留 tee 前的间隙
		const float CountTeeGap = ShowFrozenText && HasTees ? TeeGap : 0.0f;
		const float CapsuleW = 2.0f * Pad + (ShowFrozenText ? CountW + CountTeeGap : 0.0f) + ContentW;
		const float CapsuleH = 2.0f * Pad + ContentH;
		// The first tee anchor stays at StartPos; the capsule wraps text and tees.
		CUIRect Capsule = {StartPos - TeeSize / 2.0f - Pad - (ShowFrozenText ? CountW + CountTeeGap : 0.0f), 0.0f, CapsuleW, CapsuleH};
		const CUIRect PlacedCapsule = FrozenHudCapsulePlacement(Capsule, Island, m_Width, m_Height, OffsetX, OffsetY);
		const float CapsuleScale = PlacedCapsule.w / Capsule.w;
		if(CapsuleScale <= 0.0f)
		{
			TextRender()->TextColor(OldTextColor);
			TextRender()->TextOutlineColor(OldTextOutlineColor);
			TextRender()->SetRenderFlags(OldTextFlags);
			Graphics()->MapScreen(OldScreen);
			return;
		}
		// Fit the entire capsule uniformly without changing glyph metrics or
		// moving the anchor left into the island when its right side is narrow.
		Capsule.x = PlacedCapsule.x / CapsuleScale;
		Capsule.y = PlacedCapsule.y / CapsuleScale;
		Graphics()->MapScreenToSize(m_Width / CapsuleScale, m_Height / CapsuleScale);

		CUIRect TeeRow = {Capsule.x + Pad + (ShowFrozenText ? CountW + CountTeeGap : 0.0f), Capsule.y + Pad, ContentW, ContentH};

		// Capsule: translucent rounded plate with a subtle top sheen.
		// 预烘焙（学灵动岛 EnsureShapeTexture）：面板/高光各烤一张 alpha 遮罩纹理
		// 按 (宽,高,圆角) 缓存，每帧只是一次带色 blit —— 省掉 DrawRectExt 的
		// 72 次三角函数与 48 个自由三角形；烘焙 2x + 1:1 显示，边缘 AA 与岛一致。
		const float Rounding = std::min(Capsule.h / 2.0f, 6.0f * Scale);
		const int MaskW = std::max(1, (int)std::lround(Capsule.w));
		const int MaskH = std::max(1, (int)std::lround(Capsule.h));
		const IGraphics::CTextureHandle PlateTex = EnsureCapsuleMask(MaskW * 2, MaskH * 2, Rounding * 2.0f, false);
		const IGraphics::CTextureHandle SheenTex = EnsureCapsuleMask(MaskW * 2, MaskH * 2, Rounding * 2.0f, true);
		if(PlateTex.IsValid() && SheenTex.IsValid())
		{
			Graphics()->TextureSet(PlateTex);
			Graphics()->QuadsBegin();
			Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.30f);
			Graphics()->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
			const IGraphics::CQuadItem PlateQuad(Capsule.x, Capsule.y, Capsule.w, Capsule.h);
			Graphics()->QuadsDrawTL(&PlateQuad, 1);
			Graphics()->QuadsEnd();
			Graphics()->TextureSet(SheenTex);
			Graphics()->QuadsBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			Graphics()->QuadsDrawTL(&PlateQuad, 1);
			Graphics()->QuadsEnd();
			Graphics()->TextureClear();
		}
		else
		{
			// 烘焙失败（显存压力等）回退到逐帧细分路径
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.30f);
			Graphics()->DrawRectExt(Capsule.x, Capsule.y, Capsule.w, Capsule.h, Rounding, IGraphics::CORNER_ALL);
			Graphics()->DrawRectExt4(Capsule.x, Capsule.y, Capsule.w, Capsule.h,
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.05f), ColorRGBA(1.0f, 1.0f, 1.0f, 0.05f),
				ColorRGBA(1.0f, 1.0f, 1.0f, 0.0f), ColorRGBA(1.0f, 1.0f, 1.0f, 0.0f),
				Rounding, IGraphics::CORNER_ALL);
			Graphics()->QuadsEnd();
		}

		if(ShowFrozenText)
		{
			TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
			TextRender()->TextOutlineColor(ColorRGBA(0.0f, 0.0f, 0.0f, 0.9f));
			float TextHeight = 0.0f;
			STextSizeProperties TextSizeProps;
			TextSizeProps.m_pHeight = &TextHeight;
			TextRender()->TextWidth(CountFontSize, aCountBuf, -1, -1.0f, 0, TextSizeProps);
			const float TextY = Capsule.y + (Capsule.h - TextHeight) / 2.0f;
			const float CountX = Capsule.x + Pad;
			TextRender()->Text(CountX, TextY, CountFontSize, aCountBuf);
			if(Counts.m_Unknown > 0)
			{
				TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
				TextRender()->Text(CountX + CountMainW, TextY, CountFontSize, aUnknownBuf);
			}
			TextRender()->TextColor(m_FrozenHudTextColor);
			TextRender()->TextOutlineColor(m_FrozenHudTextOutlineColor);
		}

		bool Overflow = NumDrawn > MaxDisplayed;

		int NumDisplayed = 0;
		int NumInRow = 0;
		int CurrentRow = 0;
		float ProgressiveOffset = 0.0f;

		for(int OverflowIndex = 0; OverflowIndex < (Overflow ? 3 : 1); OverflowIndex++)
		{
			for(int i = 0; i < MAX_CLIENTS && NumDisplayed < MaxDisplayed; i++)
			{
				if(!m_FrozenHudFrame.m_aIncluded[i])
					continue;
				{
					const ETcFrozenHudState State = m_FrozenHudFrame.m_aStates[i];
					const bool Frozen = State == ETcFrozenHudState::FROZEN;
					const bool Unknown = State == ETcFrozenHudState::UNKNOWN;
					if(AliveOnly && State != ETcFrozenHudState::UNFROZEN)
						continue;
					if(Overflow && TcFrozenHudOverflowPass(State) != OverflowIndex)
						continue;

					CTeeRenderInfo TeeInfo = GameClient()->m_aClients[i].m_RenderInfo;
					if(Frozen && !g_Config.m_TcShowFrozenHudSkins)
						TeeInfo = FreezeInfo;

					NumDisplayed++;
					NumInRow++;
					if(NumInRow > MaxTees)
					{
						NumInRow = 1;
						ProgressiveOffset = 0.0f;
						CurrentRow++;
					}

					TeeInfo.m_Size = TeeSize;
					const CAnimState *pIdleState = CAnimState::GetIdle();
					vec2 TeeRenderPos(TeeRow.x + TeeSize / 2.0f + ProgressiveOffset, TeeRow.y + TeeSize * (0.7f) + CurrentRow * (TeeSize + TeeGap));
					float Alpha = 1.0f;
					CNetObj_Character CurChar = GameClient()->m_aClients[i].m_RenderCur;
					if(g_Config.m_TcShowFrozenHudSkins && Frozen)
					{
						// Accent tint instead of the old x0.4 darkening.
						Alpha = 0.85f;
						TeeInfo.m_ColorBody = TeeInfo.m_ColorBody.Multiply(AccentColor);
						TeeInfo.m_ColorFeet = TeeInfo.m_ColorFeet.Multiply(AccentColor);
					}
					if(Unknown)
					{
						// Unknown is not a frozen x_ninja: retain the player's skin,
						// dim it and mark it with a status dot instead of a cached emote.
						RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), TeeRenderPos, 0.45f);
					}
					else if(Frozen)
						RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_PAIN, vec2(1.0f, 0.0f), TeeRenderPos, Alpha);
					else
						RenderTools()->RenderTee(pIdleState, &TeeInfo, CurChar.m_Emote, vec2(1.0f, 0.0f), TeeRenderPos);
					ProgressiveOffset += TeeSize + TeeGap;

					// Two-layer status dot in the tee cell's bottom-right corner:
					// a dim outer disc plus a brighter inner core. Frozen tees use
					// the accent color, unknown tees stay white, alive tees get none.
					if(Frozen || Unknown)
					{
						const float DotR = std::max(1.0f, TeeSize * 0.16f);
						const float DotX = TeeRenderPos.x + TeeSize / 2.0f - DotR;
						const float DotY = TeeRow.y + CurrentRow * (TeeSize + TeeGap) + TeeSize - DotR;
						const ColorRGBA DotColor = Unknown ? ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f) : AccentColor;
						Graphics()->TextureClear();
						Graphics()->QuadsBegin();
						Graphics()->SetColor(ColorRGBA(DotColor.r, DotColor.g, DotColor.b, 0.22f));
						Graphics()->DrawCircle(DotX, DotY, DotR, 16);
						Graphics()->SetColor(ColorRGBA(DotColor.r, DotColor.g, DotColor.b, 0.55f));
						Graphics()->DrawCircle(DotX, DotY, DotR * 0.55f, 16);
						Graphics()->QuadsEnd();
					}
				}
			}
		}
	}
	TextRender()->TextColor(OldTextColor);
	TextRender()->TextOutlineColor(OldTextOutlineColor);
	TextRender()->SetRenderFlags(OldTextFlags);
	Graphics()->MapScreen(OldScreen);
}

void CHud::RenderConnectionWarning()
{
	if(Client()->ConnectionProblems())
	{
		const char *pText = Localize("Connection Problems…");
		float w = TextRender()->TextWidth(24, pText, -1, -1.0f);
		TextRender()->Text(150 * Graphics()->ScreenAspect() - w / 2, 50, 24, pText, -1.0f);
	}
}

void CHud::RenderTeambalanceWarning()
{
	// render prompt about team-balance
	bool Flash = time() / (time_freq() / 2) % 2 == 0;
	if(GameClient()->IsTeamPlay())
	{
		int TeamDiff = GameClient()->m_Snap.m_aTeamSize[TEAM_RED] - GameClient()->m_Snap.m_aTeamSize[TEAM_BLUE];
		if(g_Config.m_ClWarningTeambalance && (TeamDiff >= 2 || TeamDiff <= -2))
		{
			const char *pText = Localize("Please balance teams!");
			if(Flash)
				TextRender()->TextColor(1, 1, 0.5f, 1);
			else
				TextRender()->TextColor(0.7f, 0.7f, 0.2f, 1.0f);
			TextRender()->Text(5, 50, 6, pText, -1.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
	}
}

void CHud::RenderCursor()
{
	const float Scale = (float)g_Config.m_TcCursorScale / 100.0f;
	if(Scale <= 0.0f)
		return;

	int CurWeapon = 0;
	vec2 TargetPos;
	float Alpha = 1.0f;

	const vec2 Center = GameClient()->m_Camera.m_Center;
	CScreenRect ScreenRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), 1.0f);
	Graphics()->MapScreen(ScreenRect);

	if(Client()->State() != IClient::STATE_DEMOPLAYBACK && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Render local cursor
		CurWeapon = std::max(0, GameClient()->m_aClients[GameClient()->m_Snap.m_LocalClientId].m_Predicted.m_ActiveWeapon);
		TargetPos = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
	}
	else
	{
		// Render spec cursor
		if(!g_Config.m_ClSpecCursor)
			return;

		// [TClient] 观战指针常驻：自由视角用鼠标位置画指针，5 秒不动淡出、移动即恢复
		if(g_Config.m_TcSpecCursorAlways && GameClient()->m_Snap.m_SpecInfo.m_Active &&
			GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		{
			static vec2 s_LastUiMouse = vec2(-1.0f, -1.0f);
			static float s_IdleStart = -1.0f;
			const float Now = Client()->LocalTime();
			const vec2 UiMouse = Ui()->MousePos();
			if(UiMouse != s_LastUiMouse)
			{
				s_LastUiMouse = UiMouse;
				s_IdleStart = Now;
			}
			const float Idle = s_IdleStart < 0.0f ? 0.0f : Now - s_IdleStart;
			float PointerAlpha = 1.0f;
			if(Idle > 5.0f)
				PointerAlpha = std::max(0.0f, 1.0f - (Idle - 5.0f) * 2.5f);
			if(PointerAlpha > 0.0f)
			{
				const CUIRect *pUiScreen = Ui()->Screen();
				const vec2 Fraction = (UiMouse - vec2(pUiScreen->x, pUiScreen->y)) / vec2(pUiScreen->w, pUiScreen->h);
				TargetPos = Center + (Fraction - vec2(0.5f, 0.5f)) * (ScreenRect.m_BottomRight - ScreenRect.m_TopLeft) * GameClient()->m_Camera.m_Zoom;
				CurWeapon = 0;
				Alpha = 0.85f * PointerAlpha;
			}
			return;
		}

		if(!GameClient()->m_CursorInfo.IsAvailable())
			return;

		bool RenderSpecCursor = (GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW) || Client()->State() == IClient::STATE_DEMOPLAYBACK;

		if(!RenderSpecCursor)
			return;

		// Calculate factor to keep cursor on screen
		const vec2 HalfSize = Center - ScreenRect.m_TopLeft;
		const vec2 ScreenPos = (GameClient()->m_CursorInfo.WorldTarget() - Center) / GameClient()->m_Camera.m_Zoom;
		const float ClampFactor = std::max({
			1.0f,
			absolute(ScreenPos.x / HalfSize.x),
			absolute(ScreenPos.y / HalfSize.y),
		});

		CurWeapon = std::max(0, GameClient()->m_CursorInfo.Weapon() % NUM_WEAPONS);
		TargetPos = ScreenPos / ClampFactor + Center;
		if(ClampFactor != 1.0f)
			Alpha /= 2.0f;
	}

	Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
	Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponCursors[CurWeapon]);
	Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_aCursorOffset[CurWeapon], TargetPos.x, TargetPos.y, Scale, Scale);
}

void CHud::PrepareAmmoHealthAndArmorQuads()
{
	float x = 5;
	float y = 5;
	IGraphics::CQuadItem Array[10];

	// ammo of the different weapons
	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		// 0.6
		for(int n = 0; n < 10; n++)
			Array[n] = IGraphics::CQuadItem(x + n * 12, y, 10, 10);

		m_aAmmoOffset[i] = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

		// 0.7
		if(i == WEAPON_GRENADE)
		{
			// special case for 0.7 grenade
			for(int n = 0; n < 10; n++)
				Array[n] = IGraphics::CQuadItem(1 + x + n * 12, y, 10, 10);
		}
		else
		{
			for(int n = 0; n < 10; n++)
				Array[n] = IGraphics::CQuadItem(x + n * 12, y, 12, 12);
		}

		Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);
	}

	// health
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 10, 10);
	m_HealthOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// empty health
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 10, 10);
	m_EmptyHealthOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// armor meter
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 10, 10);
	m_ArmorOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// empty armor meter
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 10, 10);
	m_EmptyArmorOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);
}

void CHud::RenderAmmoHealthAndArmor(const CNetObj_Character *pCharacter)
{
	if(!pCharacter)
		return;

	bool IsSixupGameSkin = GameClient()->m_GameSkin.IsSixup();
	int QuadOffsetSixup = (IsSixupGameSkin ? 10 : 0);

	if(GameClient()->m_GameInfo.m_HudAmmo)
	{
		// ammo display
		float AmmoOffsetY = GameClient()->m_GameInfo.m_HudHealthArmor ? 24 : 0;
		int CurWeapon = pCharacter->m_Weapon % NUM_WEAPONS;
		// 0.7 only
		if(CurWeapon == WEAPON_NINJA)
		{
			if(!GameClient()->m_GameInfo.m_HudDDRace && Client()->IsSixup())
			{
				const int Max = g_pData->m_Weapons.m_Ninja.m_Duration * Client()->GameTickSpeed() / 1000;
				float NinjaProgress = std::clamp(pCharacter->m_AmmoCount - Client()->GameTick(g_Config.m_ClDummy), 0, Max) / (float)Max;
				RenderNinjaBarPos(5 + 10 * 12, 5, 6.f, 24.f, NinjaProgress);
			}
		}
		else if(CurWeapon >= 0 && GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon].IsValid())
		{
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon]);
			if(AmmoOffsetY > 0)
			{
				Graphics()->RenderQuadContainerEx(m_HudQuadContainerIndex, m_aAmmoOffset[CurWeapon] + QuadOffsetSixup, std::clamp(pCharacter->m_AmmoCount, 0, 10), 0, AmmoOffsetY);
			}
			else
			{
				Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_aAmmoOffset[CurWeapon] + QuadOffsetSixup, std::clamp(pCharacter->m_AmmoCount, 0, 10));
			}
		}
	}

	if(GameClient()->m_GameInfo.m_HudHealthArmor)
	{
		// health display
		const int DisplayHealth = std::min(pCharacter->m_Health, 10);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHealthFull);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_HealthOffset + QuadOffsetSixup, DisplayHealth);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHealthEmpty);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_EmptyHealthOffset + QuadOffsetSixup + DisplayHealth, 10 - DisplayHealth);

		// armor display
		const int DisplayArmor = std::min(pCharacter->m_Armor, 10);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteArmorFull);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_ArmorOffset + QuadOffsetSixup, DisplayArmor);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteArmorEmpty);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_ArmorOffset + QuadOffsetSixup + DisplayArmor, 10 - DisplayArmor);
	}
}

void CHud::PreparePlayerStateQuads()
{
	float x = 5;
	float y = 5 + 24;
	IGraphics::CQuadItem Array[10];

	// Quads for displaying the available and used jumps
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	m_AirjumpOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	m_AirjumpEmptyOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// Quads for the modern jumps row: stored at the origin like the weapon
	// sprites and positioned explicitly at render time, so the spacing is
	// independent of the quad geometry.
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(0.0f, 0.0f, 8.0f, 8.0f);
	m_ModernAirjumpOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(0.0f, 0.0f, 8.0f, 8.0f);
	m_ModernAirjumpEmptyOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// Quads for displaying weapons
	for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
	{
		const CDataWeaponspec &WeaponSpec = g_pData->m_Weapons.m_aId[Weapon];
		float ScaleX, ScaleY;
		Graphics()->GetSpriteScale(WeaponSpec.m_pSpriteBody, ScaleX, ScaleY);
		constexpr float HudWeaponScale = 0.25f;
		float Width = WeaponSpec.m_VisualSize * ScaleX * HudWeaponScale;
		float Height = WeaponSpec.m_VisualSize * ScaleY * HudWeaponScale;
		m_aWeaponOffset[Weapon] = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, Width, Height);
	}

	// Quads for displaying capabilities
	m_EndlessJumpOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_EndlessHookOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_JetpackOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_TeleportGrenadeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_TeleportGunOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_TeleportLaserOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying prohibited capabilities
	m_SoloOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_CollisionDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_HookHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_HammerHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_GunHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_ShotgunHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_GrenadeHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_LaserHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying freeze status
	m_DeepFrozenOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_LiveFrozenOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying dummy actions
	m_DummyHammerOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_DummyCopyOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying team modes
	m_PracticeModeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_LockModeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_Team0ModeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
}

void CHud::RenderPlayerState(const int ClientId)
{
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	// pCharacter contains the predicted character for local players or the last snap for players who are spectated
	CCharacterCore *pCharacter = &GameClient()->m_aClients[ClientId].m_Predicted;
	CNetObj_Character *pPlayer = &GameClient()->m_aClients[ClientId].m_RenderCur;
	const bool ModernGuns = g_Config.m_TcModernGuns != 0;
	const float HudOffsetY = (GameClient()->m_GameInfo.m_HudHealthArmor && g_Config.m_ClShowhudHealthAmmo ? 24.0f : 0.0f) +
				 (GameClient()->m_GameInfo.m_HudAmmo && g_Config.m_ClShowhudHealthAmmo ? 12.0f : 0.0f);
	int TotalJumpsToDisplay = 0;
	if(g_Config.m_ClShowhudJumpsIndicator)
	{
		int AvailableJumpsToDisplay;
		if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo)
		{
			const bool Grounded = Collision()->IsOnGround(vec2(pPlayer->m_X, pPlayer->m_Y), CCharacterCore::PhysicalSize());
			int UsedJumps = pCharacter->m_JumpedTotal;
			if(pCharacter->m_Jumps > 1)
			{
				UsedJumps += !Grounded;
			}
			else if(pCharacter->m_Jumps == 1)
			{
				// If the player has only one jump, each jump is the last one
				UsedJumps = pPlayer->m_Jumped & 2;
			}
			else if(pCharacter->m_Jumps == -1)
			{
				// The player has only one ground jump
				UsedJumps = !Grounded;
			}

			if(pCharacter->m_EndlessJump && UsedJumps >= absolute(pCharacter->m_Jumps))
			{
				UsedJumps = absolute(pCharacter->m_Jumps) - 1;
			}

			int UnusedJumps = absolute(pCharacter->m_Jumps) - UsedJumps;
			if(!(pPlayer->m_Jumped & 2) && UnusedJumps <= 0)
			{
				// In some edge cases when the player just got another number of jumps, UnusedJumps is not correct
				UnusedJumps = 1;
			}
			TotalJumpsToDisplay = std::clamp(absolute(pCharacter->m_Jumps), 0, 10);
			AvailableJumpsToDisplay = std::clamp(UnusedJumps, 0, TotalJumpsToDisplay);
		}
		else
		{
			TotalJumpsToDisplay = AvailableJumpsToDisplay = std::clamp(absolute(GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Jumps), 0, 10);
		}

		// render available and used jumps
		int JumpsOffsetY = ((GameClient()->m_GameInfo.m_HudHealthArmor && g_Config.m_ClShowhudHealthAmmo ? 24 : 0) +
				    (GameClient()->m_GameInfo.m_HudAmmo && g_Config.m_ClShowhudHealthAmmo ? 12 : 0));
		if(ModernGuns)
		{
			const char *pJumps = TCLocalize("Jumps");
			const float LabelWidth = TextRender()->TextWidth(6.0f, pJumps);
			CUIRect Label = {11.0f, HudOffsetY + 39.0f, LabelWidth, 10.0f};
			TextRender()->TextColor(0.94f, 0.935f, 0.95f, 0.9f);
			Ui()->DoLabel(&Label, pJumps, 6.0f, TEXTALIGN_ML);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
			// 跳跃图标读 HUD 皮肤（hud.png 的 airjump 槽位）：玩家自定义 HUD
			// 素材（如兔子图标）直接生效，默认皮肤显示原生图标；与量子/非现
			// 代版共用同一素材来源。
			constexpr float JumpIconSize = 8.0f;
			constexpr float JumpIconGap = 1.0f;
			const float JumpIconY = HudOffsetY + 44.0f - JumpIconSize / 2.0f;
			for(int Jump = 0; Jump < TotalJumpsToDisplay; ++Jump)
			{
				const float IconX = 18.5f + LabelWidth + Jump * (JumpIconSize + JumpIconGap);
				if(Jump < AvailableJumpsToDisplay)
				{
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
					Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjump);
					Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_ModernAirjumpOffset + Jump, IconX, JumpIconY);
				}
				else
				{
					Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.6f);
					Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjumpEmpty);
					Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_ModernAirjumpEmptyOffset + Jump, IconX, JumpIconY);
				}
			}
			Graphics()->SetColor(1, 1, 1, 1);
		}
		else if(JumpsOffsetY > 0)
		{
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjump);
			Graphics()->RenderQuadContainerEx(m_HudQuadContainerIndex, m_AirjumpOffset, AvailableJumpsToDisplay, 0, JumpsOffsetY);
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjumpEmpty);
			Graphics()->RenderQuadContainerEx(m_HudQuadContainerIndex, m_AirjumpEmptyOffset + AvailableJumpsToDisplay, TotalJumpsToDisplay - AvailableJumpsToDisplay, 0, JumpsOffsetY);
		}
		else
		{
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjump);
			Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_AirjumpOffset, AvailableJumpsToDisplay);
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjumpEmpty);
			Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_AirjumpEmptyOffset + AvailableJumpsToDisplay, TotalJumpsToDisplay - AvailableJumpsToDisplay);
		}
	}

	float x = ModernGuns ? 18.0f : 17.0f;
	float y = HudOffsetY + (ModernGuns ? 28.0f : 17.0f);
	if(ModernGuns)
	{
		const char *apWeapons[NUM_WEAPONS] = {Localize("Hammer"), Localize("Pistol"), Localize("Shotgun"), Localize("Grenade"), Localize("Laser"), TCLocalize("Ninja")};
		const int Weapon = std::clamp(pPlayer->m_Weapon, 0, NUM_WEAPONS - 1);
		CUIRect Name = {11.0f, HudOffsetY + 6.0f, 110.0f, 10.0f};
		TextRender()->TextColor(0.94f, 0.935f, 0.95f, 1.0f);
		Ui()->DoLabel(&Name, apWeapons[Weapon], 7.0f, TEXTALIGN_ML);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	// render weapons
	{
		constexpr float aWeaponWidth[NUM_WEAPONS] = {16, 12, 12, 12, 12, 12};
		constexpr float aWeaponInitialOffset[NUM_WEAPONS] = {-3, -4, -1, -1, -2, -4};
		bool InitialOffsetAdded = false;
		for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
		{
			if(!pCharacter->m_aWeapons[Weapon].m_Got)
				continue;
			if(!InitialOffsetAdded)
			{
				if(!ModernGuns)
					x += aWeaponInitialOffset[Weapon];
				InitialOffsetAdded = true;
			}
			if(g_Config.m_TcModernGuns && pPlayer->m_Weapon == Weapon)
				Graphics()->DrawRect(x - 6.0f, y + 8.0f, 12.0f, 1.0f, TClientModernUi::Accent(x, y), IGraphics::CORNER_ALL, 0.5f);
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, pPlayer->m_Weapon == Weapon ? 1.0f : (ModernGuns ? 0.65f : 0.4f));
			Graphics()->QuadsSetRotation(pi * 7 / 4);
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpritePickupWeapons[Weapon]);
			const float Scale = g_Config.m_TcModernGuns ? 0.85f : 1.0f;
			Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_aWeaponOffset[Weapon], x, y, Scale, Scale);
			Graphics()->QuadsSetRotation(0);
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			x += ModernGuns ? 18.0f : aWeaponWidth[Weapon];
		}
		if(pCharacter->m_aWeapons[WEAPON_NINJA].m_Got)
		{
			const int Max = g_pData->m_Weapons.m_Ninja.m_Duration * Client()->GameTickSpeed() / 1000;
			float NinjaProgress = std::clamp(pCharacter->m_Ninja.m_ActivationTick + g_pData->m_Weapons.m_Ninja.m_Duration * Client()->GameTickSpeed() / 1000 - Client()->GameTick(g_Config.m_ClDummy), 0, Max) / (float)Max;
			if(NinjaProgress > 0.0f && GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo)
			{
				RenderNinjaBarPos(x, y - 12, 6.f, 24.f, NinjaProgress);
			}
		}
	}

	// render capabilities
	x = 5;
	y = ModernGuns ? HudOffsetY + (g_Config.m_TcModernFreezeTimer ? 74.0f : 53.0f) : y + 12.0f;
	if(!ModernGuns && TotalJumpsToDisplay > 0)
	{
		y += 12;
	}
	bool HasCapabilities = false;
	if(pCharacter->m_EndlessJump)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudEndlessJump);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_EndlessJumpOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_EndlessHook)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudEndlessHook);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_EndlessHookOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_Jetpack)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudJetpack);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_JetpackOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HasTelegunGun && pCharacter->m_aWeapons[WEAPON_GUN].m_Got)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeleportGun);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_TeleportGunOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HasTelegunGrenade && pCharacter->m_aWeapons[WEAPON_GRENADE].m_Got)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeleportGrenade);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_TeleportGrenadeOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HasTelegunLaser && pCharacter->m_aWeapons[WEAPON_LASER].m_Got)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeleportLaser);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_TeleportLaserOffset, x, y);
	}

	// render prohibited capabilities
	x = 5;
	if(HasCapabilities)
	{
		y += 12;
	}
	bool HasProhibitedCapabilities = false;
	if(pCharacter->m_Solo)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudSolo);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_SoloOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_CollisionDisabled)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudCollisionDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_CollisionDisabledOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HookHitDisabled)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudHookHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_HookHitDisabledOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HammerHitDisabled)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudHammerHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_HammerHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_GrenadeHitDisabled && pCharacter->m_HasTelegunGun && pCharacter->m_aWeapons[WEAPON_GUN].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudGunHitDisabled);
		// [TClient] 审计修复（2026-11-20 P3）：改用专用 Gun 槽位（此前误用 Laser 槽）
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_GunHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_ShotgunHitDisabled && pCharacter->m_aWeapons[WEAPON_SHOTGUN].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudShotgunHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_ShotgunHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_GrenadeHitDisabled && pCharacter->m_aWeapons[WEAPON_GRENADE].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudGrenadeHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_GrenadeHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_LaserHitDisabled && pCharacter->m_aWeapons[WEAPON_LASER].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudLaserHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_LaserHitDisabledOffset, x, y);
	}

	// render dummy actions and freeze state
	x = 5;
	if(HasProhibitedCapabilities)
	{
		y += 12;
	}
	if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo && GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Flags & CHARACTERFLAG_LOCK_MODE)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudLockMode);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_LockModeOffset, x, y);
		x += 12;
	}
	if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo && GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Flags & CHARACTERFLAG_PRACTICE_MODE)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudPracticeMode);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_PracticeModeOffset, x, y);
		x += 12;
	}
	if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo && GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Flags & CHARACTERFLAG_TEAM0_MODE)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeam0Mode);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_Team0ModeOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_DeepFrozen)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudDeepFrozen);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_DeepFrozenOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_LiveFrozen)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudLiveFrozen);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_LiveFrozenOffset, x, y);
	}

	if(g_Config.m_TcModernFreezeTimer)
	{
		const int FreezeEnd = GameClient()->m_aClients[ClientId].m_FreezeEnd;
		const bool Permanent = pCharacter->m_DeepFrozen || pCharacter->m_LiveFrozen || FreezeEnd < 0;
		const int TickSpeed = std::max(1, Client()->GameTickSpeed());
		// 倒计时一律以快照 FreezeEnd 为准，本地/远程同源（2026-11-20 审查 P3-2）：
		// 旧写法冻结中取"总时长"当剩余，秒数钉死在总时长且进度恒 1。
		const float Remaining = std::max(0.0f, FreezeEnd - Client()->GameTick(g_Config.m_ClDummy) - Client()->IntraGameTick(g_Config.m_ClDummy));
		// 总时长只做进度分母：m_FreezeStart 快照未提供（-1/0）时退化为满条。
		const float Duration = pCharacter->m_FreezeStart > 0 && pCharacter->m_FreezeEnd > pCharacter->m_FreezeStart ?
			(float)(pCharacter->m_FreezeEnd - pCharacter->m_FreezeStart) :
			std::max(1.0f, Remaining);
		if(Permanent || (FreezeEnd > 0 && Remaining > 0.0f))
		{
			const float TimerY = ModernGuns ? HudOffsetY + 51.0f : y + 18.0f;
			const ColorRGBA Foreground(0.94f, 0.935f, 0.95f, 1.0f);
			char aTime[32];
			if(Permanent)
				str_copy(aTime, "∞");
			else
				str_format(aTime, sizeof(aTime), "%.1f s", Remaining / TickSpeed);
			const char *pFrozen = TCLocalize("Frozen");
			const float LabelWidth = TextRender()->TextWidth(6.0f, pFrozen);
			// 固定数字区域，避免每次十分之一秒变化都让进度条和秒数跳动。
			const float TimeWidth = std::max(TextRender()->TextWidth(6.0f, "0.0 s"), TextRender()->TextWidth(6.0f, aTime));
			const float Right = 26.0f + LabelWidth + TimeWidth;
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpritePickupFreeze);
			Graphics()->QuadsBegin();
			Graphics()->SetColor(Foreground);
			IGraphics::CQuadItem Snowflake(10.0f, TimerY + 2.0f, 8.0f, 8.0f);
			Graphics()->QuadsDrawTL(&Snowflake, 1);
			Graphics()->QuadsEnd();
			TextRender()->TextColor(Foreground);
			CUIRect Label = {20.0f, TimerY + 1.0f, LabelWidth, 10.0f};
			CUIRect Value = {26.0f + LabelWidth, TimerY + 1.0f, TimeWidth, 10.0f};
			Ui()->DoLabel(&Label, pFrozen, 6.0f, TEXTALIGN_ML);
			Ui()->DoLabel(&Value, aTime, 6.0f, TEXTALIGN_MR);
			const float BarWidth = Right - 11.0f;
			Graphics()->DrawRect(11.0f, TimerY + 13.0f, BarWidth, 2.0f, Foreground.WithAlpha(0.12f), IGraphics::CORNER_ALL, 1.0f);
			const float Progress = Permanent ? 1.0f : std::clamp(Remaining / Duration, 0.0f, 1.0f);
			if(Progress > 0.0f)
				Graphics()->DrawRect(11.0f, TimerY + 13.0f, BarWidth * Progress, 2.0f, TClientModernUi::Accent(11.0f, TimerY), IGraphics::CORNER_ALL, 1.0f);
			Graphics()->SetColor(1, 1, 1, 1);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
	}
}

void CHud::RenderNinjaBarPos(const float x, float y, const float Width, const float Height, float Progress, const float Alpha)
{
	Progress = std::clamp(Progress, 0.0f, 1.0f);

	// what percentage of the end pieces is used for the progress indicator and how much is the rest
	// half of the ends are used for the progress display
	const float RestPct = 0.5f;
	const float ProgPct = 0.5f;

	const float EndHeight = Width; // to keep the correct scale - the width of the sprite is as long as the height
	const float BarWidth = Width;
	const float WholeBarHeight = Height;
	const float MiddleBarHeight = WholeBarHeight - (EndHeight * 2.0f);
	const float EndProgressHeight = EndHeight * ProgPct;
	const float EndRestHeight = EndHeight * RestPct;
	const float ProgressBarHeight = WholeBarHeight - (EndProgressHeight * 2.0f);
	const float EndProgressProportion = EndProgressHeight / ProgressBarHeight;
	const float MiddleProgressProportion = MiddleBarHeight / ProgressBarHeight;

	// beginning piece
	float BeginningPieceProgress = 1;
	if(Progress <= 1)
	{
		if(Progress <= (EndProgressProportion + MiddleProgressProportion))
		{
			BeginningPieceProgress = 0;
		}
		else
		{
			BeginningPieceProgress = (Progress - EndProgressProportion - MiddleProgressProportion) / EndProgressProportion;
		}
	}
	// empty
	Graphics()->WrapClamp();
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarEmptyRight);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
	// Subset: btm_r, top_r, top_m, btm_m | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
	Graphics()->QuadsSetSubsetFree(1, 1, 1, 0, ProgPct - ProgPct * (1.0f - BeginningPieceProgress), 0, ProgPct - ProgPct * (1.0f - BeginningPieceProgress), 1);
	IGraphics::CQuadItem QuadEmptyBeginning(x, y, BarWidth, EndRestHeight + EndProgressHeight * (1.0f - BeginningPieceProgress));
	Graphics()->QuadsDrawTL(&QuadEmptyBeginning, 1);
	Graphics()->QuadsEnd();
	// full
	if(BeginningPieceProgress > 0.0f)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarFullLeft);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
		// Subset: btm_m, top_m, top_r, btm_r | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(RestPct + ProgPct * (1.0f - BeginningPieceProgress), 1, RestPct + ProgPct * (1.0f - BeginningPieceProgress), 0, 1, 0, 1, 1);
		IGraphics::CQuadItem QuadFullBeginning(x, y + (EndRestHeight + EndProgressHeight * (1.0f - BeginningPieceProgress)), BarWidth, EndProgressHeight * BeginningPieceProgress);
		Graphics()->QuadsDrawTL(&QuadFullBeginning, 1);
		Graphics()->QuadsEnd();
	}

	// middle piece
	y += EndHeight;

	float MiddlePieceProgress = 1;
	if(Progress <= EndProgressProportion + MiddleProgressProportion)
	{
		if(Progress <= EndProgressProportion)
		{
			MiddlePieceProgress = 0;
		}
		else
		{
			MiddlePieceProgress = (Progress - EndProgressProportion) / MiddleProgressProportion;
		}
	}

	const float FullMiddleBarHeight = MiddleBarHeight * MiddlePieceProgress;
	const float EmptyMiddleBarHeight = MiddleBarHeight - FullMiddleBarHeight;

	// empty ninja bar
	if(EmptyMiddleBarHeight > 0.0f)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarEmpty);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
		// select the middle portion of the sprite so we don't get edge bleeding
		if(EmptyMiddleBarHeight <= EndHeight)
		{
			// prevent pixel puree, select only a small slice
			// Subset: btm_r, top_r, top_m, btm_m | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
			Graphics()->QuadsSetSubsetFree(1, 1, 1, 0, 1.0f - (EmptyMiddleBarHeight / EndHeight), 0, 1.0f - (EmptyMiddleBarHeight / EndHeight), 1);
		}
		else
		{
			// Subset: btm_r, top_r, top_l, btm_l | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
			Graphics()->QuadsSetSubsetFree(1, 1, 1, 0, 0, 0, 0, 1);
		}
		IGraphics::CQuadItem QuadEmpty(x, y, BarWidth, EmptyMiddleBarHeight);
		Graphics()->QuadsDrawTL(&QuadEmpty, 1);
		Graphics()->QuadsEnd();
	}

	// full ninja bar
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarFull);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
	// select the middle portion of the sprite so we don't get edge bleeding
	if(FullMiddleBarHeight <= EndHeight)
	{
		// prevent pixel puree, select only a small slice
		// Subset: btm_m, top_m, top_r, btm_r | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(1.0f - (FullMiddleBarHeight / EndHeight), 1, 1.0f - (FullMiddleBarHeight / EndHeight), 0, 1, 0, 1, 1);
	}
	else
	{
		// Subset: btm_l, top_l, top_r, btm_r | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(0, 1, 0, 0, 1, 0, 1, 1);
	}
	IGraphics::CQuadItem QuadFull(x, y + EmptyMiddleBarHeight, BarWidth, FullMiddleBarHeight);
	Graphics()->QuadsDrawTL(&QuadFull, 1);
	Graphics()->QuadsEnd();

	// ending piece
	y += MiddleBarHeight;
	float EndingPieceProgress = 1;
	if(Progress <= EndProgressProportion)
	{
		EndingPieceProgress = Progress / EndProgressProportion;
	}
	// empty
	if(EndingPieceProgress < 1.0f)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarEmptyRight);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
		// Subset: btm_l, top_l, top_m, btm_m | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(0, 1, 0, 0, ProgPct - ProgPct * EndingPieceProgress, 0, ProgPct - ProgPct * EndingPieceProgress, 1);
		IGraphics::CQuadItem QuadEmptyEnding(x, y, BarWidth, EndProgressHeight * (1.0f - EndingPieceProgress));
		Graphics()->QuadsDrawTL(&QuadEmptyEnding, 1);
		Graphics()->QuadsEnd();
	}
	// full
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarFullLeft);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
	// Subset: btm_m, top_m, top_l, btm_l | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
	Graphics()->QuadsSetSubsetFree(RestPct + ProgPct * EndingPieceProgress, 1, RestPct + ProgPct * EndingPieceProgress, 0, 0, 0, 0, 1);
	IGraphics::CQuadItem QuadFullEnding(x, y + (EndProgressHeight * (1.0f - EndingPieceProgress)), BarWidth, EndRestHeight + EndProgressHeight * EndingPieceProgress);
	Graphics()->QuadsDrawTL(&QuadFullEnding, 1);
	Graphics()->QuadsEnd();

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
	Graphics()->WrapNormal();
}

void CHud::RenderSpectatorCount()
{
	if(!g_Config.m_ClShowhudSpectatorCount)
	{
		return;
	}

	int Count = 0;
	int aSpectatorIds[MAX_CLIENTS];
	int NumSpectatorIds = 0;
	bool ReliableNames = false;
	auto AddSpectator = [&](int ClientId) {
		if(ClientId < 0 || ClientId >= MAX_CLIENTS)
			return;
		for(int i = 0; i < NumSpectatorIds; i++)
			if(aSpectatorIds[i] == ClientId)
				return;
		aSpectatorIds[NumSpectatorIds++] = ClientId;
	};
	const int LocalId = GameClient()->m_aLocalIds[0];
	const int DummyId = Client()->DummyConnected() ? GameClient()->m_aLocalIds[1] : -1;
	if(Client()->IsSixup())
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == LocalId || i == DummyId)
				continue;

			if(Client()->m_TranslationContext.m_aClients[i].m_PlayerFlags7 & protocol7::PLAYERFLAG_WATCHING)
			{
				AddSpectator(i);
			}
		}
		Count = NumSpectatorIds;
		ReliableNames = true;
	}
	else
	{
		const CNetObj_SpectatorCount *pSpectatorCount = GameClient()->m_Snap.m_pSpectatorCount;
		if(!pSpectatorCount)
		{
			m_LastSpectatorCountTick = Client()->GameTick(g_Config.m_ClDummy);
			return;
		}
		Count = pSpectatorCount->m_NumSpectators;
		if(g_Config.m_TcShowSpectatorNames && Count > 0)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(i == LocalId || i == DummyId)
					continue;

				const CNetObj_PlayerInfo *pInfo = GameClient()->m_Snap.m_apPlayerInfos[i];
				if(!pInfo || !GameClient()->m_aClients[i].m_Active)
					continue;

				if(pInfo->m_Team == TEAM_SPECTATORS || GameClient()->m_aClients[i].m_Spec || GameClient()->m_aClients[i].m_Paused)
					AddSpectator(i);
			}
			ReliableNames = NumSpectatorIds == Count;
		}
	}

	if(Count == 0)
	{
		m_LastSpectatorCountTick = Client()->GameTick(g_Config.m_ClDummy);
		return;
	}

	// 1 second delay
	if(Client()->GameTick(g_Config.m_ClDummy) < m_LastSpectatorCountTick + Client()->GameTickSpeed())
		return;

	char aBuf[16];
	str_format(aBuf, sizeof(aBuf), "%d", Count);

	const float Fontsize = 6.0f;
	const float LineHeight = Fontsize + 1.0f;

	const char *apNameLines[6] = {};
	int NumNameLines = 0;
	char aRemaining[16] = {};
	if(g_Config.m_TcShowSpectatorNames && ReliableNames && NumSpectatorIds > 0)
	{
		for(int i = 0; i < NumSpectatorIds && NumNameLines < 5; i++)
		{
			const char *pName = GameClient()->m_aClients[aSpectatorIds[i]].m_aName;
			if(pName[0] == '\0')
				continue;
			apNameLines[NumNameLines++] = pName;
		}

		if(Count > NumNameLines)
		{
			str_format(aRemaining, sizeof(aRemaining), "+%d", Count - NumNameLines);
			apNameLines[NumNameLines++] = aRemaining;
		}
	}

	float MaxNameWidth = 0.0f;
	for(int i = 0; i < NumNameLines; i++)
		MaxNameWidth = std::max(MaxNameWidth, TextRender()->TextWidth(Fontsize, apNameLines[i]));

	const float BoxWidth = std::max(13.0f + TextRender()->TextWidth(Fontsize, aBuf), MaxNameWidth + 4.0f);
	const float BoxHeight = 14.0f + NumNameLines * LineHeight;

	float StartX = m_Width - BoxWidth;
	float StartY = 285.0f - BoxHeight - 4; // 4 units distance to the next display;
	if(g_Config.m_ClShowhudPlayerPosition || g_Config.m_ClShowhudPlayerSpeed || g_Config.m_ClShowhudPlayerAngle)
	{
		StartY -= 4;
	}
	StartY -= GetMovementInformationBoxHeight();

	if(g_Config.m_ClShowhudScore)
	{
		StartY -= 56;
	}

	if(g_Config.m_ClShowhudDummyActions && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) && Client()->DummyConnected())
	{
		StartY = StartY - 29.0f - 4; // dummy actions height and padding
	}

	m_SpectatorCountBoxShown = true;
	m_SpectatorCountBoxHeight = BoxHeight;
	Graphics()->DrawRect(StartX, StartY, BoxWidth, BoxHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_L, 5.0f);

	float y = StartY + 4.0f;
	float x = StartX + 2;

	TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
	TextRender()->Text(x, y, Fontsize, FontIcon::EYE, -1.0f);
	TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	TextRender()->Text(x + Fontsize + 3.f, y, Fontsize, aBuf, -1.0f);

	float NameY = StartY + 14.0f + 1.0f;
	for(int i = 0; i < NumNameLines; i++)
	{
		TextRender()->Text(x, NameY, Fontsize, apNameLines[i], -1.0f);
		NameY += LineHeight;
	}
}

void CHud::RenderDummyActions()
{
	if(!g_Config.m_ClShowhudDummyActions || (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) || !Client()->DummyConnected())
	{
		return;
	}
	// render small dummy actions hud
	const float BoxHeight = 29.0f;
	const float BoxWidth = 16.0f;

	float StartX = m_Width - BoxWidth;
	float StartY = 285.0f - BoxHeight - 4; // 4 units distance to the next display;
	if(g_Config.m_ClShowhudPlayerPosition || g_Config.m_ClShowhudPlayerSpeed || g_Config.m_ClShowhudPlayerAngle)
	{
		StartY -= 4;
	}
	StartY -= GetMovementInformationBoxHeight();

	if(g_Config.m_ClShowhudScore)
	{
		StartY -= 56;
	}

	Graphics()->DrawRect(StartX, StartY, BoxWidth, BoxHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_L, 5.0f);

	float y = StartY + 2;
	float x = StartX + 2;
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
	if(g_Config.m_ClDummyHammer)
	{
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudDummyHammer);
	Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_DummyHammerOffset, x, y);
	y += 13;
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
	if(g_Config.m_ClDummyCopyMoves)
	{
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudDummyCopy);
	Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_DummyCopyOffset, x, y);
}

inline int CHud::GetDigitsIndex(int Value, int Max)
{
	if(Value < 0)
	{
		Value *= -1;
	}
	int DigitsIndex = std::log10((Value ? Value : 1));
	if(DigitsIndex > Max)
	{
		DigitsIndex = Max;
	}
	if(DigitsIndex < 0)
	{
		DigitsIndex = 0;
	}
	return DigitsIndex;
}

inline float CHud::GetMovementInformationBoxHeight()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && (GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW || GameClient()->m_aClients[GameClient()->m_Snap.m_SpecInfo.m_SpectatorId].m_SpecCharPresent))
		return g_Config.m_ClShowhudPlayerPosition ? 3.0f * MOVEMENT_INFORMATION_LINE_HEIGHT + 2.0f : 0.0f;
	float BoxHeight = 3.0f * MOVEMENT_INFORMATION_LINE_HEIGHT * (g_Config.m_ClShowhudPlayerPosition + g_Config.m_ClShowhudPlayerSpeed) + 2.0f * MOVEMENT_INFORMATION_LINE_HEIGHT * g_Config.m_ClShowhudPlayerAngle;
	if(g_Config.m_ClShowhudPlayerPosition || g_Config.m_ClShowhudPlayerSpeed || g_Config.m_ClShowhudPlayerAngle)
	{
		BoxHeight += 2.0f;
	}
	return BoxHeight;
}

void CHud::UpdateMovementInformationTextContainer(STextContainerIndex &TextContainer, float FontSize, float Value, float &PrevValue)
{
	Value = std::round(Value * 100.0f) / 100.0f; // Round to 2dp
	if(TextContainer.Valid() && PrevValue == Value)
		return;
	PrevValue = Value;

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "%.2f", Value);

	CTextCursor Cursor;
	Cursor.m_FontSize = FontSize;
	if(TextContainer.Valid())
		TextRender()->RecreateTextContainerSoft(TextContainer, &Cursor, aBuf);
	else
		TextRender()->CreateTextContainer(TextContainer, &Cursor, aBuf);
}

void CHud::RenderMovementInformationTextContainer(STextContainerIndex &TextContainer, const ColorRGBA &Color, float X, float Y)
{
	if(TextContainer.Valid())
	{
		TextRender()->RenderTextContainer(TextContainer, Color, TextRender()->DefaultTextOutlineColor(), X - TextRender()->GetBoundingBoxTextContainer(TextContainer).m_W, Y);
	}
}

CHud::CMovementInformation CHud::GetMovementInformation(int ClientId, int Conn) const
{
	CMovementInformation Out;
	if(ClientId == SPEC_FREEVIEW)
	{
		Out.m_Pos = GameClient()->m_Camera.m_Center / 32.0f;
	}
	else if(GameClient()->m_aClients[ClientId].m_SpecCharPresent)
	{
		Out.m_Pos = GameClient()->m_aClients[ClientId].m_SpecChar / 32.0f;
	}
	else
	{
		const CNetObj_Character *pPrevChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev;
		const CNetObj_Character *pCurChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
		const float IntraTick = Client()->IntraGameTick(Conn);

		// To make the player position relative to blocks we need to divide by the block size
		Out.m_Pos = mix(vec2(pPrevChar->m_X, pPrevChar->m_Y), vec2(pCurChar->m_X, pCurChar->m_Y), IntraTick) / 32.0f;

		const vec2 Vel = mix(vec2(pPrevChar->m_VelX, pPrevChar->m_VelY), vec2(pCurChar->m_VelX, pCurChar->m_VelY), IntraTick);

		float VelspeedX = Vel.x / 256.0f * Client()->GameTickSpeed();
		if(Vel.x >= -1.0f && Vel.x <= 1.0f)
		{
			VelspeedX = 0.0f;
		}
		float VelspeedY = Vel.y / 256.0f * Client()->GameTickSpeed();
		if(Vel.y >= -128.0f && Vel.y <= 128.0f)
		{
			VelspeedY = 0.0f;
		}
		// We show the speed in Blocks per Second (Bps) and therefore have to divide by the block size
		Out.m_Speed.x = VelspeedX / 32.0f;
		float VelspeedLength = length(vec2(Vel.x, Vel.y) / 256.0f) * Client()->GameTickSpeed();
		// Todo: Use Velramp tuning of each individual player
		// Since these tuning parameters are almost never changed, the default values are sufficient in most cases
		float Ramp = VelocityRamp(VelspeedLength, GameClient()->m_aTuning[Conn].m_VelrampStart, GameClient()->m_aTuning[Conn].m_VelrampRange, GameClient()->m_aTuning[Conn].m_VelrampCurvature);
		Out.m_Speed.x *= Ramp;
		Out.m_Speed.y = VelspeedY / 32.0f;

		float Angle = GameClient()->m_Players.GetPlayerTargetAngle(pPrevChar, pCurChar, ClientId, IntraTick);
		if(Angle < 0.0f)
		{
			Angle += 2.0f * pi;
		}
		Out.m_Angle = Angle * 180.0f / pi;
	}
	return Out;
}

void CHud::RenderMovementInformation()
{
	const int ClientId = GameClient()->m_Snap.m_SpecInfo.m_Active ? GameClient()->m_Snap.m_SpecInfo.m_SpectatorId : GameClient()->m_Snap.m_LocalClientId;
	const bool PosOnly = ClientId == SPEC_FREEVIEW || (GameClient()->m_aClients[ClientId].m_SpecCharPresent);
	// Draw the information depending on settings: Position, speed and target angle
	// This display is only to present the available information from the last snapshot, not to interpolate or predict
	if(!g_Config.m_ClShowhudPlayerPosition && (PosOnly || (!g_Config.m_ClShowhudPlayerSpeed && !g_Config.m_ClShowhudPlayerAngle)))
	{
		return;
	}
	const float LineSpacer = 1.0f; // above and below each entry
	const float Fontsize = 6.0f;

	float BoxHeight = GetMovementInformationBoxHeight();
	bool HasDummyInfo = false;
	CMovementInformation DummyInfo{};

	if(Client()->DummyConnected())
	{
		int DummyClientId = -1;

		if(GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int SpectId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;

			if(SpectId == GameClient()->m_aLocalIds[0])
			{
				DummyClientId = GameClient()->m_aLocalIds[1];
			}
			else if(SpectId == GameClient()->m_aLocalIds[1])
			{
				DummyClientId = GameClient()->m_aLocalIds[0];
			}
			else
			{
				DummyClientId = GameClient()->m_aLocalIds[1 - (g_Config.m_ClDummy ? 1 : 0)];
			}
		}
		else
		{
			DummyClientId = GameClient()->m_aLocalIds[1 - (g_Config.m_ClDummy ? 1 : 0)];
		}

		if(DummyClientId >= 0 && DummyClientId < MAX_CLIENTS &&
			GameClient()->m_aClients[DummyClientId].m_Active)
		{
			DummyInfo = GetMovementInformation(
				DummyClientId,
				DummyClientId == GameClient()->m_aLocalIds[1]);
			HasDummyInfo = true;
		}
	}

	const bool ShowDummyPos = HasDummyInfo && g_Config.m_ClShowhudPlayerPosition && g_Config.m_TcShowhudDummyPosition;
	const bool ShowDummySpeed = HasDummyInfo && !PosOnly && g_Config.m_ClShowhudPlayerSpeed && g_Config.m_TcShowhudDummySpeed;
	const bool ShowDummyAngle = HasDummyInfo && !PosOnly && g_Config.m_ClShowhudPlayerAngle && g_Config.m_TcShowhudDummyAngle;

	if(ShowDummyPos)
		BoxHeight += 2.0f * MOVEMENT_INFORMATION_LINE_HEIGHT;
	if(ShowDummySpeed)
		BoxHeight += 2.0f * MOVEMENT_INFORMATION_LINE_HEIGHT;
	if(ShowDummyAngle)
		BoxHeight += 1.0f * MOVEMENT_INFORMATION_LINE_HEIGHT;

	const float BoxWidth = 62.0f;

	float StartX = m_Width - BoxWidth;
	float StartY = 285.0f - BoxHeight - 4.0f; // 4 units distance to the next display;
	if(g_Config.m_ClShowhudScore)
	{
		StartY -= 56.0f;
	}

	Graphics()->DrawRect(StartX, StartY, BoxWidth, BoxHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_L, 5.0f);

	const CMovementInformation Info = GetMovementInformation(ClientId, g_Config.m_ClDummy);

	float y = StartY + LineSpacer * 2.0f;
	const float LeftX = StartX + 2.0f;
	const float RightX = m_Width - 2.0f;

	if(g_Config.m_ClShowhudPlayerPosition)
	{
		TextRender()->Text(LeftX, y, Fontsize, Localize("Position:"), -1.0f);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		TextRender()->Text(LeftX, y, Fontsize, "X:", -1.0f);
		UpdateMovementInformationTextContainer(m_aPlayerPositionContainers[0], Fontsize, Info.m_Pos.x, m_aPlayerPrevPosition[0]);

		ColorRGBA TextColor = TextRender()->DefaultTextColor();
		if(ShowDummyPos && fabsf(Info.m_Pos.x - DummyInfo.m_Pos.x) < 0.01f)
			TextColor = ColorRGBA(0.2f, 1.0f, 0.2f, 1.0f);

		RenderMovementInformationTextContainer(m_aPlayerPositionContainers[0], TextColor, RightX, y);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		TextRender()->Text(LeftX, y, Fontsize, "Y:", -1.0f);
		UpdateMovementInformationTextContainer(m_aPlayerPositionContainers[1], Fontsize, Info.m_Pos.y, m_aPlayerPrevPosition[1]);
		RenderMovementInformationTextContainer(m_aPlayerPositionContainers[1], TextRender()->DefaultTextColor(), RightX, y);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		if(ShowDummyPos)
		{
			char aBuf[32];

			TextRender()->Text(LeftX, y, Fontsize, "DX:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Pos.x);

			ColorRGBA DummyTextColor = TextRender()->DefaultTextColor();
			if(fabsf(Info.m_Pos.x - DummyInfo.m_Pos.x) < 0.01f)
				DummyTextColor = ColorRGBA(0.2f, 1.0f, 0.2f, 1.0f);

			TextRender()->TextColor(DummyTextColor);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;

			TextRender()->Text(LeftX, y, Fontsize, "DY:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Pos.y);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;
		}
	}

	if(PosOnly)
		return;

	if(g_Config.m_ClShowhudPlayerSpeed)
	{
		TextRender()->Text(LeftX, y, Fontsize, Localize("Speed:"), -1.0f);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		const char aaCoordinates[][4] = {"X:", "Y:"};
		for(int i = 0; i < 2; i++)
		{
			ColorRGBA Color(1.0f, 1.0f, 1.0f, 1.0f);
			if(m_aLastPlayerSpeedChange[i] == ESpeedChange::INCREASE)
				Color = ColorRGBA(0.0f, 1.0f, 0.0f, 1.0f);
			if(m_aLastPlayerSpeedChange[i] == ESpeedChange::DECREASE)
				Color = ColorRGBA(1.0f, 0.5f, 0.5f, 1.0f);
			TextRender()->Text(LeftX, y, Fontsize, aaCoordinates[i], -1.0f);
			UpdateMovementInformationTextContainer(m_aPlayerSpeedTextContainers[i], Fontsize, i == 0 ? Info.m_Speed.x : Info.m_Speed.y, m_aPlayerPrevSpeed[i]);
			RenderMovementInformationTextContainer(m_aPlayerSpeedTextContainers[i], Color, RightX, y);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;
		}

		if(ShowDummySpeed)
		{
			char aBuf[32];

			TextRender()->Text(LeftX, y, Fontsize, "DX:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Speed.x);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;

			TextRender()->Text(LeftX, y, Fontsize, "DY:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Speed.y);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;
		}

		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	if(g_Config.m_ClShowhudPlayerAngle)
	{
		TextRender()->Text(LeftX, y, Fontsize, Localize("Angle:"), -1.0f);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		UpdateMovementInformationTextContainer(m_PlayerAngleTextContainerIndex, Fontsize, Info.m_Angle, m_PlayerPrevAngle);
		RenderMovementInformationTextContainer(m_PlayerAngleTextContainerIndex, TextRender()->DefaultTextColor(), RightX, y);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		if(ShowDummyAngle)
		{
			char aBuf[32];

			TextRender()->Text(LeftX, y, Fontsize, "DA:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Angle);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
		}
	}
}

void CHud::RenderSpectatorHud()
{
	if(!g_Config.m_ClShowhudSpectator)
		return;

	// TClient
	float AdjustedHeight = m_Height - (g_Config.m_TcStatusBar ? g_Config.m_TcStatusBarHeight : 0.0f);
	// draw the box
	Graphics()->DrawRect(m_Width - 180.0f, AdjustedHeight - 15.0f, 180.0f, 15.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_TL, 5.0f);

	// draw the text
	char aBuf[128];
	if(GameClient()->m_MultiViewActivated)
	{
		str_copy(aBuf, Localize("Multi-View"));
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
	{
		const auto &Player = GameClient()->m_aClients[GameClient()->m_Snap.m_SpecInfo.m_SpectatorId];
		if(g_Config.m_ClShowIds)
			str_format(aBuf, sizeof(aBuf), Localize("Following %d: %s", "Spectating"), Player.ClientId(), Player.m_aName);
		else
			str_format(aBuf, sizeof(aBuf), Localize("Following %s", "Spectating"), Player.m_aName);
	}
	else
	{
		str_copy(aBuf, Localize("Free-View"));
	}
	TextRender()->Text(m_Width - 174.0f, AdjustedHeight - 15.0f + (15.f - 8.f) / 2.f, 8.0f, aBuf, -1.0f);

	// draw the camera info
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK && GameClient()->m_Camera.SpectatingPlayer() && GameClient()->m_Camera.CanUseAutoSpecCamera() && g_Config.m_ClSpecAutoSync)
	{
		bool AutoSpecCameraEnabled = GameClient()->m_Camera.m_AutoSpecCamera;
		const char *pLabelText = Localize("AUTO", "Spectating Camera Mode Icon");
		const float TextWidth = TextRender()->TextWidth(6.0f, pLabelText);

		constexpr float RightMargin = 4.0f;
		constexpr float IconWidth = 6.0f;
		constexpr float Padding = 3.0f;
		const float TagWidth = IconWidth + TextWidth + Padding * 3.0f;
		const float TagX = m_Width - RightMargin - TagWidth;
		Graphics()->DrawRect(TagX, m_Height - 12.0f, TagWidth, 10.0f, ColorRGBA(1.0f, 1.0f, 1.0f, AutoSpecCameraEnabled ? 0.50f : 0.10f), IGraphics::CORNER_ALL, 2.5f);
		TextRender()->TextColor(1, 1, 1, AutoSpecCameraEnabled ? 1.0f : 0.65f);
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->Text(TagX + Padding, m_Height - 10.0f, 6.0f, FontIcon::CAMERA, -1.0f);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		TextRender()->Text(TagX + Padding + IconWidth + Padding, m_Height - 10.0f, 6.0f, pLabelText, -1.0f);
		TextRender()->TextColor(1, 1, 1, 1);
	}
}

void CHud::RenderLocalTime(float x)
{
	if(!g_Config.m_ClShowLocalTimeAlways)
		return;

	const bool Seconds = g_Config.m_TcShowLocalTimeSeconds; // TClient

	char aTimeStr[16];
	str_timestamp_format(aTimeStr, sizeof(aTimeStr), Seconds ? "%H:%M.%S" : "%H:%M");
	const float Width = std::round(TextRender()->TextBoundingBox(5.0f, aTimeStr).m_W);

	Graphics()->DrawRect(x - (Width + 15.0f), 0.0f, Width + 10.0f, 12.5f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_B, 3.75f);
	TextRender()->Text(x - (Width + 10.0f), (12.5f - 5.f) / 2.f, 5.0f, aTimeStr, -1.0f);

	// Graphics()->DrawRect(x - 30.0f, 0.0f, 25.0f, 12.5f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_B, 3.75f);
	// TextRender()->Text(x - 25.0f, (12.5f - 5.f) / 2.f, 5.0f, aTimeStr, -1.0f);
}

void CHud::OnNewSnapshot()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	if(!GameClient()->m_Snap.m_pGameInfoObj)
		return;

	int ClientId = -1;
	if(GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
		ClientId = GameClient()->m_Snap.m_LocalClientId;
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active)
		ClientId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;

	if(ClientId == -1)
		return;

	const CNetObj_Character *pPrevChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev;
	const CNetObj_Character *pCurChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
	const float IntraTick = Client()->IntraGameTick(g_Config.m_ClDummy);
	ivec2 Vel = mix(ivec2(pPrevChar->m_VelX, pPrevChar->m_VelY), ivec2(pCurChar->m_VelX, pCurChar->m_VelY), IntraTick);

	CCharacter *pChar = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
	if(pChar && pChar->IsGrounded())
		Vel.y = 0;

	int aVels[2] = {Vel.x, Vel.y};

	for(int i = 0; i < 2; i++)
	{
		int AbsVel = abs(aVels[i]);
		if(AbsVel > m_aPlayerSpeed[i])
		{
			m_aLastPlayerSpeedChange[i] = ESpeedChange::INCREASE;
		}
		if(AbsVel < m_aPlayerSpeed[i])
		{
			m_aLastPlayerSpeedChange[i] = ESpeedChange::DECREASE;
		}
		if(AbsVel < 2)
		{
			m_aLastPlayerSpeedChange[i] = ESpeedChange::NONE;
		}
		m_aPlayerSpeed[i] = AbsVel;
	}
}

void CHud::UpdateMapProgress()
{
	const bool Enabled = g_Config.m_TcMapProgress != 0 &&
		(Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK);
	const float DeltaSeconds = Client()->RenderFrameTime();
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		if(!Enabled)
		{
			m_aMapProgressState[Dummy].Reset();
			m_aMapProgressValid[Dummy] = false;
			m_aMapProgressDisplayed[Dummy] = 0.0f;
			continue;
		}

		const auto Estimate = GameClient()->m_TClient.MapProgressEstimate(Dummy);
		const int ClientId = GameClient()->m_TClient.MapProgressClientId(Dummy);
		m_aMapProgressValid[Dummy] = Estimate.m_Valid && ClientId >= 0;
		m_aMapProgressDisplayed[Dummy] = m_aMapProgressState[Dummy].Update(
			m_aMapProgressValid[Dummy], Estimate.m_Progress, DeltaSeconds, ClientId);
	}
}

void CHud::RenderMapProgressStandalone(int Dummy)
{
	if(!g_Config.m_TcMapProgress || Dummy < 0 || Dummy >= NUM_DUMMIES || m_Width <= 0.0f || m_Height <= 0.0f)
		return;

	const CScreenRect OldScreen = Graphics()->GetScreen();
	Graphics()->MapScreenToSize(m_Width, m_Height);
	const auto Layout = TClientHudMapProgress::FallbackTextLayout(
		m_Width, m_Height,
		(float)g_Config.m_TcMapProgressWidth,
		(float)g_Config.m_TcMapProgressHeight,
		(float)g_Config.m_TcMapProgressX,
		(float)g_Config.m_TcMapProgressY);
	char aValue[16];
	if(m_aMapProgressValid[Dummy])
		str_format(aValue, sizeof(aValue), "%.1f%%", m_aMapProgressDisplayed[Dummy] * 100.0f);
	else
		str_copy(aValue, "--");
	const ColorRGBA PreviousColor = TextRender()->GetTextColor();
	const ColorRGBA PreviousOutline = TextRender()->GetTextOutlineColor();
	const unsigned PreviousFlags = TextRender()->GetRenderFlags();
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT);
	TextRender()->TextOutlineColor(0.0f, 0.0f, 0.0f, 0.55f);
	TextRender()->TextColor(ColorRGBA(0.95f, 0.97f, 1.0f, 1.0f));
	const float TextWidth = TextRender()->TextWidth(Layout.m_FontSize, aValue);
	TextRender()->Text(std::round(Layout.m_Text.x + (Layout.m_Text.w - TextWidth) * 0.5f), Layout.m_Text.y, Layout.m_FontSize, aValue);
	TextRender()->TextColor(PreviousColor);
	TextRender()->TextOutlineColor(PreviousOutline);
	TextRender()->SetRenderFlags(PreviousFlags);
	Graphics()->MapScreen(OldScreen);
}

bool CHud::RenderMapProgressBelowBoard(const CUIRect &Board, float SafeBottom, int Dummy)
{
	if(!g_Config.m_TcMapProgress || Dummy < 0 || Dummy >= NUM_DUMMIES ||
		!TClientHudMapProgress::HasRoomBelowBoard(Board, SafeBottom))
		return false;

	const CUIRect Value = TClientHudMapProgress::BoardValue(Board);
	char aValue[16];
	if(m_aMapProgressValid[Dummy])
		str_format(aValue, sizeof(aValue), "%.1f%%", m_aMapProgressDisplayed[Dummy] * 100.0f);
	else
		str_copy(aValue, "--");
	const ColorRGBA PreviousColor = TextRender()->GetTextColor();
	const ColorRGBA PreviousOutline = TextRender()->GetTextOutlineColor();
	const unsigned PreviousFlags = TextRender()->GetRenderFlags();
	const ColorRGBA Configured = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcMapProgressColor));
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT);
	TextRender()->TextOutlineColor(0.0f, 0.0f, 0.0f, 0.55f);
	TextRender()->TextColor(ColorRGBA(Configured.r, Configured.g, Configured.b, 1.0f));
	const float TextWidth = TextRender()->TextWidth(Value.h, aValue);
	TextRender()->Text(Value.x + (Value.w - TextWidth) * 0.5f, Value.y, Value.h, aValue);
	TextRender()->TextColor(PreviousColor);
	TextRender()->TextOutlineColor(PreviousOutline);
	TextRender()->SetRenderFlags(PreviousFlags);
	return true;
}

bool CHud::GetMapProgressIslandValue(int Dummy, float &Progress) const
{
	if(Dummy < 0 || Dummy >= NUM_DUMMIES || Dummy != g_Config.m_ClDummy ||
		!TClientHudMapProgress::ShouldShowIslandValue(g_Config.m_TcMapProgress != 0, g_Config.m_TcMapProgressStyle,
			g_Config.m_TcIslandEnable != 0, m_aMapProgressValid[Dummy]))
		return false;
	Progress = m_aMapProgressDisplayed[Dummy];
	return true;
}

void CHud::OnRender()
{
	m_FrozenHudPending = false;
	m_SpectatorCountBoxShown = false;
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(!GameClient()->m_Snap.m_pGameInfoObj)
		return;

	m_Width = 300.0f * Graphics()->ScreenAspect();
	m_Height = 300.0f;
	Graphics()->MapScreenToSize(m_Width, m_Height);
	UpdateMapProgress();

#if defined(CONF_VIDEORECORDER)
	if((IVideo::Current() && g_Config.m_ClVideoShowhud) || (!IVideo::Current() && g_Config.m_ClShowhud))
#else
	if(g_Config.m_ClShowhud)
#endif
	{
		if(GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
		{
			if(g_Config.m_ClShowhudHealthAmmo)
			{
				RenderAmmoHealthAndArmor(GameClient()->m_Snap.m_pLocalCharacter);
			}
			if(GameClient()->m_Snap.m_aCharacters[GameClient()->m_Snap.m_LocalClientId].m_HasExtendedData && g_Config.m_ClShowhudDDRace && GameClient()->m_GameInfo.m_HudDDRace)
			{
				RenderPlayerState(GameClient()->m_Snap.m_LocalClientId);
			}
			RenderSpectatorCount();
			RenderMovementInformation();
			RenderDDRaceEffects();
		}
		else if(GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			int SpectatorId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;
			if(SpectatorId != SPEC_FREEVIEW && g_Config.m_ClShowhudHealthAmmo)
			{
				RenderAmmoHealthAndArmor(&GameClient()->m_Snap.m_aCharacters[SpectatorId].m_Cur);
			}
			if(SpectatorId != SPEC_FREEVIEW &&
				GameClient()->m_Snap.m_aCharacters[SpectatorId].m_HasExtendedData &&
				g_Config.m_ClShowhudDDRace &&
				(!GameClient()->m_MultiViewActivated || GameClient()->m_MultiViewShowHud) &&
				GameClient()->m_GameInfo.m_HudDDRace)
			{
				RenderPlayerState(SpectatorId);
			}
			RenderMovementInformation();
			RenderSpectatorHud();
		}

		// TClient: when the dynamic island is enabled it draws the round timer
		// inside itself (top-left of the capsule), so skip the top-centre timer
		// here to avoid drawing it underneath the island.
		if(g_Config.m_ClShowhudTimer && !g_Config.m_TcIslandEnable)
			RenderGameTimer();
		RenderPauseNotification();
		RenderSuddenDeath();
		if(g_Config.m_ClShowhudScore)
			RenderScoreHud();
		RenderDummyActions();
		RenderWarmupTimer();
		RenderTextInfo();
		GameClient()->m_TClient.RenderCenterLines();
		RenderLocalTime((m_Width / 7) * 3);
		if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
			RenderConnectionWarning();
		RenderTeambalanceWarning();
		GameClient()->m_Voting.Render();
		if(g_Config.m_ClShowRecord)
			RenderRecord();
		const auto DrownBoard = RenderDrownBoard();
		const int Dummy = g_Config.m_ClDummy;
		const bool HasBoardValue = DrownBoard && RenderMapProgressBelowBoard(DrownBoard->m_Bounds, DrownBoard->m_SafeBottom, Dummy);
		if(!HasBoardValue)
			RenderMapProgressStandalone(Dummy);
		}
		RenderCursor();
}

void CHud::OnMessage(int MsgType, void *pRawMsg)
{
	if(MsgType == NETMSGTYPE_SV_DDRACETIME || MsgType == NETMSGTYPE_SV_DDRACETIMELEGACY)
	{
		CNetMsg_Sv_DDRaceTime *pMsg = (CNetMsg_Sv_DDRaceTime *)pRawMsg;

		m_DDRaceTime = pMsg->m_Time;

		m_ShowFinishTime = pMsg->m_Finish != 0;

		if(!m_ShowFinishTime)
		{
			m_TimeCpDiff = (float)pMsg->m_Check / 100;
			m_TimeCpLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);
		}
		else
		{
			m_FinishTimeDiff = (float)pMsg->m_Check / 100;
			m_FinishTimeLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);
		}
	}
	else if(MsgType == NETMSGTYPE_SV_RECORD || MsgType == NETMSGTYPE_SV_RECORDLEGACY)
	{
		CNetMsg_Sv_Record *pMsg = (CNetMsg_Sv_Record *)pRawMsg;

		// NETMSGTYPE_SV_RACETIME on old race servers
		if(MsgType == NETMSGTYPE_SV_RECORDLEGACY && GameClient()->m_GameInfo.m_DDRaceRecordMessage)
		{
			m_DDRaceTime = pMsg->m_ServerTimeBest; // First value: m_Time

			m_FinishTimeLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);

			if(pMsg->m_PlayerTimeBest) // Second value: m_Check
			{
				m_TimeCpDiff = (float)pMsg->m_PlayerTimeBest / 100;
				m_TimeCpLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);
			}
		}
		else if(MsgType == NETMSGTYPE_SV_RECORD || GameClient()->m_GameInfo.m_RaceRecordMessage)
		{
			// ignore m_ServerTimeBest, it's handled by the game client
			m_aPlayerRecord[g_Config.m_ClDummy] = (float)pMsg->m_PlayerTimeBest / 100;
		}
	}
}

void CHud::RenderDDRaceEffects()
{
	if(m_DDRaceTime)
	{
		char aBuf[64];
		char aTime[32];
		if(m_ShowFinishTime && m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
		{
			str_time(m_DDRaceTime, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
			str_format(aBuf, sizeof(aBuf), "Finish time: %s", aTime);

			// calculate alpha (4 sec 1 than get lower the next 2 sec)
			float Alpha = 1.0f;
			if(m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 4 < Client()->GameTick(g_Config.m_ClDummy) && m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
			{
				// lower the alpha slowly to blend text out
				Alpha = ((float)(m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 6) - (float)Client()->GameTick(g_Config.m_ClDummy)) / (float)(Client()->GameTickSpeed() * 2);
			}

			TextRender()->TextColor(1, 1, 1, Alpha);
			CTextCursor Cursor;
			Cursor.SetPosition(vec2(150 * Graphics()->ScreenAspect() - TextRender()->TextWidth(12, aBuf) / 2, 20));
			Cursor.m_FontSize = 12.0f;
			TextRender()->RecreateTextContainer(m_DDRaceEffectsTextContainerIndex, &Cursor, aBuf);
			if(m_FinishTimeDiff != 0.0f && m_DDRaceEffectsTextContainerIndex.Valid())
			{
				if(m_FinishTimeDiff < 0)
				{
					str_time_float(-m_FinishTimeDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
					str_format(aBuf, sizeof(aBuf), "-%s", aTime);
					TextRender()->TextColor(0.5f, 1.0f, 0.5f, Alpha); // green
				}
				else
				{
					str_time_float(m_FinishTimeDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
					str_format(aBuf, sizeof(aBuf), "+%s", aTime);
					TextRender()->TextColor(1.0f, 0.5f, 0.5f, Alpha); // red
				}
				CTextCursor DiffCursor;
				DiffCursor.SetPosition(vec2(150 * Graphics()->ScreenAspect() - TextRender()->TextWidth(10, aBuf) / 2, 34));
				DiffCursor.m_FontSize = 10.0f;
				TextRender()->AppendTextContainer(m_DDRaceEffectsTextContainerIndex, &DiffCursor, aBuf);
			}
			if(m_DDRaceEffectsTextContainerIndex.Valid())
			{
				auto OutlineColor = TextRender()->DefaultTextOutlineColor();
				OutlineColor.a *= Alpha;
				TextRender()->RenderTextContainer(m_DDRaceEffectsTextContainerIndex, TextRender()->DefaultTextColor(), OutlineColor);
			}
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
		else if(g_Config.m_ClShowhudTimeCpDiff && !m_ShowFinishTime && m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
		{
			if(m_TimeCpDiff < 0)
			{
				str_time_float(-m_TimeCpDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
				str_format(aBuf, sizeof(aBuf), "-%s", aTime);
			}
			else
			{
				str_time_float(m_TimeCpDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
				str_format(aBuf, sizeof(aBuf), "+%s", aTime);
			}

			// calculate alpha (4 sec 1 than get lower the next 2 sec)
			float Alpha = 1.0f;
			if(m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 4 < Client()->GameTick(g_Config.m_ClDummy) && m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
			{
				// lower the alpha slowly to blend text out
				Alpha = ((float)(m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 6) - (float)Client()->GameTick(g_Config.m_ClDummy)) / (float)(Client()->GameTickSpeed() * 2);
			}

			if(m_TimeCpDiff > 0)
				TextRender()->TextColor(1.0f, 0.5f, 0.5f, Alpha); // red
			else if(m_TimeCpDiff < 0)
				TextRender()->TextColor(0.5f, 1.0f, 0.5f, Alpha); // green
			else if(!m_TimeCpDiff)
				TextRender()->TextColor(1, 1, 1, Alpha); // white

			CTextCursor Cursor;
			Cursor.SetPosition(vec2(150 * Graphics()->ScreenAspect() - TextRender()->TextWidth(10, aBuf) / 2, 20));
			Cursor.m_FontSize = 10.0f;
			TextRender()->RecreateTextContainer(m_DDRaceEffectsTextContainerIndex, &Cursor, aBuf);

			if(m_DDRaceEffectsTextContainerIndex.Valid())
			{
				auto OutlineColor = TextRender()->DefaultTextOutlineColor();
				OutlineColor.a *= Alpha;
				TextRender()->RenderTextContainer(m_DDRaceEffectsTextContainerIndex, TextRender()->DefaultTextColor(), OutlineColor);
			}
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
	}
}

void CHud::RenderRecord()
{
	if(GameClient()->m_MapBestTimeSeconds != FinishTime::UNSET && GameClient()->m_MapBestTimeSeconds != FinishTime::NOT_FINISHED_MILLIS)
	{
		char aBuf[64];
		TextRender()->Text(5, 75, 6, Localize("Server best:"), -1.0f);
		char aTime[32];
		int64_t TimeCentiseconds = static_cast<int64_t>(GameClient()->m_MapBestTimeSeconds) * 100 + static_cast<int64_t>(GameClient()->m_MapBestTimeMillis) / 10;
		str_time(TimeCentiseconds, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
		str_format(aBuf, sizeof(aBuf), "%s%s", GameClient()->m_MapBestTimeSeconds > 3600 ? "" : "   ", aTime);
		TextRender()->Text(53, 75, 6, aBuf, -1.0f);
	}

	if(GameClient()->m_ReceivedDDNetPlayerFinishTimes)
	{
		// [TClient] 审计修复（2026-11-20 P3）：dummy 未连接时 LocalId=-1，先守卫再索引
		const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
		if(LocalId >= 0 && LocalId < MAX_CLIENTS)
		{
			const int PlayerTimeSeconds = GameClient()->m_aClients[LocalId].m_FinishTimeSeconds;
			if(PlayerTimeSeconds != FinishTime::NOT_FINISHED_MILLIS)
			{
				char aBuf[64];
				TextRender()->Text(5, 82, 6, Localize("Personal best:"), -1.0f);
				char aTime[32];
				const int PlayerTimeMillis = GameClient()->m_aClients[LocalId].m_FinishTimeMillis;
				int64_t TimeCentiseconds = static_cast<int64_t>(PlayerTimeSeconds) * 100 + static_cast<int64_t>(PlayerTimeMillis) / 10;
				str_time(TimeCentiseconds, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
				str_format(aBuf, sizeof(aBuf), "%s%s", PlayerTimeSeconds > 3600 ? "" : "   ", aTime);
				TextRender()->Text(53, 82, 6, aBuf, -1.0f);
			}
		}
	}
	else
	{
		const float PlayerRecord = m_aPlayerRecord[g_Config.m_ClDummy];
		if(PlayerRecord > 0.0f)
		{
			char aBuf[64];
			TextRender()->Text(5, 82, 6, Localize("Personal best:"), -1.0f);
			char aTime[32];
			str_time_float(PlayerRecord, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
			str_format(aBuf, sizeof(aBuf), "%s%s", PlayerRecord > 3600 ? "" : "   ", aTime);
			TextRender()->Text(53, 82, 6, aBuf, -1.0f);
		}
	}
}

std::optional<CHud::SDrownBoardPlacement> CHud::RenderDrownBoard()
{
	if(!g_Config.m_TcDrownBoard || !GameClient()->m_TClient.IsDrownBoardMode())
		return std::nullopt;
	const float BoardAlpha = std::clamp(g_Config.m_TcDrownBoardOpacity / 100.0f, 0.0f, 1.0f);
	if(BoardAlpha <= 0.0f)
		return std::nullopt;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy] >= 0 ? GameClient()->m_aLocalIds[g_Config.m_ClDummy] : GameClient()->m_Snap.m_LocalClientId;
	const bool HasLocalClient = LocalId >= 0 && LocalId < MAX_CLIENTS && GameClient()->m_aClients[LocalId].m_Active && GameClient()->m_Snap.m_apPlayerInfos[LocalId] != nullptr;
	const int LocalTeam = HasLocalClient ? GameClient()->m_Teams.Team(LocalId) : -1;
	const bool IncludeTeamZero = g_Config.m_TcDrownBoardIncludeTeam0 != 0;
	if(!CTClientDrownBoard::IsBoardVisible(GameClient()->m_TClient.IsDrownBoardMode(), HasLocalClient, LocalTeam, IncludeTeamZero))
		return std::nullopt;

	// 列定义：位掩码 → 列（矢量小图标 + 专属配色，参考 Nova 精简统计板）。除落水外
	// 均为客户端估算（判据见 docs 落水榜多列语义 2026-11-19）。
	struct SColumn
	{
		int m_Bit;
		ColorRGBA m_Color;
		bool m_Time;    // tick 换算秒
		bool m_Percent; // Eff 百分比
	};
	const int ColumnsMask = g_Config.m_TcDrownBoardColumns == 0 ? 1 : g_Config.m_TcDrownBoardColumns;
	const SColumn aAllColumns[] = {
		{0, ColorRGBA(0.247f, 0.776f, 0.831f, 1.0f), false, false}, // 落水：水色雪花
		{1, ColorRGBA(0.95f, 0.36f, 0.36f, 1.0f), false, false}, // 死亡：红骷髅
		{2, ColorRGBA(0.45f, 0.85f, 0.45f, 1.0f), false, false}, // 救援：绿锤
		{3, ColorRGBA(0.96f, 0.82f, 0.32f, 1.0f), false, false}, // 坚持：金星
		{4, ColorRGBA(0.96f, 0.55f, 0.28f, 1.0f), false, false}, // 全灭：橙骷髅+线
		{5, ColorRGBA(0.55f, 0.85f, 0.60f, 1.0f), false, false}, // 跳跃：草绿双箭头
		{6, ColorRGBA(0.55f, 0.68f, 0.95f, 1.0f), false, false}, // 出钩：天蓝钩形
		{7, ColorRGBA(0.75f, 0.60f, 0.95f, 1.0f), false, false}, // 钩人：紫钩+目标点
		{8, ColorRGBA(0.92f, 0.92f, 0.92f, 1.0f), true, false}, // 存活：白时钟
		{9, ColorRGBA(0.60f, 0.78f, 0.95f, 1.0f), true, false}, // 冻结时长：冰蓝冻结钟
		{10, ColorRGBA(0.85f, 0.85f, 0.85f, 1.0f), false, true}, // 效率：灰 %
	};
	std::vector<SColumn> vColumns;
	int SortBit = 0;
	for(const SColumn &Column : aAllColumns)
	{
		if(!(ColumnsMask & (1 << Column.m_Bit)))
			continue;
		if(vColumns.empty())
			SortBit = Column.m_Bit;
		vColumns.push_back(Column);
	}
	if(vColumns.empty())
		vColumns.push_back(aAllColumns[0]);

	const auto ColumnValueAt = [](int Bit, const CTClientDrownBoard::SPlayerStats &Stats) -> int {
		switch(Bit)
		{
		case 0: return Stats.m_Freezes;
		case 1: return Stats.m_Deaths;
		case 2: return Stats.m_Saves;
		case 3: return Stats.m_Lasts;
		case 4: return Stats.m_Aleds;
		case 5: return Stats.m_Jumps;
		case 6: return Stats.m_HooksLaunched;
		case 7: return Stats.m_PlayerHooks;
		case 8: return Stats.m_TicksAlive;
		case 9: return Stats.m_TicksFrozen;
		default: return (100 * Stats.m_Saves) / std::max(1, Stats.m_Saves + Stats.m_Freezes);
		}
	};

	struct SEntry
	{
		int m_ClientId;
		CTClientDrownBoard::SPlayerStats m_Stats;
	};
	std::vector<SEntry> vEntries;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!GameClient()->m_aClients[ClientId].m_Active ||
			GameClient()->m_Snap.m_apPlayerInfos[ClientId] == nullptr ||
			!CTClientDrownBoard::IsSameTrackedTeam(LocalTeam, GameClient()->m_Teams.Team(ClientId), IncludeTeamZero))
			continue;
		vEntries.push_back({ClientId, GameClient()->m_TClient.GetDrownStats(ClientId)});
	}
	if(vEntries.empty())
		return std::nullopt;

	std::stable_sort(vEntries.begin(), vEntries.end(), [&](const SEntry &Left, const SEntry &Right) {
		const int LeftValue = ColumnValueAt(SortBit, Left.m_Stats);
		const int RightValue = ColumnValueAt(SortBit, Right.m_Stats);
		if(LeftValue != RightValue)
			return LeftValue > RightValue;
		return Left.m_ClientId < Right.m_ClientId;
	});

	const int MaxRows = std::clamp(g_Config.m_TcDrownBoardMaxPlayers, 1, 16);
	const int RowCount = std::min((int)vEntries.size(), MaxRows);
	const bool HasMoreRows = (int)vEntries.size() > RowCount;
	const bool ShowTee = g_Config.m_TcDrownBoardShowTee != 0;
	const char *pTitle = TCLocalize("Drown board");
	const int LeaderValue = ColumnValueAt(SortBit, vEntries.front().m_Stats);

	// 设计：深水面板 + 表头一行小字 + 数值列右对齐。首个启用列即排序列，榜首
	// 用 aqua 水色；标题下一条渐隐"水面线"，仅此一处装饰。右缘贴边 + 左圆角，
	// 与下方观战名字框同款语言。
	constexpr float TitleFontSize = 4.0f;
	constexpr float RowFontSize = 5.0f;
	constexpr float ValueFontSize = 5.0f;
	constexpr float MoreFontSize = 4.5f;
	constexpr float PaddingX = 3.0f;
	constexpr float PaddingY = 3.0f;
	constexpr float TitleHeight = TitleFontSize + 2.5f;
	constexpr float IconSize = 5.5f;
	constexpr float HeaderHeight = IconSize + 1.0f;
	constexpr float RowHeight = 6.5f;
	constexpr float MoreHeight = MoreFontSize + 1.5f;
	constexpr float TeeSize = 6.0f;
	constexpr float TeeGap = 1.5f;
	constexpr float ColumnGap = 2.0f;
	constexpr float NameBudget = 34.0f; // 名字列固定预算：超宽按 UTF-8 边界省略号截断，面板不随名字变宽
	constexpr float WaterlineWidth = 10.0f;
	const ColorRGBA WaterlineColor(0.247f, 0.776f, 0.831f, 1.0f);
	const float TeeColumnWidth = ShowTee ? TeeSize + TeeGap : 0.0f;

	const auto FormatCell = [&](const SColumn &Column, const CTClientDrownBoard::SPlayerStats &Stats, char *pBuf, size_t BufSize) {
		if(Column.m_Time)
			str_format(pBuf, BufSize, "%ds", ColumnValueAt(Column.m_Bit, Stats) / Client()->GameTickSpeed());
		else if(Column.m_Percent)
			str_format(pBuf, BufSize, "%d%%", ColumnValueAt(Column.m_Bit, Stats));
		else
			str_format(pBuf, BufSize, "%d", ColumnValueAt(Column.m_Bit, Stats));
	};

	// 表头图标：Font Awesome 字形（DDNet 字体系统自带 FA6 Free，SIL OFL 许可；
	// 与 Nova 同源做法）。钩人用 Noto Emoji 的 🪝（走默认字体 emoji 回退链）。
	// 全部码点已用 fontTools 对照字体 cmap 校验存在，视觉样张见
	// D:\kehuduan\NovaClient-2.2.9.1-分析\glyph-preview.png。
	const auto DrawIcon = [&](int Bit, float X, float Y, float S, const ColorRGBA &Color) {
		struct SGlyphSpec
		{
			const char *m_pUtf8;
			bool m_IconFace;
		};
		static const SGlyphSpec aGlyphs[11] = {
			{"\xEF\x8B\x9C", true}, // f2dc snowflake  落水次数
			{"\xEF\x95\x8C", true}, // f54c skull      死亡
			{"\xEF\x9B\xA3", true}, // f6e3 hammer     救援次数
			{"\xEF\x80\x85", true}, // f005 star       坚持次数
			{"\xEF\x9B\xA2", true}, // f6e2 ghost      全灭次数
			{"\xEF\x9C\x8C", true}, // f70c person-running 跳跃次数
			{"\xEF\x84\xBD", true}, // f13d anchor     出钩次数
			{"\xF0\x9F\xAA\x9D", false}, // 1fa9d hook 钩人次数
			{"\xEF\x88\x9E", true}, // f21e heart-pulse 存活时间
			{"\xEF\x9E\xAD", true}, // f7ad icicles    冻结时间
			{"\xEF\x8A\x95", true}, // f295 percent    效率
		};
		TextRender()->SetFontPreset(aGlyphs[Bit].m_IconFace ? EFontPreset::ICON_FONT : EFontPreset::DEFAULT_FONT);
		TextRender()->TextColor(Color.r, Color.g, Color.b, Color.a);
		const float GlyphWidth = TextRender()->TextWidth(S, aGlyphs[Bit].m_pUtf8);
		TextRender()->Text(X + (S - GlyphWidth) / 2.0f, Y, S, aGlyphs[Bit].m_pUtf8);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	};

	// 名字按固定预算截断：从尾部按 UTF-8 边界回退，补省略号（面板不随长名变宽）。
	const auto EllipsizeName = [&](const char *pText, char *pBuf, size_t BufSize) {
		str_copy(pBuf, pText, BufSize);
		if(TextRender()->TextWidth(RowFontSize, pBuf) <= NameBudget)
			return;
		const float DotWidth = TextRender()->TextWidth(RowFontSize, "…");
		int Len = str_length(pBuf);
		while(Len > 0)
		{
			--Len;
			while(Len > 0 && (pBuf[Len] & 0xC0) == 0x80)
				--Len; // UTF-8 续字节，不在多字节字符中间切断
			pBuf[Len] = '\0';
			if(TextRender()->TextWidth(RowFontSize, pBuf) + DotWidth <= NameBudget)
				break;
		}
		str_append(pBuf, "…", BufSize);
	};

	std::vector<float> vColumnWidth(vColumns.size(), 0.0f);
	for(size_t ColumnIndex = 0; ColumnIndex < vColumns.size(); ++ColumnIndex)
	{
		const SColumn &Column = vColumns[ColumnIndex];
		float WidthMax = IconSize + 1.5f;
		for(int Index = 0; Index < RowCount; ++Index)
		{
			char aCell[16];
			FormatCell(Column, vEntries[Index].m_Stats, aCell, sizeof(aCell));
			WidthMax = std::max(WidthMax, TextRender()->TextWidth(ValueFontSize, aCell));
		}
		vColumnWidth[ColumnIndex] = WidthMax;
	}

	float ColumnsWidth = 0.0f;
	for(float Width : vColumnWidth)
		ColumnsWidth += Width + ColumnGap;

	float BoardWidth = PaddingX * 2.0f + TeeColumnWidth + NameBudget + ColumnsWidth;
	BoardWidth = std::max(BoardWidth, PaddingX * 2.0f + TextRender()->TextWidth(TitleFontSize, pTitle));
	BoardWidth = std::clamp(BoardWidth, 50.0f, m_Width * 0.5f);
	const float BoardHeight = PaddingY * 2.0f + TitleHeight + HeaderHeight + RowCount * RowHeight + (HasMoreRows ? MoreHeight : 0.0f);

	// 与 RenderSpectatorCount 的右下角堆叠链保持一致，落在观战名字框上方
	float Anchor = 285.0f - 4.0f;
	if(g_Config.m_ClShowhudPlayerPosition || g_Config.m_ClShowhudPlayerSpeed || g_Config.m_ClShowhudPlayerAngle)
		Anchor -= 4.0f;
	Anchor -= GetMovementInformationBoxHeight();
	if(g_Config.m_ClShowhudScore)
		Anchor -= 56.0f;
	if(g_Config.m_ClShowhudDummyActions && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) && Client()->DummyConnected())
		Anchor -= 29.0f + 4.0f;
	float BoardBottom = Anchor - 6.0f;
	if(m_SpectatorCountBoxShown)
		BoardBottom -= m_SpectatorCountBoxHeight + 4.0f;
	const float SafeBottom = BoardBottom;
	if(g_Config.m_TcMapProgress)
		BoardBottom -= TClientHudMapProgress::BOARD_VALUE_GAP + TClientHudMapProgress::BOARD_VALUE_HEIGHT + TClientHudMapProgress::BOARD_VALUE_BOTTOM_PADDING;
	const float BoardX = m_Width - BoardWidth;
	const float BoardY = std::max(BoardBottom - BoardHeight, 32.0f);
	const CUIRect BoardBounds{BoardX, BoardY, BoardWidth, BoardHeight};

	const ColorRGBA PreviousColor = TextRender()->GetTextColor();
	const ColorRGBA PreviousOutline = TextRender()->GetTextOutlineColor();

	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->DrawRectExt4(BoardX, BoardY, BoardWidth, BoardHeight,
		ColorRGBA(0.047f, 0.157f, 0.188f, 0.66f * BoardAlpha),
		ColorRGBA(0.047f, 0.157f, 0.188f, 0.66f * BoardAlpha),
		ColorRGBA(0.020f, 0.059f, 0.082f, 0.74f * BoardAlpha),
		ColorRGBA(0.020f, 0.059f, 0.082f, 0.74f * BoardAlpha),
		5.0f, IGraphics::CORNER_L);
	const float WaterY = BoardY + PaddingY + TitleFontSize + 1.0f;
	Graphics()->DrawRectExt4(BoardX + PaddingX, WaterY, WaterlineWidth, 1.2f,
		ColorRGBA(WaterlineColor.r, WaterlineColor.g, WaterlineColor.b, 0.85f * BoardAlpha),
		ColorRGBA(WaterlineColor.r, WaterlineColor.g, WaterlineColor.b, 0.0f),
		ColorRGBA(WaterlineColor.r, WaterlineColor.g, WaterlineColor.b, 0.85f * BoardAlpha),
		ColorRGBA(WaterlineColor.r, WaterlineColor.g, WaterlineColor.b, 0.0f),
		0.6f, IGraphics::CORNER_ALL);
	Graphics()->QuadsEnd();

	TextRender()->TextColor(TextRender()->DefaultTextColor());
	TextRender()->TextOutlineColor(TextRender()->DefaultTextOutlineColor());

	const float RowLeft = BoardX + PaddingX;
	const float HeaderY = BoardY + PaddingY + TitleHeight;
	std::vector<float> vColumnX(vColumns.size(), 0.0f);
	float NextColumnX = RowLeft + TeeColumnWidth + NameBudget + ColumnGap;
	for(size_t ColumnIndex = 0; ColumnIndex < vColumns.size(); ++ColumnIndex)
	{
		vColumnX[ColumnIndex] = NextColumnX;
		NextColumnX += vColumnWidth[ColumnIndex] + ColumnGap;
	}

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.62f);
	TextRender()->Text(RowLeft, BoardY + PaddingY, TitleFontSize, pTitle);
	for(size_t ColumnIndex = 0; ColumnIndex < vColumns.size(); ++ColumnIndex)
	{
		const SColumn &Column = vColumns[ColumnIndex];
		DrawIcon(Column.m_Bit, vColumnX[ColumnIndex] + (vColumnWidth[ColumnIndex] - IconSize) / 2.0f, HeaderY + (HeaderHeight - IconSize) / 2.0f, IconSize, Column.m_Color);
	}

	for(int Index = 0; Index < RowCount; ++Index)
	{
		const int ClientId = vEntries[Index].m_ClientId;
		char aName[64];
		EllipsizeName(GameClient()->m_aClients[ClientId].m_aName, aName, sizeof(aName));
		const int RowValue = ColumnValueAt(SortBit, vEntries[Index].m_Stats);
		const bool IsLeader = Index == 0 && LeaderValue > 0;
		const float Y = HeaderY + HeaderHeight + Index * RowHeight;

		if(ShowTee)
		{
			CTeeRenderInfo TeeInfo = GameClient()->m_aClients[ClientId].m_RenderInfo;
			TeeInfo.m_Size = TeeSize;
			vec2 OffsetToMid;
			CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &TeeInfo, OffsetToMid);
			RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), vec2(RowLeft + TeeSize / 2.0f, Y + RowHeight / 2.0f + OffsetToMid.y), BoardAlpha);
		}

		if(IsLeader)
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.95f);
		else if(RowValue > 0)
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.85f);
		else
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.55f);
		TextRender()->Text(RowLeft + TeeColumnWidth, Y + (RowHeight - RowFontSize) / 2.0f, RowFontSize, aName, NameBudget);

		for(size_t ColumnIndex = 0; ColumnIndex < vColumns.size(); ++ColumnIndex)
		{
			const SColumn &Column = vColumns[ColumnIndex];
			char aCell[16];
			FormatCell(Column, vEntries[Index].m_Stats, aCell, sizeof(aCell));
			const int CellValue = ColumnValueAt(Column.m_Bit, vEntries[Index].m_Stats);
			TextRender()->TextColor(Column.m_Color.r, Column.m_Color.g, Column.m_Color.b,
				(IsLeader && Column.m_Bit == SortBit) ? 1.0f : (CellValue > 0 ? 0.88f : 0.38f));
			TextRender()->Text(vColumnX[ColumnIndex] + (vColumnWidth[ColumnIndex] - TextRender()->TextWidth(ValueFontSize, aCell)) / 2.0f,
				Y + RowHeight / 2.0f - ValueFontSize / 2.0f, ValueFontSize, aCell);
		}
	}
	if(HasMoreRows)
	{
		char aMore[16];
		str_format(aMore, sizeof(aMore), "+%d", (int)vEntries.size() - RowCount);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.55f);
		TextRender()->Text(RowLeft, HeaderY + HeaderHeight + RowCount * RowHeight + (MoreHeight - MoreFontSize) / 2.0f, MoreFontSize, aMore);
	}

	TextRender()->TextColor(PreviousColor);
	TextRender()->TextOutlineColor(PreviousOutline);
	return SDrownBoardPlacement{BoardBounds, SafeBottom};
}

