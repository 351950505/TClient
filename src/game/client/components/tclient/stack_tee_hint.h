#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_STACK_TEE_HINT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_STACK_TEE_HINT_H

#include <base/vmath.h>

#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace tclient_stack_tee
{
	inline constexpr const char *BIND_COMMAND = "toggle me_stack_tee_hint 0 1";

	class CStackTeeCollisionState
	{
	public:
		CStackTeeCollisionState(bool Super, bool Solo, bool CollisionDisabled) :
			m_Super(Super),
			m_Solo(Solo),
			m_CollisionDisabled(CollisionDisabled)
		{
		}

		bool m_Super;
		bool m_Solo;
		bool m_CollisionDisabled;
	};

	class CStackTeeHintCharacter
	{
	public:
		CStackTeeHintCharacter(bool Active, bool Frozen, int ClientId, int X, float Y) :
			m_Active(Active),
			m_Frozen(Frozen),
			m_ClientId(ClientId),
			m_X(X),
			m_Y(Y)
		{
		}

		bool m_Active;
		bool m_Frozen;
		int m_ClientId;
		int m_X;
		float m_Y;
	};

	inline bool CanPlayersCollide(bool TeamCanCollide, bool PlayerCollisionEnabled, const CStackTeeCollisionState &Local, const CStackTeeCollisionState &Other)
	{
		if(!TeamCanCollide)
			return false;

		const bool Super = Local.m_Super || Other.m_Super;
		if(!Super && (Local.m_Solo || Other.m_Solo))
			return false;

		return Super || (PlayerCollisionEnabled && !Local.m_CollisionDisabled && !Other.m_CollisionDisabled);
	}

	inline bool TileStopsDrop(int Tile)
	{
		return Tile == TILE_DEATH;
	}

	inline bool DropPathClearOfTee(vec2 Start, vec2 End, vec2 Other, float CollisionRadius)
	{
		if(!std::isfinite(Start.x) || !std::isfinite(Start.y) || !std::isfinite(End.x) || !std::isfinite(End.y) ||
			!std::isfinite(Other.x) || !std::isfinite(Other.y) || CollisionRadius < 0.0f)
			return false;
		if(End.y <= Start.y)
			return true;

		const float ClosestY = std::clamp(Other.y, Start.y, End.y);
		const float DeltaX = Other.x - Start.x;
		const float DeltaY = Other.y - ClosestY;
		return DeltaX * DeltaX + DeltaY * DeltaY >= CollisionRadius * CollisionRadius;
	}

	template<typename FCanMoveDown>
	bool ClearDropPath(vec2 Start, vec2 End, FCanMoveDown CanMoveDown)
	{
		if(!std::isfinite(Start.x) || !std::isfinite(Start.y) || !std::isfinite(End.x) || !std::isfinite(End.y))
			return false;
		if(End.y <= Start.y)
			return true;

		constexpr float SampleStep = 8.0f;
		const int Steps = std::max(1, (int)std::ceil((End.y - Start.y) / SampleStep));
		for(int Step = 0; Step <= Steps; ++Step)
		{
			const float Fraction = (float)Step / Steps;
			const vec2 Point(Start.x, Start.y + (End.y - Start.y) * Fraction);
			if(!CanMoveDown(Point))
				return false;
		}
		return true;
	}

	template<typename FClearDropPath>
	bool ShouldShow(bool Enabled, const CStackTeeHintCharacter &Local, const CStackTeeHintCharacter &Target, FClearDropPath ClearDropPath)
	{
		if(!Enabled || !Local.m_Active || Local.m_Frozen || !Target.m_Active || !Target.m_Frozen ||
			Local.m_ClientId < 0 || Local.m_ClientId == Target.m_ClientId ||
			Local.m_X != Target.m_X || Local.m_Y >= Target.m_Y)
			return false;

		return ClearDropPath();
	}
}

#endif
