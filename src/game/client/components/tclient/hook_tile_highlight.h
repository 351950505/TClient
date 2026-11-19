#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_HOOK_TILE_HIGHLIGHT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_HOOK_TILE_HIGHLIGHT_H

#include <base/vmath.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace tclient_hook_tile
{
	struct STile
	{
		int m_X = 0;
		int m_Y = 0;
	};

	struct SSelection
	{
		std::array<STile, 2> m_aTiles{};
		int m_Count = 0;
	};

	// 网络钩点为整数，碰撞按 round_to_int 取整；容许接缝两侧各 1.5 世界单位。
	// 只检查钩点附近，不能从玩家当前位置重新射线（钩传送/绕角后路径可能已改变）。
	template<typename FHookable>
	SSelection Select(vec2 HookPos, int Width, int Height, FHookable Hookable)
	{
		SSelection Result;
		constexpr float Seam = 1.5f;
		if(Width <= 0 || Height <= 0 || !std::isfinite(HookPos.x) || !std::isfinite(HookPos.y) ||
			HookPos.x < 0.0f || HookPos.y < 0.0f || HookPos.x >= Width * 32.0f || HookPos.y >= Height * 32.0f)
			return Result;

		const auto Cell = [](float Value) { return (int)std::floor((Value + 0.5f) / 32.0f); };
		const int MinX = std::max(0, Cell(HookPos.x - Seam));
		const int MaxX = std::min(Width - 1, Cell(HookPos.x + Seam));
		const int MinY = std::max(0, Cell(HookPos.y - Seam));
		const int MaxY = std::min(Height - 1, Cell(HookPos.y + Seam));
		STile Primary{Cell(HookPos.x), Cell(HookPos.y)};
		const auto Valid = [&](STile Tile) {
			return Tile.m_X >= 0 && Tile.m_X < Width && Tile.m_Y >= 0 && Tile.m_Y < Height && Hookable(Tile.m_X, Tile.m_Y);
		};
		const auto DistanceSquared = [&](STile Tile) {
			const float DX = HookPos.x - std::clamp(HookPos.x, Tile.m_X * 32.0f - 0.5f, (Tile.m_X + 1) * 32.0f - 0.5f);
			const float DY = HookPos.y - std::clamp(HookPos.y, Tile.m_Y * 32.0f - 0.5f, (Tile.m_Y + 1) * 32.0f - 0.5f);
			return DX * DX + DY * DY;
		};
		if(!Valid(Primary))
		{
			float BestDistance = Seam * Seam + 0.001f;
			bool Found = false;
			for(int Y = MinY; Y <= MaxY; ++Y)
				for(int X = MinX; X <= MaxX; ++X)
				{
					const STile Tile{X, Y};
					const float Distance = DistanceSquared(Tile);
					if(Valid(Tile) && Distance < BestDistance)
					{
						Primary = Tile;
						BestDistance = Distance;
						Found = true;
					}
				}
			if(!Found)
				return Result;
		}
		Result.m_aTiles[Result.m_Count++] = Primary;
		float BestDistance = Seam * Seam + 0.001f;
		for(int Y = MinY; Y <= MaxY; ++Y)
			for(int X = MinX; X <= MaxX; ++X)
			{
				// 只补一块共边的砖；四砖交点不扩成四块，斜角砖不冒充接缝。
				const STile Tile{X, Y};
				if(std::abs(X - Primary.m_X) + std::abs(Y - Primary.m_Y) != 1 || !Valid(Tile))
					continue;
				const float Distance = DistanceSquared(Tile);
				if(Distance < BestDistance)
				{
					Result.m_aTiles[1] = Tile;
					Result.m_Count = 2;
					BestDistance = Distance;
				}
			}
		return Result;
	}
}

#endif
