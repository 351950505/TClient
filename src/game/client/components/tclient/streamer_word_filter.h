#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_STREAMER_WORD_FILTER_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_STREAMER_WORD_FILTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace QmStreamerWordFilter
{
	// 只替换命中片段，不增长缓冲区；调用方决定是否启用，不修改网络输入。
	// 仅在主线程发布完整词表，所有显示入口共用同一版本。
	void SetAdditionalWords(const std::vector<std::string> &vWords);
	uint64_t Revision();
	bool Filter(char *pText, bool Enabled);
	bool Filter(std::string &Text, bool Enabled);
}

#endif
