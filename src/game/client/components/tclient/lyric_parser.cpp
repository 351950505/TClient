/* TClient: 歌词解析实现，见 lyric_parser.h */
#include "lyric_parser.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <iterator>

#include <base/detect.h>
#if defined(CONF_FAMILY_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace TcLyric {

// 中文脚本转换：Traditional=false 转 zh-CN 简体，true 转 zh-Hant 繁体。
// 转换标志必须独占（LCMAP_*_CHINESE 不能与其他标志组合）。
static std::string ConvertChinese(const std::string &Text, bool Traditional)
{
#if defined(CONF_FAMILY_WINDOWS)
	if(Text.empty() || Text.size() > (size_t)std::numeric_limits<int>::max())
		return Text;
	const wchar_t *pLocale = Traditional ? L"zh-Hant" : L"zh-CN";
	const DWORD Mapping = Traditional ? LCMAP_TRADITIONAL_CHINESE : LCMAP_SIMPLIFIED_CHINESE;
	const int Size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), (int)Text.size(), nullptr, 0);
	if(Size <= 0)
		return Text;
	std::wstring Wide(Size, L'\0');
	if(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), (int)Text.size(), Wide.data(), Size) != Size)
		return Text;
	const int MappedSize = LCMapStringEx(pLocale, Mapping, Wide.data(), Size, nullptr, 0, nullptr, nullptr, 0);
	if(MappedSize <= 0)
		return Text;
	std::wstring Mapped(MappedSize, L'\0');
	if(LCMapStringEx(pLocale, Mapping, Wide.data(), Size, Mapped.data(), MappedSize, nullptr, nullptr, 0) != MappedSize)
		return Text;
	const int Bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Mapped.data(), MappedSize, nullptr, 0, nullptr, nullptr);
	if(Bytes <= 0)
		return Text;
	std::string Result(Bytes, '\0');
	if(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Mapped.data(), MappedSize, Result.data(), Bytes, nullptr, nullptr) == Bytes)
		return Result;
#endif
	return Text;
}

static std::string SimplifyChinese(const std::string &Text)
{
	return ConvertChinese(Text, false);
}

// Validate UTF-8 and reject kana/Hangul for both uses, but keep the original
// language heuristic separate from provider-supplied Chinese translations.
static bool ChineseTextLetters(const std::string &Text, int &Han, int &OtherLetters)
{
	Han = 0;
	OtherLetters = 0;
	for(size_t i = 0; i < Text.size();)
	{
		const unsigned char First = (unsigned char)Text[i++];
		uint32_t Code = First;
		int Remaining = 0;
		if(First >= 0xc2 && First <= 0xdf) { Code &= 0x1f; Remaining = 1; }
		else if(First >= 0xe0 && First <= 0xef) { Code &= 0x0f; Remaining = 2; }
		else if(First >= 0xf0 && First <= 0xf4) { Code &= 0x07; Remaining = 3; }
		else if(First >= 0x80) return false;
		if(i + Remaining > Text.size())
			return false;
		const int SequenceLength = Remaining + 1;
		while(Remaining-- > 0)
		{
			const unsigned char Next = (unsigned char)Text[i++];
			if((Next & 0xc0) != 0x80) return false;
			Code = (Code << 6) | (Next & 0x3f);
		}
		if((SequenceLength == 2 && Code < 0x80) || (SequenceLength == 3 && Code < 0x800) ||
			(SequenceLength == 4 && Code < 0x10000) || Code > 0x10ffff || (Code >= 0xd800 && Code <= 0xdfff))
			return false;
		if((Code >= 0x3040 && Code <= 0x30ff) || (Code >= 0xac00 && Code <= 0xd7af) || (Code >= 0x1100 && Code <= 0x11ff))
			return false;
		if((Code >= 0x3400 && Code <= 0x9fff) || (Code >= 0x20000 && Code <= 0x3134f))
			++Han;
		else if((Code >= 'A' && Code <= 'Z') || (Code >= 'a' && Code <= 'z') || (Code >= 0xc0 && Code <= 0x2af) || (Code >= 0x370 && Code <= 0x52f))
			++OtherLetters;
	}
	return true;
}

bool IsSimplifiedChinese(const std::string &Text)
{
	int Han, OtherLetters;
	// Shared Han characters alone cannot identify a language perfectly.
	return ChineseTextLetters(Text, Han, OtherLetters) && Han > 0 && Han > OtherLetters && SimplifyChinese(Text) == Text;
}

// 渠道返回的中文脚本不统一（QQ/酷狗/lrclib 常回繁体），正文统一转到目标
// 脚本（默认简体；用户勾选繁体模式则转繁体）。只转 predominantly 中文
// 的行（假名/谚文一律原样，日文汉字不能转成中文简繁字形），已是目标
// 脚本的行原样。翻译叠加始终走简体（AttachTranslatedLyrics 不受此开关影响）。
static void ConvertLyricScript(std::string &Text, bool Traditional)
{
	int Han, OtherLetters;
	if(!ChineseTextLetters(Text, Han, OtherLetters) || Han <= 0 || Han <= OtherLetters)
		return;
	std::string Converted = ConvertChinese(Text, Traditional);
	if(Converted != Text)
		Text = std::move(Converted);
}

static bool IsProviderChineseTranslation(const std::string &Text)
{
	int Han, OtherLetters;
	// Provider translation fields may retain English words ("Baby，宝贝").
	// Require Han and valid text; pure romaji/Latin, kana and Hangul are not Chinese.
	return ChineseTextLetters(Text, Han, OtherLetters) && Han > 0;
}

static void SynthesizeWordTimings(std::vector<SLyricLine> &vLines);
static int Utf8CharLen(unsigned char c);



	static bool ParseLrcTimestamp(const char *pText, int64_t &OutMs, const char **ppEnd)
	{
		if(pText == nullptr || pText[0] != '[')
			return false;

		int Minutes = 0;
		int Seconds = 0;
		int Fraction = 0;
		int FractionDigits = 0;
		const char *p = pText + 1;
		if(!isdigit((unsigned char)*p))
			return false;

		while(isdigit((unsigned char)*p))
		{
			const int Digit = *p - '0';
			if(Minutes > (std::numeric_limits<int>::max() - Digit) / 10)
				return false;
			Minutes = Minutes * 10 + Digit;
			++p;
		}
		if(*p != ':')
			return false;
		++p;
		if(!isdigit((unsigned char)*p))
			return false;
		while(isdigit((unsigned char)*p))
		{
			const int Digit = *p - '0';
			if(Seconds > (std::numeric_limits<int>::max() - Digit) / 10)
				return false;
			Seconds = Seconds * 10 + Digit;
			++p;
		}
		if(*p == '.' || *p == ',')
		{
			++p;
			while(isdigit((unsigned char)*p) && FractionDigits < 3)
			{
				Fraction = Fraction * 10 + (*p - '0');
				++FractionDigits;
				++p;
			}
			while(isdigit((unsigned char)*p))
				++p;
		}
		if(*p != ']')
			return false;
		++p;

		while(FractionDigits > 0 && FractionDigits < 3)
		{
			Fraction *= 10;
			++FractionDigits;
		}

		OutMs = (int64_t)Minutes * 60000 + (int64_t)Seconds * 1000 + Fraction;
		if(ppEnd)
			*ppEnd = p;
		return true;
	}

	bool ParseSyncedLyrics(const char *pSyncedLyrics, std::vector<SLyricLine> &vOut, bool Traditional)
	{
		vOut.clear();
		if(pSyncedLyrics == nullptr || pSyncedLyrics[0] == '\0')
			return false;

		const char *p = pSyncedLyrics;
		std::vector<int64_t> vTimestamps;
		while(*p)
		{
			while(*p == '\r' || *p == '\n')
				++p;
			if(*p == '\0')
				break;

			const char *pLineStart = p;
			while(*p && *p != '\n' && *p != '\r')
				++p;
			const char *pLineEnd = p;

			vTimestamps.clear();
			const char *pCursor = pLineStart;
			while(pCursor < pLineEnd)
			{
				int64_t TimestampMs = 0;
				const char *pAfter = nullptr;
				if(!ParseLrcTimestamp(pCursor, TimestampMs, &pAfter) || pAfter == nullptr || pAfter > pLineEnd)
					break;
				vTimestamps.push_back(TimestampMs);
				pCursor = pAfter;
			}

			while(pCursor < pLineEnd && (*pCursor == ' ' || *pCursor == '\t'))
				++pCursor;

			if(vTimestamps.empty())
				continue;

			std::string Text(pCursor, pLineEnd);
			while(!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
				Text.pop_back();
			if(Text.empty())
				continue;

			for(int64_t TimestampMs : vTimestamps)
				vOut.push_back({TimestampMs, Text});
		}

		if(vOut.empty())
			return false;

		std::stable_sort(vOut.begin(), vOut.end(), [](const SLyricLine &A, const SLyricLine &B) {
			return A.m_StartMs < B.m_StartMs;
		});

		// merge duplicate timestamps of the same line
		std::vector<SLyricLine> vMerged;
		vMerged.reserve(vOut.size());
		for(SLyricLine &Line : vOut)
		{
			if(!vMerged.empty() && vMerged.back().m_Text == Line.m_Text && Line.m_StartMs - vMerged.back().m_StartMs <= 150)
				continue;
			vMerged.push_back(std::move(Line));
		}
		vOut = std::move(vMerged);
		for(SLyricLine &Line : vOut)
		{
			ConvertLyricScript(Line.m_Text, Traditional);
			Line.m_IsSimplifiedChinese = IsSimplifiedChinese(Line.m_Text);
		}
		SynthesizeWordTimings(vOut);
		return !vOut.empty();
	}

	// No per-character timings available (plain LRC). Synthesize an even
	// per-character timeline across each line so we can still show a karaoke
	// wipe: every char of a line turns green in proportion to how far playback
	// has progressed through that line. Line duration = next line's start - this
	// line's start (capped to a sane maximum for the final / very long lines).
	static void SynthesizeWordTimings(std::vector<SLyricLine> &vLines)
	{
		for(size_t i = 0; i < vLines.size(); ++i)
		{
			SLyricLine &Line = vLines[i];
			if(!Line.m_vWords.empty())
				continue; // real yrc timings already present
			if(Line.m_Text.empty())
				continue;

			int64_t LineStart = Line.m_StartMs;
			int64_t LineEnd;
			if(i + 1 < vLines.size())
				LineEnd = vLines[i + 1].m_StartMs;
			else
				LineEnd = LineStart + 5000; // last line: assume 5s
			int64_t LineDur = LineEnd - LineStart;
			if(LineDur <= 0)
				LineDur = 3000; // degenerate: assume 3s
			if(LineDur > 15000)
				LineDur = 15000; // don't stretch silence gaps over the whole line

			// split into UTF-8 characters
			const std::string &Text = Line.m_Text;
			std::vector<std::pair<int, int>> vChars; // (byteOffset, byteLen)
			for(int Off = 0; Off < (int)Text.size();)
			{
				int Len = Utf8CharLen((unsigned char)Text[Off]);
				if(Off + Len > (int)Text.size())
					Len = 1;
				vChars.push_back({Off, Len});
				Off += Len;
			}
			if(vChars.empty())
				continue;

			const int N = (int)vChars.size();
			for(int k = 0; k < N; ++k)
			{
				SWord W;
				W.m_CharBegin = vChars[k].first;
				W.m_CharLen = vChars[k].second;
				W.m_StartMs = LineStart + LineDur * k / N;
				W.m_EndMs = LineStart + LineDur * (k + 1) / N;
				Line.m_vWords.push_back(W);
			}
		}
	}

	// ---- karaoke (per-character) lyrics ----
	// UTF-8 lead-byte -> sequence length. Invalid bytes are treated as 1.
	static int Utf8CharLen(unsigned char c)
	{
		if(c < 0x80)
			return 1;
		if((c & 0xE0) == 0xC0)
			return 2;
		if((c & 0xF0) == 0xE0)
			return 3;
		if((c & 0xF8) == 0xF0)
			return 4;
		return 1;
	}

	// Build one SWord per UTF-8 character from the raw timings produced by the
	// netease yrc format: each timing segment is (time_ms, duration_ms, text)
	// where `text` may span several characters. Every char inside a segment
	// shares the segment's start time.
	static void BuildLineWords(const std::string &Text, std::vector<SWord> &vOut,
		const std::vector<std::pair<int64_t, int64_t>> &vSegStartDur)
	{
		vOut.clear();
		(void)Text;
		(void)vSegStartDur;
	}

	bool ParseYrcLyrics(const char *pYrc, std::vector<SLyricLine> &vOut, bool Traditional)
	{
		vOut.clear();
		if(pYrc == nullptr || pYrc[0] == '\0')
			return false;

		const char *p = pYrc;
		while(*p)
		{
			while(*p == '\r' || *p == '\n')
				++p;
			if(*p == '\0')
				break;

			const char *pLineStart = p;
			while(*p && *p != '\n' && *p != '\r')
				++p;
			const char *pLineEnd = p;

			// line metadata: [start,duration](segStart,dur,0)text(segStart,dur,0)text...
			const char *pCursor = pLineStart;
			if(*pCursor != '[')
				continue;
			++pCursor;
			int64_t LineStart = 0, LineDur = 0;
			{
				int64_t Val = 0;
				bool Any = false;
				while(pCursor < pLineEnd && *pCursor != ',')
				{
					if(isdigit((unsigned char)*pCursor)) { Val = Val * 10 + (*pCursor - '0'); Any = true; }
					++pCursor;
				}
				if(!Any) continue;
				LineStart = Val;
				if(pCursor < pLineEnd && *pCursor == ',') ++pCursor;
				Val = 0;
				while(pCursor < pLineEnd && *pCursor != ']')
				{
					if(isdigit((unsigned char)*pCursor)) Val = Val * 10 + (*pCursor - '0');
					++pCursor;
				}
				LineDur = Val;
				if(pCursor < pLineEnd && *pCursor == ']') ++pCursor;
			}

			std::string Text;
			std::vector<SWord> vWords;
			// parse (segStart, segDur, 0)char(s) groups
			while(pCursor < pLineEnd)
			{
				if(*pCursor == '(')
				{
					++pCursor;
					int64_t SegStart = 0, SegDur = 0;
					int Field = 0;
					int64_t Val = 0;
					while(pCursor < pLineEnd && *pCursor != ')')
					{
						if(isdigit((unsigned char)*pCursor)) Val = Val * 10 + (*pCursor - '0');
						else if(*pCursor == ',')
						{
							if(Field == 0) SegStart = Val;
							else if(Field == 1) SegDur = Val;
							Val = 0;
							++Field;
						}
						++pCursor;
					}
					if(pCursor < pLineEnd) ++pCursor; // skip ')'
					// consume chars until next '(' or end
					int SegCharBegin = (int)Text.size();
					while(pCursor < pLineEnd && *pCursor != '(')
					{
						Text.push_back(*pCursor);
						++pCursor;
					}
					// split the just-added run into individual chars
					int i = SegCharBegin;
					int NumChars = 0;
					std::vector<int> vCharPos;
					while(i < (int)Text.size())
					{
						int Len = Utf8CharLen((unsigned char)Text[i]);
						if(i + Len > (int)Text.size()) Len = (int)Text.size() - i;
						vCharPos.push_back(i);
						i += Len;
						NumChars++;
					}
					for(int c = 0; c < NumChars; ++c)
					{
						SWord W;
						W.m_StartMs = SegStart;
						W.m_EndMs = SegStart + SegDur;
						W.m_CharBegin = vCharPos[c];
						int NextPos = (c + 1 < NumChars) ? vCharPos[c + 1] : (int)Text.size();
						W.m_CharLen = NextPos - vCharPos[c];
						vWords.push_back(W);
					}
				}
				else
				{
					// stray char outside a segment: keep as plain char, no timing
					Text.push_back(*pCursor);
					++pCursor;
				}
			}

			while(!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
				Text.pop_back();
			if(Text.empty())
				continue;
			// trim leading whitespace (and its words)
			{
				int Lead = 0;
				while(Lead < (int)Text.size() && (Text[Lead] == ' ' || Text[Lead] == '\t'))
					Lead++;
				if(Lead > 0)
				{
					Text.erase(0, Lead);
					std::vector<SWord> vFixed;
					for(SWord &W : vWords)
					{
						if(W.m_CharBegin < Lead)
							continue;
						W.m_CharBegin -= Lead;
						vFixed.push_back(W);
					}
					vWords.swap(vFixed);
				}
			}
			if(Text.empty())
				continue;

			SLyricLine Line;
			Line.m_StartMs = LineStart;
			Line.m_Text = Text;
			Line.m_vWords = std::move(vWords);
			ConvertLyricScript(Line.m_Text, Traditional);
			// 逐字时轴按字节偏移索引 m_Text；简繁是 1:1 等长字符映射、偏移保持
			// 有效，万一映射结果不等长（异常）就回退原文，宁保原脚本也不破坏
			// 卡拉OK逐字偏移。
			if(Line.m_Text.size() != Text.size())
				Line.m_Text = Text;
			Line.m_IsSimplifiedChinese = IsSimplifiedChinese(Line.m_Text);
			vOut.push_back(std::move(Line));
		}

		if(vOut.empty())
			return false;
		std::stable_sort(vOut.begin(), vOut.end(), [](const SLyricLine &A, const SLyricLine &B) {
			return A.m_StartMs < B.m_StartMs;
		});
		return true;
	}

void AttachTranslatedLyrics(std::vector<SLyricLine> &vLines, const char *pTranslatedLyrics)
{
	std::vector<SLyricLine> vTranslations;
	if(vLines.empty() || !ParseSyncedLyrics(pTranslatedLyrics, vTranslations))
		return;

	struct SCandidate
	{
		size_t m_Index;
		uint64_t m_Distance = 251;
	};
	struct SGroup
	{
		size_t m_Begin, m_End;
		SCandidate m_First, m_Second;
	};
	const size_t None = vTranslations.size();
	std::vector<SGroup> vGroups;
	for(size_t Begin = 0; Begin < vLines.size();)
	{
		size_t End = Begin + 1;
		while(End < vLines.size() && vLines[End].m_StartMs == vLines[Begin].m_StartMs)
			++End;
		vGroups.push_back({Begin, End, {None, 251}, {None, 251}});
		Begin = End;
	}

	auto Gap = [](int64_t A, int64_t B) -> uint64_t {
		return A >= B ? (uint64_t)A - (uint64_t)B : (uint64_t)B - (uint64_t)A;
	};
	auto Better = [](const SCandidate &A, const SCandidate &B) {
		return A.m_Distance < B.m_Distance || (A.m_Distance == B.m_Distance && A.m_Index < B.m_Index);
	};
	// Both timelines are sorted. Advance over original timestamp groups once;
	// retain only the best two distinct texts so self-translations can be skipped
	// per original line without scanning the whole group for every translation.
	size_t After = 0;
	for(size_t i = 0; i < vTranslations.size(); ++i)
	{
		SLyricLine &Translation = vTranslations[i];
		while(After < vGroups.size() && vLines[vGroups[After].m_Begin].m_StartMs < Translation.m_StartMs)
			++After;
		const uint64_t NextGap = After < vGroups.size() ? Gap(vLines[vGroups[After].m_Begin].m_StartMs, Translation.m_StartMs) : std::numeric_limits<uint64_t>::max();
		const uint64_t PrevGap = After > 0 ? Gap(vLines[vGroups[After - 1].m_Begin].m_StartMs, Translation.m_StartMs) : std::numeric_limits<uint64_t>::max();
		if(NextGap == PrevGap || std::min(NextGap, PrevGap) > 250)
			continue; // equally near distinct timestamps are ambiguous, including duplicates
		if(!IsProviderChineseTranslation(Translation.m_Text))
			continue;
		Translation.m_Text = SimplifyChinese(Translation.m_Text);
		SGroup &Group = vGroups[NextGap < PrevGap ? After : After - 1];
		const SCandidate Candidate{i, std::min(NextGap, PrevGap)};
		if(Group.m_First.m_Index != None && Translation.m_Text == vTranslations[Group.m_First.m_Index].m_Text)
		{
			if(Better(Candidate, Group.m_First)) Group.m_First = Candidate;
		}
		else if(Group.m_Second.m_Index != None && Translation.m_Text == vTranslations[Group.m_Second.m_Index].m_Text)
		{
			if(Better(Candidate, Group.m_Second)) Group.m_Second = Candidate;
		}
		else if(Better(Candidate, Group.m_First))
		{
			Group.m_Second = Group.m_First;
			Group.m_First = Candidate;
		}
		else if(Better(Candidate, Group.m_Second))
			Group.m_Second = Candidate;
		if(Better(Group.m_Second, Group.m_First))
			std::swap(Group.m_First, Group.m_Second);
	}
	// Apply each group once. Existing ytlrc remains authoritative when tlyric
	// is attached later, even if the lower-priority field has a closer timestamp.
	for(const SGroup &Group : vGroups)
	{
		if(Group.m_First.m_Index == None)
			continue;
		for(size_t i = Group.m_Begin; i < Group.m_End; ++i)
		{
			SLyricLine &Line = vLines[i];
			if(!Line.m_Translation.empty())
				continue;
				const std::string ComparableLine = SimplifyChinese(Line.m_Text);
				const size_t Match = vTranslations[Group.m_First.m_Index].m_Text != ComparableLine ? Group.m_First.m_Index : Group.m_Second.m_Index;
			if(Match != None)
				Line.m_Translation = vTranslations[Match].m_Text;
		}
	}
}

} // namespace TcLyric
