#ifndef ENGINE_CLIENT_AUTO_SESSION_LOG_H
#define ENGINE_CLIENT_AUTO_SESSION_LOG_H

#include <base/io.h>
#include <base/str.h>

namespace TClientAutoSessionLog
{
	inline bool IsFilename(const char *pFilename)
	{
		static constexpr char PREFIX[] = "ddnet-log-";
		static constexpr char SUFFIX[] = ".log";
		static constexpr int TIMESTAMP_LENGTH = 19; // %Y-%m-%d_%H-%M-%S
		static constexpr int LEGACY_FILENAME_LENGTH = sizeof(PREFIX) - 1 + TIMESTAMP_LENGTH + sizeof(SUFFIX) - 1;

		const int Length = str_length(pFilename);
		if(Length < LEGACY_FILENAME_LENGTH || !str_startswith(pFilename, PREFIX) || !str_endswith(pFilename, SUFFIX))
			return false;

		const char *pTimestamp = pFilename + sizeof(PREFIX) - 1;
		for(int i = 0; i < TIMESTAMP_LENGTH; ++i)
		{
			const bool IsSeparatorPosition = i == 4 || i == 7 || i == 10 || i == 13 || i == 16;
			if(IsSeparatorPosition)
			{
				const char Expected = i == 10 ? '_' : '-';
				if(pTimestamp[i] != Expected)
					return false;
			}
			else if(pTimestamp[i] < '0' || pTimestamp[i] > '9')
			{
				return false;
			}
		}

		const char *pTail = pTimestamp + TIMESTAMP_LENGTH;
		if(str_comp(pTail, SUFFIX) == 0)
			return true; // Legacy timestamp-only name.
		if(*pTail++ != '-' || *pTail < '0' || *pTail > '9')
			return false;
		while(*pTail >= '0' && *pTail <= '9')
			++pTail;
		return str_comp(pTail, SUFFIX) == 0;
	}

	inline void FormatFilename(char *pBuffer, int BufferSize, const char *pTimestamp, int ProcessId)
	{
		str_format(pBuffer, BufferSize, "ddnet-log-%s-%d.log", pTimestamp, ProcessId);
	}

	enum class EOpenLocation
	{
		NONE,
		INSTALL,
		STORAGE,
	};

	struct SOpenResult
	{
		IOHANDLE m_File = nullptr;
		EOpenLocation m_Location = EOpenLocation::NONE;
	};

	template<typename F>
	inline SOpenResult OpenFile(const char *pInstallPath, const char *pStoragePath, int Flags, F BeforeStorageOpen)
	{
		if(pInstallPath != nullptr && pInstallPath[0] != '\0')
		{
			IOHANDLE File = io_open(pInstallPath, Flags);
			if(File)
				return {File, EOpenLocation::INSTALL};
		}
		BeforeStorageOpen();
		if(pStoragePath != nullptr && pStoragePath[0] != '\0')
		{
			IOHANDLE File = io_open(pStoragePath, Flags);
			if(File)
				return {File, EOpenLocation::STORAGE};
		}
		return {};
	}

	inline SOpenResult OpenFile(const char *pInstallPath, const char *pStoragePath, int Flags)
	{
		return OpenFile(pInstallPath, pStoragePath, Flags, [] {});
	}
}

#endif // ENGINE_CLIENT_AUTO_SESSION_LOG_H
