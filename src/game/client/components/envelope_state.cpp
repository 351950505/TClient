#include "envelope_state.h"

#include <base/time.h>

#include <game/client/gameclient.h>
#include <game/localization.h>

#include <chrono>

using namespace std::chrono_literals;

CEnvelopeState::CEnvelopeState(IMap *pMap, bool OnlineOnly)
{
	m_pEnvelopePoints = std::make_shared<CMapBasedEnvelopePointAccess>(pMap);
	m_OnlineOnly = OnlineOnly;

	if(pMap != nullptr)
	{
		int EnvelopeStart, EnvelopeNum;
		pMap->GetType(MAPITEMTYPE_ENVELOPE, &EnvelopeStart, &EnvelopeNum);
		m_vpEnvelopes.reserve(EnvelopeNum);
		for(int i = 0; i < EnvelopeNum; i++)
			m_vpEnvelopes.push_back((const CMapItemEnvelope *)pMap->GetItem(EnvelopeStart + i));
	}
}

void CEnvelopeState::Update()
{
	using namespace std::chrono;

	if(!m_OnlineOnly)
	{
		m_CachedTime = time_get_nanoseconds();
	}
	else if(GameClient()->m_Snap.m_pGameInfoObj)
	{
		int EnvelopeTick;
		double TickRatio;
		if(Client()->State() == IClient::STATE_DEMOPLAYBACK || !g_Config.m_ClPredict ||
			(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW))
		{
			EnvelopeTick = Client()->PrevGameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick;
			const int CurTick = Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick;
			TickRatio = mix<double>(0, CurTick - EnvelopeTick, (double)Client()->IntraGameTick(g_Config.m_ClDummy));
		}
		else
		{
			EnvelopeTick = Client()->PredGameTick(g_Config.m_ClDummy) - 1 - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick;
			TickRatio = (double)Client()->PredIntraGameTick(g_Config.m_ClDummy);
		}
		const nanoseconds NanosPerTick = nanoseconds(1s) / static_cast<int64_t>(Client()->GameTickSpeed());
		m_CachedTime = duration_cast<nanoseconds>(TickRatio * NanosPerTick) + EnvelopeTick * NanosPerTick;
	}
	else
	{
		m_CachedTime = nanoseconds::zero();
	}
}

void CEnvelopeState::EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const
{
	if(EnvelopeIndex < 0 || EnvelopeIndex >= (int)m_vpEnvelopes.size())
		return;

	const CMapItemEnvelope *pItem = m_vpEnvelopes[EnvelopeIndex];
	if(pItem->m_Channels <= 0)
		return;
	Channels = std::min({Channels, (size_t)pItem->m_Channels, (size_t)CEnvPoint::MAX_CHANNELS});

	m_pEnvelopePoints->SetPointsRange(pItem->m_StartPoint, pItem->m_NumPoints);
	if(m_pEnvelopePoints->NumPoints() == 0)
		return;

	CRenderMap::RenderEvalEnvelope(m_pEnvelopePoints.get(), m_CachedTime + std::chrono::milliseconds(TimeOffsetMillis), Result, Channels);
}
