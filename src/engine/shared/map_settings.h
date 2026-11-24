#ifndef ENGINE_SHARED_MAP_SETTINGS_H
#define ENGINE_SHARED_MAP_SETTINGS_H

#include <cstddef>
#include <cstring>

inline bool IsMapSettingsDataValid(const char *pData, int DataSize)
{
	if(pData == nullptr || DataSize <= 0)
		return false;

	const size_t Size = static_cast<size_t>(DataSize);
	size_t Offset = 0;
	while(Offset < Size)
	{
		const char *pTerminator = static_cast<const char *>(std::memchr(pData + Offset, '\0', Size - Offset));
		if(pTerminator == nullptr)
			return false;
		Offset = static_cast<size_t>(pTerminator - pData) + 1;
	}
	return true;
}

template<typename TVisitor>
bool ForEachMapSetting(const char *pData, int DataSize, TVisitor &&Visit)
{
	if(!IsMapSettingsDataValid(pData, DataSize))
		return false;

	const size_t Size = static_cast<size_t>(DataSize);
	size_t Offset = 0;
	while(Offset < Size)
	{
		const char *pCommand = pData + Offset;
		const char *pTerminator = static_cast<const char *>(std::memchr(pCommand, '\0', Size - Offset));
		const size_t CommandLength = static_cast<size_t>(pTerminator - pCommand);
		Visit(pCommand, CommandLength);
		Offset += CommandLength + 1;
	}
	return true;
}

#endif // ENGINE_SHARED_MAP_SETTINGS_H
