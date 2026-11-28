#ifndef GAME_MAP_FREEZE_OUTLINE_GEOMETRY_H
#define GAME_MAP_FREEZE_OUTLINE_GEOMETRY_H

#include <algorithm>
#include <array>

// Values are deliberately ordered like the outline priority in outlines.cpp.
inline constexpr int FREEZE_OUTLINE_NONE = 0;
inline constexpr int FREEZE_OUTLINE_UNFREEZE = 1;
inline constexpr int FREEZE_OUTLINE_FREEZE = 2;
inline constexpr int FREEZE_OUTLINE_TELE = 3;
inline constexpr int FREEZE_OUTLINE_KILL = 4;
inline constexpr int FREEZE_OUTLINE_SOLID = 5;

class CFreezeOutlineMapView
{
public:
	const int *m_pTypes = nullptr;
	int m_Width = 0;
	int m_Height = 0;

	int TypeAt(int TileX, int TileY) const
	{
		if(!m_pTypes || m_Width <= 0 || m_Height <= 0)
			return FREEZE_OUTLINE_NONE;
		TileX = std::clamp(TileX, 0, m_Width - 1);
		TileY = std::clamp(TileY, 0, m_Height - 1);
		return m_pTypes[TileY * m_Width + TileX];
	}
};

class CFreezeOutlineRect
{
public:
	int m_X = 0;
	int m_Y = 0;
	int m_Width = 0;
	int m_Height = 0;
};

class CFreezeOutlineGeometry
{
public:
	std::array<CFreezeOutlineRect, 8> m_aBand{};
	int m_NumBand = 0;

	// Subtracting eight disjoint band rectangles from one tile yields at most
	// 25 rectangles. Keep a little spare capacity so malformed input is safe.
	std::array<CFreezeOutlineRect, 32> m_aInterior{};
	int m_NumInterior = 0;
};

inline void AddFreezeOutlineRect(CFreezeOutlineGeometry &Geometry, int X, int Y, int Width, int Height)
{
	if(Width <= 0 || Height <= 0 || Geometry.m_NumBand >= (int)Geometry.m_aBand.size())
		return;
	Geometry.m_aBand[Geometry.m_NumBand++] = {X, Y, Width, Height};
}

// Returns the exact border quads used by the outline renderer. When
// IncludeSolidSides is true, a solid neighbor is treated as an exposed side
// for the entity-art cutout only; outline drawing continues to use false.
inline CFreezeOutlineGeometry MakeFreezeOutlineGeometry(const CFreezeOutlineMapView &Map, int TileX, int TileY, int Type, int Width, bool IncludeSolidSides, bool BuildComplement)
{
	CFreezeOutlineGeometry Result;
	if(!Map.m_pTypes || Type == FREEZE_OUTLINE_NONE)
		return Result;

	Width = std::clamp(Width, 1, 16);
	constexpr int TILE_SIZE = 32;
	auto Covered = [&](int TileType) {
		return TileType >= Type && !(IncludeSolidSides && TileType == FREEZE_OUTLINE_SOLID);
	};

	// NW, N, NE, W, E, SW, S, SE; clamp behavior matches COutlines::GetTile.
	const bool NW = Covered(Map.TypeAt(TileX - 1, TileY - 1));
	const bool N = Covered(Map.TypeAt(TileX, TileY - 1));
	const bool NE = Covered(Map.TypeAt(TileX + 1, TileY - 1));
	const bool W = Covered(Map.TypeAt(TileX - 1, TileY));
	const bool E = Covered(Map.TypeAt(TileX + 1, TileY));
	const bool SW = Covered(Map.TypeAt(TileX - 1, TileY + 1));
	const bool S = Covered(Map.TypeAt(TileX, TileY + 1));
	const bool SE = Covered(Map.TypeAt(TileX + 1, TileY + 1));

	// Isolated corners are emitted only when both adjacent sides are covered
	// but the diagonal tile is not, matching the existing outline topology.
	if(!NW && N && W)
		AddFreezeOutlineRect(Result, 0, 0, Width, Width);
	if(!NE && N && E)
		AddFreezeOutlineRect(Result, TILE_SIZE - Width, 0, Width, Width);
	if(!SW && W && S)
		AddFreezeOutlineRect(Result, 0, TILE_SIZE - Width, Width, Width);
	if(!SE && S && E)
		AddFreezeOutlineRect(Result, TILE_SIZE - Width, TILE_SIZE - Width, Width, Width);

	if(!N)
		AddFreezeOutlineRect(Result, 0, 0, TILE_SIZE, Width);
	if(!S)
		AddFreezeOutlineRect(Result, 0, TILE_SIZE - Width, TILE_SIZE, Width);
	if(!W)
	{
		if(N && S)
			AddFreezeOutlineRect(Result, 0, 0, Width, TILE_SIZE);
		else if(S)
			AddFreezeOutlineRect(Result, 0, Width, Width, TILE_SIZE - Width);
		else if(N)
			AddFreezeOutlineRect(Result, 0, 0, Width, TILE_SIZE - Width);
		else
			AddFreezeOutlineRect(Result, 0, Width, Width, TILE_SIZE - 2 * Width);
	}
	if(!E)
	{
		if(N && S)
			AddFreezeOutlineRect(Result, TILE_SIZE - Width, 0, Width, TILE_SIZE);
		else if(S)
			AddFreezeOutlineRect(Result, TILE_SIZE - Width, Width, Width, TILE_SIZE - Width);
		else if(N)
			AddFreezeOutlineRect(Result, TILE_SIZE - Width, 0, Width, TILE_SIZE - Width);
		else
			AddFreezeOutlineRect(Result, TILE_SIZE - Width, Width, Width, TILE_SIZE - 2 * Width);
	}

	if(!BuildComplement)
		return Result;

	// Complement the band into rectangles suitable for batched tile drawing.
	Result.m_aInterior[0] = {0, 0, TILE_SIZE, TILE_SIZE};
	Result.m_NumInterior = 1;
	for(int BandIndex = 0; BandIndex < Result.m_NumBand; ++BandIndex)
	{
		const auto &Band = Result.m_aBand[BandIndex];
		std::array<CFreezeOutlineRect, 32> Next{};
		int NumNext = 0;
		for(int RectangleIndex = 0; RectangleIndex < Result.m_NumInterior; ++RectangleIndex)
		{
			const auto &Rect = Result.m_aInterior[RectangleIndex];
			const int Left = std::max(Rect.m_X, Band.m_X);
			const int Top = std::max(Rect.m_Y, Band.m_Y);
			const int Right = std::min(Rect.m_X + Rect.m_Width, Band.m_X + Band.m_Width);
			const int Bottom = std::min(Rect.m_Y + Rect.m_Height, Band.m_Y + Band.m_Height);
			if(Left >= Right || Top >= Bottom)
			{
				if(NumNext < (int)Next.size())
					Next[NumNext++] = Rect;
				continue;
			}

			auto AddInterior = [&](int X, int Y, int Width, int Height) {
				if(Width > 0 && Height > 0 && NumNext < (int)Next.size())
					Next[NumNext++] = {X, Y, Width, Height};
			};
			AddInterior(Rect.m_X, Rect.m_Y, Rect.m_Width, Top - Rect.m_Y);
			AddInterior(Rect.m_X, Bottom, Rect.m_Width, Rect.m_Y + Rect.m_Height - Bottom);
			AddInterior(Rect.m_X, Top, Left - Rect.m_X, Bottom - Top);
			AddInterior(Right, Top, Rect.m_X + Rect.m_Width - Right, Bottom - Top);
		}
		Result.m_aInterior = Next;
		Result.m_NumInterior = NumNext;
	}
	return Result;
}

#endif
