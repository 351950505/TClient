#include "bg_draw_file.h"

#include <algorithm>
#include <cstring>

#define MAX_LINE_LENGTH 256
#define MAX_PARTS_PER_ITEM 4096

bool BgDrawFile::CLineReader::ReadLine(char *pBuf, int Length)
{
	if(Length < 2 || m_LimitExceeded)
		return false;
	int Len = 0;
	bool ReadAny = false, TooLong = false;
	while(true)
	{
		if(m_Pos == m_Size)
		{
			// Read one extra byte at the limit to distinguish EOF from overflow.
			const unsigned Request = std::min<unsigned>(sizeof(m_aBuffer), MAX_FILE_SIZE - m_Total + 1);
			m_Size = m_Read(m_aBuffer, Request);
			m_Pos = 0;
			if(m_Size == 0)
				break;
			if(m_Size > MAX_FILE_SIZE - m_Total)
			{
				m_LimitExceeded = true;
				return false;
			}
			m_Total += m_Size;
		}
		const char C = m_aBuffer[m_Pos++];
		ReadAny = true;
		if(C == '\n')
			break;
		if(Len < Length - 1)
			pBuf[Len++] = C;
		else
			TooLong = true;
	}
	if(!ReadAny)
		return false;
	if(TooLong)
	{
		pBuf[0] = '#';
		Len = 1;
	}
	while(Len > 0 && pBuf[Len - 1] == '\r')
		--Len;
	pBuf[Len] = '\0';
	return true;
}

bool BgDrawFile::Write(const std::function<bool(const char *)> &WriteLine, const CBgDrawItemData &Data)
{
	char aBuf[MAX_LINE_LENGTH];
	for(const CBgDrawItemDataPoint &Point : Data)
	{
		std::snprintf(aBuf, sizeof(aBuf), "%f,%f,%f,%f,%f,%f,%f",
			Point.x, Point.y, Point.w, Point.r, Point.g, Point.b, Point.a);
		if(!WriteLine(aBuf))
			return false;
	}
	if(!WriteLine(","))
		return false;
	return true;
}
bool BgDrawFile::Read(const std::function<bool(char *pBuf, int Length)> &ReadLine, CBgDrawItemData &Data)
{
	char aBuf[MAX_LINE_LENGTH];
	Data.clear();
	while(true)
	{
		if(!ReadLine(aBuf, sizeof(aBuf)))
			return !Data.empty();
		if(aBuf[0] == '\0' || aBuf[0] == ',')
			return !Data.empty();
		if(aBuf[0] == '#')
			continue;
		float x, y;
		float w = 5.0f;
		float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
		int Ret = std::sscanf(aBuf, "%f,%f,%f,%f,%f,%f,%f", &x, &y, &w, &r, &g, &b, &a);
		if(Ret < 2)
			continue; // Need x and y
		if(Data.size() < MAX_PARTS_PER_ITEM)
			Data.emplace_back(x, y, w, r, g, b, a);
	}
}
