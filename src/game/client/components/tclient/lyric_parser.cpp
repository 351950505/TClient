/* TClient: 歌词解析实现，见 lyric_parser.h */
#include "lyric_parser.h"

#include <algorithm>
#include <cctype>

namespace TcLyric {

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
			Minutes = Minutes * 10 + (*p - '0');
			++p;
		}
		if(*p != ':')
			return false;
		++p;
		if(!isdigit((unsigned char)*p))
			return false;
		while(isdigit((unsigned char)*p))
		{
			Seconds = Seconds * 10 + (*p - '0');
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

	bool ParseSyncedLyrics(const char *pSyncedLyrics, std::vector<SLyricLine> &vOut)
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

	bool ParseYrcLyrics(const char *pYrc, std::vector<SLyricLine> &vOut)
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
			Line.m_vWords = vWords;
			vOut.push_back(std::move(Line));
		}

		if(vOut.empty())
			return false;
		std::stable_sort(vOut.begin(), vOut.end(), [](const SLyricLine &A, const SLyricLine &B) {
			return A.m_StartMs < B.m_StartMs;
		});
		return true;
	}

} // namespace TcLyric
