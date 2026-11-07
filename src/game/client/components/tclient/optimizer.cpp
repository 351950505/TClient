#include <game/client/components/tclient/optimizer.h>

#include <base/math.h>

#include <engine/shared/config.h>

#include <game/client/gameclient.h>

#include <cmath>

bool COptimizer::FogEnabled() const
{
	if(!g_Config.m_TcOptimizer || !g_Config.m_TcOptimizerFpsFog)
		return false;
	return Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK;
}

bool COptimizer::ParticlesDisabled() const
{
	if(!g_Config.m_TcOptimizer || !g_Config.m_TcOptimizerDisableParticles)
		return false;
	return Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK;
}

float COptimizer::CullRadius() const
{
	// 当前缩放下屏幕短边对应的世界尺寸的一半，乘可视百分比 = 清晰区世界半径；
	// 裁剪半径取它与雾半径（图块换算）的较大者，保证清晰区内永不裁。
	float Width, Height;
	Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom, &Width, &Height);
	const float Hole = std::min(Width, Height) * 0.5f * (g_Config.m_TcOptimizerFpsFogZoomPercent / 100.0f);
	return std::max(Hole, g_Config.m_TcOptimizerFpsFogRadiusTiles * 32.0f);
}

bool COptimizer::CullWorldPos(const vec2 &Pos, float Size) const
{
	if(!FogEnabled())
		return false;
	return length(Pos - GameClient()->m_Camera.m_Center) > CullRadius() + Size;
}

void COptimizer::ClipWorld()
{
	m_WorldClipped = false;
	if(!FogEnabled() || !g_Config.m_TcOptimizerFpsFogRenderRect || g_Config.m_TcOptimizerFpsFogMode != 1)
		return;
	// UI 屏幕固定高 1200，清晰区边长 = 1200 * percent%；换算到缓冲像素即
	// ScreenHeight() * percent%（等比缩放）。scissor 覆盖雾矩形之外的全部世界渲染。
	const int Hole = std::max((int)(Graphics()->ScreenHeight() * (g_Config.m_TcOptimizerFpsFogZoomPercent / 100.0f)), 1);
	Graphics()->ClipEnable((Graphics()->ScreenWidth() - Hole) / 2, (Graphics()->ScreenHeight() - Hole) / 2, Hole, Hole);
	m_WorldClipped = true;
}

void COptimizer::OnRender()
{
	if(m_WorldClipped)
	{
		Graphics()->ClipDisable();
		m_WorldClipped = false;
	}
	if(!FogEnabled() || !GameClient()->m_Snap.m_pGameInfoObj)
		return;
	DrawFog();
}

void COptimizer::DrawFog()
{
	Ui()->MapScreen();
	const CUIRect Screen = *Ui()->Screen();

	const float HoleR = std::min(Screen.w, Screen.h) * 0.5f * (g_Config.m_TcOptimizerFpsFogZoomPercent / 100.0f);
	constexpr float FogAlpha = 0.92f;
	const vec2 Center = vec2(Screen.w / 2.0f, Screen.h / 2.0f);

	Graphics()->TextureClear();
	Graphics()->BlendNormal();
	Graphics()->QuadsBegin();

	if(g_Config.m_TcOptimizerFpsFogMode == 1)
	{
		// 矩形雾：四条边各一条四角渐变带，外缘实、内缘透明；四角由左右带（整高）兜住。
		const ColorRGBA Fog(0.0f, 0.0f, 0.0f, FogAlpha);
		const ColorRGBA Clear(0.0f, 0.0f, 0.0f, 0.0f);
		const float Left = Center.x - HoleR;
		const float Right = Center.x + HoleR;
		const float Top = Center.y - HoleR;
		const float Bottom = Center.y + HoleR;

		Graphics()->SetColor4(Fog, Clear, Fog, Clear);
		IGraphics::CQuadItem LeftBand(0.0f, 0.0f, Left, Screen.h);
		Graphics()->QuadsDrawTL(&LeftBand, 1);
		Graphics()->SetColor4(Clear, Fog, Clear, Fog);
		IGraphics::CQuadItem RightBand(Right, 0.0f, Screen.w - Right, Screen.h);
		Graphics()->QuadsDrawTL(&RightBand, 1);
		Graphics()->SetColor4(Fog, Fog, Clear, Clear);
		IGraphics::CQuadItem TopBand(Left, 0.0f, Right - Left, Top);
		Graphics()->QuadsDrawTL(&TopBand, 1);
		Graphics()->SetColor4(Clear, Clear, Fog, Fog);
		IGraphics::CQuadItem BottomBand(Left, Bottom, Right - Left, Screen.h - Bottom);
		Graphics()->QuadsDrawTL(&BottomBand, 1);
	}
	else
	{
		// 圆形雾：清晰区外一圈渐变带（径向分 12 档），再往外一圈实雾直到盖住屏幕四角。
		const float BandW = std::max(HoleR * 0.45f, 48.0f);
		const float FullR = HoleR + BandW;
		const float CornerR = std::hypot(Screen.w, Screen.h) * 0.5f + 1.0f;
		constexpr int Sectors = 48;
		constexpr int Bands = 12;
		constexpr float Tau = 2.0f * pi;

		const auto &&DrawRing = [&](float r0, float r1, float Alpha) {
			for(int s = 0; s < Sectors; s++)
			{
				const float a0 = Tau * s / Sectors;
				const float a1 = Tau * (s + 1) / Sectors;
				const float c0 = std::cos(a0), s0 = std::sin(a0);
				const float c1 = std::cos(a1), s1 = std::sin(a1);
				// CFreeformItem 的几何环序是 v0→v1→v3→v2（后端两条路径一致），
				// 内/外弧必须放在 v0/v3，v1/v2 放径向对角，否则三角化成交叉尖三角。
				const IGraphics::CFreeformItem Item(
					Center.x + c0 * r0, Center.y + s0 * r0,
					Center.x + c0 * r1, Center.y + s0 * r1,
					Center.x + c1 * r0, Center.y + s1 * r0,
					Center.x + c1 * r1, Center.y + s1 * r1);
				Graphics()->SetColor(0.0f, 0.0f, 0.0f, Alpha);
				Graphics()->QuadsDrawFreeform(&Item, 1);
			}
		};

		for(int b = 0; b < Bands; b++)
			DrawRing(HoleR + BandW * b / (float)Bands, HoleR + BandW * (b + 1) / (float)Bands, FogAlpha * (b + 1) / (float)Bands);
		DrawRing(FullR, CornerR, FogAlpha);
	}

	Graphics()->QuadsEnd();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
}
