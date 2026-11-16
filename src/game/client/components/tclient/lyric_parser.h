/* TClient: 歌词解析（从 island_music 提取的纯函数集）。
   输入为纯文本（标准 LRC / 网易 yrc），输出为统一的歌词行结构；
   无状态、不触网络、不依赖组件——可独立测试与复用。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_LYRIC_PARSER_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_LYRIC_PARSER_H

#include <cstdint>
#include <string>
#include <vector>

namespace TcLyric {

// per-character karaoke timing, relative to the line start.
struct SWord
{
	int64_t m_StartMs = 0; // absolute start time of this char
	int64_t m_EndMs = 0;   // absolute end time (start of next char / line end)
	int m_CharBegin = 0;   // offset of the char in the line's UTF-8 text
	int m_CharLen = 0;     // UTF-8 byte length of the char
};

struct SLyricLine
{
	int64_t m_StartMs = 0;
	std::string m_Text;
	// empty -> whole line turns green at m_StartMs (classic LRC).
	// non-empty -> netease karaoke: each char lights up at its own time.
	std::vector<SWord> m_vWords;
	std::string m_Translation;
	bool m_IsSimplifiedChinese = false;
};

// 标准 LRC：[mm:ss.xx] 文本（多时间戳/去重/逐字时间合成）。
bool ParseSyncedLyrics(const char *pSyncedLyrics, std::vector<SLyricLine> &vOut);

// 网易 yrc：[start,duration](segStart,dur,0)文本(...) 逐字格式。
bool ParseYrcLyrics(const char *pYrc, std::vector<SLyricLine> &vOut);

// Sorted original cues: merge timestamp groups once, choose the nearest <=250ms
// cue (reject equal-distance ambiguity), and preserve already attached translations.
// Provider Chinese may mix Latin words; this does not change original-language detection.
void AttachTranslatedLyrics(std::vector<SLyricLine> &vLines, const char *pTranslatedLyrics);
bool IsSimplifiedChinese(const std::string &Text);

} // namespace TcLyric

#endif
