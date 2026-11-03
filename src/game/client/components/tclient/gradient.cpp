/* Copyright © 2026 BestProject Team (MIT); adapted for TClient. */
#include "gradient.h"

#include "gradient_math.h"

#include <base/math.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/ui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
	ColorRGBA ConfigColor(unsigned ConfigValue)
	{
		return color_cast<ColorRGBA>(ColorHSLA(ConfigValue, true));
	}

	std::array<ColorRGBA, 4> CustomColors()
	{
		return {
			ConfigColor(g_Config.m_TcGradientColor1),
			ConfigColor(g_Config.m_TcGradientColor2),
			ConfigColor(g_Config.m_TcGradientColor3),
			ConfigColor(g_Config.m_TcGradientColor4)};
	}
} // namespace

bool CTClientGradient::AppliesTo(int ClientId, const CGameClient *pGameClient)
{
	const auto Target = static_cast<ETClientGradientTarget>(g_Config.m_TcGradientTarget);
	if(Target == ETClientGradientTarget::ALL || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return true;

	bool Own = false;
	if(pGameClient->Client()->State() == IClient::STATE_DEMOPLAYBACK)
	{
		const int OwnId = pGameClient->m_Snap.m_SpecInfo.m_Active ? pGameClient->m_Snap.m_SpecInfo.m_SpectatorId : pGameClient->m_Snap.m_LocalClientId;
		Own = ClientId == OwnId;
	}
	else
	{
		for(const int LocalId : pGameClient->m_aLocalIds)
		{
			if(ClientId == LocalId)
			{
				Own = true;
				break;
			}
		}
	}

	return Own == (Target == ETClientGradientTarget::OWN);
}

float CTClientGradient::AnimatePhase(double GlobalTime, int Speed)
{
	return TClientGradient::AnimatePhase(GlobalTime, Speed);
}

float CTClientGradient::AnimatePhase(double GlobalTime)
{
	return AnimatePhase(GlobalTime, g_Config.m_TcGradientAnimateSpeed);
}

int CTClientGradient::PhaseBucket(float Phase)
{
	return TClientGradient::PhaseBucket(Phase);
}

ColorRGBA CTClientGradient::SampleRainbow(float Hue)
{
	return TClientGradient::SampleRainbow(Hue);
}

ColorRGBA CTClientGradient::SampleColorStops(const ColorRGBA *pColors, int Count, float Position)
{
	return TClientGradient::SampleColorStops(pColors, Count, Position);
}

void CTClientGradient::GetSkinToneColors(int ClientId, const CGameClient *pGameClient, ColorRGBA &Body, ColorRGBA &Feet)
{
	Body = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	Feet = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;

	const CTClientGradient &Gradient = pGameClient->m_Gradient;
	if(g_Config.m_TcGradientSkin && Gradient.m_aHasOriginal[ClientId])
	{
		Body = Gradient.m_aOriginalBody[ClientId];
		Feet = Gradient.m_aOriginalFeet[ClientId];
		return;
	}

	const CTeeRenderInfo &RenderInfo = pGameClient->m_aClients[ClientId].m_RenderInfo;
	if(RenderInfo.m_CustomColoredSkin)
	{
		Body = RenderInfo.m_ColorBody;
		Feet = RenderInfo.m_ColorFeet;
	}
	else
	{
		Body = RenderInfo.m_BloodColor;
		Feet = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	}
}

void CTClientGradient::GetAnimatedEndpointColors(int ClientId, const CGameClient *pGameClient, float Phase, ColorRGBA &Color1, ColorRGBA &Color2)
{
	switch(static_cast<ETClientGradientMode>(g_Config.m_TcGradientMode))
	{
	case ETClientGradientMode::CUSTOM:
	{
		const std::array<ColorRGBA, 4> aColors = CustomColors();
		const int Count = std::clamp(g_Config.m_TcGradientColorCount, 2, 4);
		Color1 = SampleColorStops(aColors.data(), Count, Phase);
		Color2 = SampleColorStops(aColors.data(), Count, Phase + 0.5f);
		break;
	}
	case ETClientGradientMode::RAINBOW:
		Color1 = SampleRainbow(Phase);
		Color2 = SampleRainbow(Phase + 0.33f);
		break;
	case ETClientGradientMode::SKIN:
	default:
	{
		ColorRGBA Body;
		ColorRGBA Feet;
		GetSkinToneColors(ClientId, pGameClient, Body, Feet);
		const float Wave = 0.5f + 0.5f * std::cos(2.0f * pi * Phase);
		Color1 = TClientGradient::MixColor(Body, Feet, Wave);
		Color2 = Feet;
		break;
	}
	}
}

std::vector<STextColorSplit> CTClientGradient::BuildAnimatedColorSplits(const char *pText, const ColorRGBA &Color1, const ColorRGBA &Color2, float Phase, int Length)
{
	return TClientGradient::BuildAnimatedColorSplits(pText, Color1, Color2, Phase, Length);
}

std::vector<STextColorSplit> CTClientGradient::BuildAnimatedTextSplits(const char *pText, int ClientId, CGameClient *pGameClient, float Phase, int Length)
{
	switch(static_cast<ETClientGradientMode>(g_Config.m_TcGradientMode))
	{
	case ETClientGradientMode::CUSTOM:
	{
		const std::array<ColorRGBA, 4> aColors = CustomColors();
		const int Count = std::clamp(g_Config.m_TcGradientColorCount, 2, 4);
		return TClientGradient::BuildCustomColorSplits(pText, aColors.data(), Count, Phase, Length);
	}
	case ETClientGradientMode::RAINBOW:
		return TClientGradient::BuildRainbowColorSplits(pText, Phase, Length);
	case ETClientGradientMode::SKIN:
	default:
	{
		ColorRGBA Body;
		ColorRGBA Feet;
		GetSkinToneColors(ClientId, pGameClient, Body, Feet);
		return BuildAnimatedColorSplits(pText, Body, Feet, Phase, Length);
	}
	}
}

void CTClientGradient::ApplySixupColors(CTeeRenderInfo &RenderInfo, const ColorRGBA &Body, const ColorRGBA &Feet)
{
	TClientGradient::ApplySixupColors(RenderInfo, Body, Feet);
}

void CTClientGradient::TextColorSplitCallback(CTextCursor *pCursor, const char *pText, int Length, void *pUserData)
{
	static_cast<CTClientGradient *>(pUserData)->ApplyEverythingGradient(pCursor, pText, Length);
}

void CTClientGradient::ApplyEverythingGradient(CTextCursor *pCursor, const char *pText, int Length)
{
	if(!g_Config.m_TcGradientEverything || pCursor == nullptr || pText == nullptr || Length <= 0 || !pCursor->m_vColorSplits.empty())
		return;

	int ClientId = GameClient()->m_Snap.m_LocalClientId;
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK && GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId >= 0)
		ClientId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;

	const int CharacterOffset = pCursor->m_CharCount;
	const float Phase = AnimatePhase(Client()->GlobalTime());
	const std::vector<STextColorSplit> vSplits = BuildAnimatedTextSplits(pText, ClientId, GameClient(), Phase, Length);
	pCursor->m_vColorSplits.reserve(vSplits.size());
	for(const STextColorSplit &Split : vSplits)
		pCursor->m_vColorSplits.emplace_back(CharacterOffset + Split.m_CharIndex, Split.m_Length, Split.m_Color);
}

void CTClientGradient::RefreshCachedText()
{
	Ui()->OnElementsReset();
	GameClient()->m_Chat.RebuildChat();
	GameClient()->m_NamePlates.ResetNamePlates();
	GameClient()->m_Hud.ResetHudContainers();
	GameClient()->m_Scoreboard.ResetTexts();
	GameClient()->m_InfoMessages.OnWindowResize();
	GameClient()->m_Broadcast.OnWindowResize();
}

void CTClientGradient::UpdateConfigState()
{
	const bool Changed = m_ConfigInitialized &&
			     (m_LastName != g_Config.m_TcGradientName ||
				     m_LastClan != g_Config.m_TcGradientClan ||
				     m_LastSkin != g_Config.m_TcGradientSkin ||
				     m_LastEverything != g_Config.m_TcGradientEverything ||
				     m_LastMode != g_Config.m_TcGradientMode ||
				     m_LastTarget != g_Config.m_TcGradientTarget ||
				     m_LastAnimateSpeed != g_Config.m_TcGradientAnimateSpeed ||
				     m_LastColorCount != g_Config.m_TcGradientColorCount ||
				     m_LastColor1 != g_Config.m_TcGradientColor1 ||
				     m_LastColor2 != g_Config.m_TcGradientColor2 ||
				     m_LastColor3 != g_Config.m_TcGradientColor3 ||
				     m_LastColor4 != g_Config.m_TcGradientColor4);

	m_LastName = g_Config.m_TcGradientName;
	m_LastClan = g_Config.m_TcGradientClan;
	m_LastSkin = g_Config.m_TcGradientSkin;
	m_LastEverything = g_Config.m_TcGradientEverything;
	m_LastMode = g_Config.m_TcGradientMode;
	m_LastTarget = g_Config.m_TcGradientTarget;
	m_LastAnimateSpeed = g_Config.m_TcGradientAnimateSpeed;
	m_LastColorCount = g_Config.m_TcGradientColorCount;
	m_LastColor1 = g_Config.m_TcGradientColor1;
	m_LastColor2 = g_Config.m_TcGradientColor2;
	m_LastColor3 = g_Config.m_TcGradientColor3;
	m_LastColor4 = g_Config.m_TcGradientColor4;
	m_ConfigInitialized = true;

	if(Changed)
		RefreshCachedText();
}

bool CTClientGradient::LegacyRainbowActive(int ClientId) const
{
	if(!g_Config.m_TcRainbowTees || g_Config.m_TcRainbowMode == 0)
		return false;
	if(GameClient()->m_Snap.m_LocalClientId == ClientId)
		return true;
	return g_Config.m_TcRainbowOthers != 0;
}

void CTClientGradient::ReapplyLegacyRainbow(int ClientId)
{
	if(!g_Config.m_TcRainbowTees || g_Config.m_TcRainbowMode == 0)
		return;
	const bool Local = GameClient()->m_Snap.m_LocalClientId == ClientId;
	if(!Local && !g_Config.m_TcRainbowOthers)
		return;

	CTeeRenderInfo &RenderInfo = GameClient()->m_aClients[ClientId].m_RenderInfo;
	RenderInfo.m_BloodColor = GameClient()->m_Rainbow.m_RainbowColor;
	RenderInfo.m_ColorBody = GameClient()->m_Rainbow.m_RainbowColor;
	RenderInfo.m_ColorFeet = GameClient()->m_Rainbow.m_RainbowColor;
	RenderInfo.m_CustomColoredSkin = true;
}

void CTClientGradient::OnInit()
{
	TextRender()->SetTextColorSplitCallback(TextColorSplitCallback, this);
}

void CTClientGradient::OnShutdown()
{
	TextRender()->SetTextColorSplitCallback(nullptr, nullptr);
}

void CTClientGradient::OnRender()
{
	UpdateConfigState();
	m_aHasOriginal.fill(false);

	const bool SkinEnabled = g_Config.m_TcGradientSkin != 0;
	const float Phase = AnimatePhase(Client()->GlobalTime());
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const bool WasModified = m_aModified[ClientId];
		const bool ShouldApply = SkinEnabled && GameClient()->m_Snap.m_aCharacters[ClientId].m_Active && AppliesTo(ClientId, GameClient());
		if(!WasModified && !ShouldApply)
			continue;

		GameClient()->m_aClients[ClientId].UpdateRenderInfo();
		m_aModified[ClientId] = false;
		if(!ShouldApply)
		{
			ReapplyLegacyRainbow(ClientId);
			continue;
		}

		CTeeRenderInfo &RenderInfo = GameClient()->m_aClients[ClientId].m_RenderInfo;
		if(RenderInfo.m_CustomColoredSkin)
		{
			m_aOriginalBody[ClientId] = RenderInfo.m_ColorBody;
			m_aOriginalFeet[ClientId] = RenderInfo.m_ColorFeet;
		}
		else
		{
			m_aOriginalBody[ClientId] = RenderInfo.m_BloodColor;
			m_aOriginalFeet[ClientId] = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
		}
		m_aHasOriginal[ClientId] = true;

		// 旧彩虹功能仍作用于该玩家时让位：tee 显示彩虹，文字渐变继续用基础肤色。
		if(LegacyRainbowActive(ClientId))
		{
			ReapplyLegacyRainbow(ClientId);
			m_aModified[ClientId] = true;
			continue;
		}

		ColorRGBA Body;
		ColorRGBA Feet;
		GetAnimatedEndpointColors(ClientId, GameClient(), Phase, Body, Feet);
		if(static_cast<ETClientGradientMode>(g_Config.m_TcGradientMode) != ETClientGradientMode::SKIN)
		{
			Body.a *= m_aOriginalBody[ClientId].a;
			Feet.a *= m_aOriginalFeet[ClientId].a;
		}
		RenderInfo.m_ColorBody = Body;
		RenderInfo.m_ColorFeet = Feet;
		RenderInfo.m_CustomColoredSkin = true;
		ApplySixupColors(RenderInfo, Body, Feet);
		m_aModified[ClientId] = true;
	}
}
