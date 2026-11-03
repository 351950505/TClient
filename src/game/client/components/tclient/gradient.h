/* Copyright © 2026 BestProject Team (MIT); adapted for TClient. */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_GRADIENT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_GRADIENT_H

#include <base/color.h>

#include <engine/shared/protocol.h>
#include <engine/textrender.h>

#include <game/client/component.h>

#include <array>
#include <vector>

class CGameClient;
class CTeeRenderInfo;

enum class ETClientGradientMode
{
	SKIN = 0,
	CUSTOM,
	RAINBOW,
};

enum class ETClientGradientTarget
{
	OWN = 0,
	OTHERS,
	ALL,
};

class CTClientGradient : public CComponent
{
	std::array<ColorRGBA, MAX_CLIENTS> m_aOriginalBody;
	std::array<ColorRGBA, MAX_CLIENTS> m_aOriginalFeet;
	std::array<bool, MAX_CLIENTS> m_aHasOriginal = {};
	std::array<bool, MAX_CLIENTS> m_aModified = {};

	bool m_ConfigInitialized = false;
	int m_LastName = 0;
	int m_LastClan = 0;
	int m_LastSkin = 0;
	int m_LastEverything = 0;
	int m_LastMode = 0;
	int m_LastTarget = 0;
	int m_LastAnimateSpeed = 0;
	int m_LastColorCount = 0;
	unsigned m_LastColor1 = 0;
	unsigned m_LastColor2 = 0;
	unsigned m_LastColor3 = 0;
	unsigned m_LastColor4 = 0;

	void RefreshCachedText();
	void UpdateConfigState();
	bool LegacyRainbowActive(int ClientId) const;
	void ReapplyLegacyRainbow(int ClientId);
	void ApplyEverythingGradient(CTextCursor *pCursor, const char *pText, int Length);
	static void TextColorSplitCallback(CTextCursor *pCursor, const char *pText, int Length, void *pUserData);

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnShutdown() override;
	void OnRender() override;

	static bool AppliesTo(int ClientId, const CGameClient *pGameClient);
	static float AnimatePhase(double GlobalTime, int Speed);
	static float AnimatePhase(double GlobalTime);
	static int PhaseBucket(float Phase);
	static ColorRGBA SampleRainbow(float Hue);
	static ColorRGBA SampleColorStops(const ColorRGBA *pColors, int Count, float Position);
	static void GetSkinToneColors(int ClientId, const CGameClient *pGameClient, ColorRGBA &Body, ColorRGBA &Feet);
	static void GetAnimatedEndpointColors(int ClientId, const CGameClient *pGameClient, float Phase, ColorRGBA &Color1, ColorRGBA &Color2);
	static std::vector<STextColorSplit> BuildAnimatedColorSplits(const char *pText, const ColorRGBA &Color1, const ColorRGBA &Color2, float Phase, int Length = -1);
	static std::vector<STextColorSplit> BuildAnimatedTextSplits(const char *pText, int ClientId, CGameClient *pGameClient, float Phase, int Length = -1);
	static void ApplySixupColors(CTeeRenderInfo &RenderInfo, const ColorRGBA &Body, const ColorRGBA &Feet);
};

#endif
