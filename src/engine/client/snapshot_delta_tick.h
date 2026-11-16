#ifndef ENGINE_CLIENT_SNAPSHOT_DELTA_TICK_H
#define ENGINE_CLIENT_SNAPSHOT_DELTA_TICK_H

#include <cstdint>

inline bool DecodeSnapshotDeltaTick(int GameTick, int DeltaDistance, int &DeltaTick)
{
	if(GameTick < 0 || DeltaDistance < 0)
		return false;
	const int64_t Tick = (int64_t)GameTick - (int64_t)DeltaDistance;
	// -1 is the full-snapshot sentinel; other bases must not be in the future.
	if(Tick < -1 || Tick > GameTick)
		return false;
	DeltaTick = (int)Tick;
	return true;
}

#endif // ENGINE_CLIENT_SNAPSHOT_DELTA_TICK_H
