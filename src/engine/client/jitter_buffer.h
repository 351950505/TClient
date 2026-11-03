/* TClient: 自适应快照插值缓冲的估计器（纯函数，便于单测）。
   统计最近若干个快照的到达间隔，网络抖动升高时给出建议的插值滞后面（毫秒），
   网络平稳时回到 0。运行时由 CClient 在快照到达处调用并平滑逼近该目标。 */
#ifndef ENGINE_CLIENT_JITTER_BUFFER_H
#define ENGINE_CLIENT_JITTER_BUFFER_H

#include <cstdlib>

namespace TcJitterBuffer
{
constexpr int MIN_SAMPLES = 8;    // 少于该样本数不启用（刚进服）
constexpr int MAX_INTERVAL_MS = 200; // 超出视为断线/尖峰，样本作废
constexpr int MIN_INTERVAL_MS = 5;

/* 从最近的到达间隔（毫秒）估计建议缓冲。
   jitter = 平均绝对偏差（MAD）；目标 = 2*jitter 四舍五入到 2ms 档，钳到 [0, MaxMs]。
   样本不足或全为非法值时返回 0。 */
inline int TargetMs(const int *pIntervalsMs, int Count, int MaxMs)
{
	if(!pIntervalsMs || Count < MIN_SAMPLES || MaxMs <= 0)
		return 0;
	int Valid = 0;
	long long Sum = 0;
	for(int i = 0; i < Count; i++)
	{
		if(pIntervalsMs[i] < MIN_INTERVAL_MS || pIntervalsMs[i] > MAX_INTERVAL_MS)
			continue;
		Sum += pIntervalsMs[i];
		Valid++;
	}
	if(Valid < MIN_SAMPLES)
		return 0;
	const long long Mean = Sum / Valid;
	long long Deviation = 0;
	for(int i = 0; i < Count; i++)
	{
		if(pIntervalsMs[i] < MIN_INTERVAL_MS || pIntervalsMs[i] > MAX_INTERVAL_MS)
			continue;
		Deviation += std::abs((long long)pIntervalsMs[i] - Mean);
	}
	const long long Target = ((Deviation * 2 + Valid - 1) / Valid + 1) & ~1; // 2*MAD，进位到 2ms 档
	if(Target < 2)
		return 0;
	return (int)(Target > MaxMs ? MaxMs : Target);
}
} // namespace TcJitterBuffer

#endif
