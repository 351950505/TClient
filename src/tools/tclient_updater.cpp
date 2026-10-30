/* (c) TClient. See licence.txt in the root of the distribution for more information. */

// TClientUpdater.exe - self-contained install helper for the TClient autoupdater.
//
// The client downloads the update archive and then spawns this helper with its own process id,
// after which the client exits. This helper waits for the client to terminate, unpacks the
// archive into a staging directory next to the installation, backs up the files that are about
// to be replaced and then moves the new files into place. If anything goes wrong the previous
// state is restored from the backup.
//
// Usage (all arguments are required):
//   TClientUpdater.exe
//     --pid <client pid>            pid of the running client, 0 to skip waiting
//     --zip <absolute zip path>     downloaded update archive
//     --sha256 <64 hex chars>       expected sha256 of the zip file
//     --target <absolute dir>       client installation directory
//     --restart <absolute exe>      executable that is started after a successful update
//     --log <absolute log path>     log file, appended to
//
// Exit codes:
//   0  success, the restart executable has been started
//   1  usage error, invalid or missing arguments, target directory or zip missing
//   2  zip file could not be opened or is not a valid zip archive (includes encrypted entries,
//      entries using a data descriptor, unsupported compression methods and symlink entries,
//      all of which are rejected while reading the central directory)
//   3  sha256 of the zip file does not match --sha256
//   4  timeout while waiting for the --pid process to exit (the process is never killed)
//   5  installing failed, the previous state was restored from the backup
//   6  unsafe entry name (absolute path, drive letter, "..", reserved device name, duplicate
//      path) or an entry that could not be extracted
//   7  file system error while preparing the staging directory or the backup
//   8  the update was installed but the restart executable could not be started
//
// Only method 0 (stored) and method 8 (deflate) zip entries are supported. Zip64 archives are
// supported as long as the entries themselves use one of those two methods.
//
// Files that belong to the user (cfg/, demos/, screenshots/, themes/, downloadedmaps/, tclient/,
// storage.cfg, settings_*.cfg, *.log, *.dmp) are never touched: such entries are skipped and
// logged, everything else in the archive is installed.

#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <set>
#include <string>
#include <vector>

namespace
{

	constexpr int EXIT_OK = 0;
	constexpr int EXIT_USAGE = 1;
	constexpr int EXIT_ZIP_INVALID = 2;
	constexpr int EXIT_SHA_MISMATCH = 3;
	constexpr int EXIT_PID_TIMEOUT = 4;
	constexpr int EXIT_INSTALL_FAILED = 5;
	constexpr int EXIT_UNSAFE_ENTRY = 6;
	constexpr int EXIT_FILE_SYSTEM = 7;
	constexpr int EXIT_RESTART_FAILED = 8;

	// Waiting for the client to exit, never kill it. The environment variable
	// TCLIENT_UPDATER_WAIT_TIMEOUT_MS may only shorten this limit (it is clamped to the value below),
	// which is used by the automated tests so they do not have to wait five minutes.
	constexpr DWORD WAIT_FOR_PID_TIMEOUT_MS = 300 * 1000;

	DWORD GetWaitTimeout()
	{
		wchar_t aValue[32];
		const DWORD Length = GetEnvironmentVariableW(L"TCLIENT_UPDATER_WAIT_TIMEOUT_MS", aValue, (DWORD)(sizeof(aValue) / sizeof(aValue[0])));
		if(Length == 0 || Length >= sizeof(aValue) / sizeof(aValue[0]))
			return WAIT_FOR_PID_TIMEOUT_MS;
		wchar_t *pEnd = nullptr;
		const unsigned long Parsed = wcstoul(aValue, &pEnd, 10);
		if(pEnd == aValue || *pEnd != L'\0' || Parsed < 1000)
			return WAIT_FOR_PID_TIMEOUT_MS;
		return (DWORD)std::min<unsigned long>(Parsed, WAIT_FOR_PID_TIMEOUT_MS);
	}

	// Sanity limits so a malicious archive cannot fill the disk.
	constexpr uint64_t MAX_ENTRY_UNCOMPRESSED_SIZE = 2ull * 1024 * 1024 * 1024;
	constexpr uint64_t MAX_TOTAL_UNCOMPRESSED_SIZE = 8ull * 1024 * 1024 * 1024;

	//////////////////////////////////////////////////////////////////////////
	// small string helpers
	//////////////////////////////////////////////////////////////////////////

	std::wstring WideFromUtf8(const std::string &Text)
	{
		if(Text.empty())
			return std::wstring();
		const int Length = MultiByteToWideChar(CP_UTF8, 0, Text.data(), (int)Text.size(), nullptr, 0);
		if(Length <= 0)
			return std::wstring();
		std::wstring Result((size_t)Length, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, Text.data(), (int)Text.size(), Result.data(), Length);
		return Result;
	}

	std::string Utf8FromWide(const std::wstring &Text)
	{
		if(Text.empty())
			return std::string();
		const int Length = WideCharToMultiByte(CP_UTF8, 0, Text.data(), (int)Text.size(), nullptr, 0, nullptr, nullptr);
		if(Length <= 0)
			return std::string();
		std::string Result((size_t)Length, '\0');
		WideCharToMultiByte(CP_UTF8, 0, Text.data(), (int)Text.size(), Result.data(), Length, nullptr, nullptr);
		return Result;
	}

	std::wstring ToLowerWide(const std::wstring &Text)
	{
		std::wstring Result = Text;
		for(wchar_t &Character : Result)
		{
			if(Character >= L'A' && Character <= L'Z')
				Character = (wchar_t)(Character - L'A' + L'a');
		}
		return Result;
	}

	bool EndsWithNoCase(const std::wstring &Text, const wchar_t *pSuffix)
	{
		const size_t SuffixLength = wcslen(pSuffix);
		if(Text.size() < SuffixLength)
			return false;
		return ToLowerWide(Text.substr(Text.size() - SuffixLength)) == pSuffix;
	}

	bool StartsWithNoCase(const std::wstring &Text, const wchar_t *pPrefix)
	{
		const size_t PrefixLength = wcslen(pPrefix);
		if(Text.size() < PrefixLength)
			return false;
		return ToLowerWide(Text.substr(0, PrefixLength)) == pPrefix;
	}

	std::wstring JoinPath(const std::wstring &Left, const std::wstring &Right)
	{
		if(Left.empty())
			return Right;
		if(Left.back() == L'\\' || Left.back() == L'/')
			return Left + Right;
		return Left + L"\\" + Right;
	}

	// Prefixes a fully qualified path with the \\?\ marker so that paths longer than MAX_PATH keep
	// working with the file APIs. All file system calls below go through this helper.
	std::wstring LongPath(const std::wstring &Path)
	{
		if(Path.size() >= 4 && Path.compare(0, 4, L"\\\\?\\") == 0)
			return Path;
		if(Path.size() >= 2 && Path[0] == L'\\' && Path[1] == L'\\')
			return L"\\\\?\\UNC\\" + Path.substr(2);
		return L"\\\\?\\" + Path;
	}

	std::wstring GetFullPath(const std::wstring &Path)
	{
		const DWORD Length = GetFullPathNameW(Path.c_str(), 0, nullptr, nullptr);
		if(Length == 0)
			return Path;
		std::wstring Result((size_t)Length, L'\0');
		const DWORD Written = GetFullPathNameW(Path.c_str(), Length, Result.data(), nullptr);
		if(Written == 0 || Written >= Length)
			return Path;
		Result.resize((size_t)Written);
		return Result;
	}

	//////////////////////////////////////////////////////////////////////////
	// logging
	//////////////////////////////////////////////////////////////////////////

	std::string FormatLastError(DWORD Error)
	{
		char *pBuffer = nullptr;
		const DWORD Length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&pBuffer, 0, nullptr);
		std::string Result;
		char aNumber[64];
		snprintf(aNumber, sizeof(aNumber), " (error %lu)", (unsigned long)Error);
		if(Length != 0 && pBuffer != nullptr)
		{
			Result.assign(pBuffer, Length);
			while(!Result.empty() && (Result.back() == '\r' || Result.back() == '\n' || Result.back() == ' '))
				Result.pop_back();
			Result += aNumber;
		}
		else
		{
			Result = "unknown error";
			Result += aNumber;
		}
		if(pBuffer != nullptr)
			LocalFree(pBuffer);
		return Result;
	}

	class CLogger
	{
	public:
		bool Open(const std::wstring &Path)
		{
			Close();
			m_hFile = CreateFileW(LongPath(Path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			return m_hFile != INVALID_HANDLE_VALUE;
		}

		void Close()
		{
			if(m_hFile != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_hFile);
				m_hFile = INVALID_HANDLE_VALUE;
			}
		}

		void Info(const char *pFormat, ...)
		{
			va_list Args;
			va_start(Args, pFormat);
			Write("INFO", pFormat, Args);
			va_end(Args);
		}

		void Warn(const char *pFormat, ...)
		{
			va_list Args;
			va_start(Args, pFormat);
			Write("WARN", pFormat, Args);
			va_end(Args);
		}

		void Error(const char *pFormat, ...)
		{
			va_list Args;
			va_start(Args, pFormat);
			Write("ERROR", pFormat, Args);
			va_end(Args);
		}

	private:
		void Write(const char *pLevel, const char *pFormat, va_list Args)
		{
			SYSTEMTIME Time;
			GetLocalTime(&Time);

			char aMessage[4096];
			vsnprintf(aMessage, sizeof(aMessage), pFormat, Args);
			aMessage[sizeof(aMessage) - 1] = '\0';

			char aLine[4300];
			const int LineLength = snprintf(aLine, sizeof(aLine), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\r\n",
				(unsigned)Time.wYear, (unsigned)Time.wMonth, (unsigned)Time.wDay,
				(unsigned)Time.wHour, (unsigned)Time.wMinute, (unsigned)Time.wSecond, (unsigned)Time.wMilliseconds,
				pLevel, aMessage);
			if(LineLength <= 0)
				return;

			const DWORD WriteLength = (DWORD)std::min<int>(LineLength, (int)sizeof(aLine) - 1);
			if(m_hFile != INVALID_HANDLE_VALUE)
			{
				DWORD Written = 0;
				WriteFile(m_hFile, aLine, WriteLength, &Written, nullptr);
			}
			fwrite(aLine, 1, WriteLength, stderr);
			fflush(stderr);
		}

		HANDLE m_hFile = INVALID_HANDLE_VALUE;
	};

	CLogger g_Logger;

	//////////////////////////////////////////////////////////////////////////
	// SHA-256 (self contained implementation)
	//////////////////////////////////////////////////////////////////////////

	class CSha256
	{
	public:
		void Init()
		{
			memcpy(m_aState, S_INITIAL_STATE, sizeof(m_aState));
			m_TotalLength = 0;
			m_BufferLength = 0;
		}

		void Update(const void *pData, size_t Length)
		{
			const unsigned char *pBytes = (const unsigned char *)pData;
			m_TotalLength += Length;
			while(Length > 0)
			{
				const size_t Space = sizeof(m_aBuffer) - m_BufferLength;
				const size_t Take = std::min(Space, Length);
				memcpy(m_aBuffer + m_BufferLength, pBytes, Take);
				m_BufferLength += Take;
				pBytes += Take;
				Length -= Take;
				if(m_BufferLength == sizeof(m_aBuffer))
				{
					Transform(m_aBuffer);
					m_BufferLength = 0;
				}
			}
		}

		void Finish(unsigned char *pDigest)
		{
			const uint64_t BitLength = m_TotalLength * 8;
			unsigned char aPadding[128];
			memset(aPadding, 0, sizeof(aPadding));
			aPadding[0] = 0x80;
			const size_t PaddingLength = (m_BufferLength < 56) ? (56 - m_BufferLength) : (120 - m_BufferLength);
			for(int i = 0; i < 8; i++)
				aPadding[PaddingLength + (size_t)i] = (unsigned char)(BitLength >> (56 - 8 * i));
			Update(aPadding, PaddingLength + 8);
			for(int i = 0; i < 8; i++)
			{
				pDigest[i * 4 + 0] = (unsigned char)(m_aState[i] >> 24);
				pDigest[i * 4 + 1] = (unsigned char)(m_aState[i] >> 16);
				pDigest[i * 4 + 2] = (unsigned char)(m_aState[i] >> 8);
				pDigest[i * 4 + 3] = (unsigned char)(m_aState[i]);
			}
		}

	private:
		static uint32_t RotateRight(uint32_t Value, int Bits)
		{
			return (Value >> Bits) | (Value << (32 - Bits));
		}

		void Transform(const unsigned char *pBlock)
		{
			uint32_t aWords[64];
			for(int i = 0; i < 16; i++)
			{
				aWords[i] = ((uint32_t)pBlock[i * 4 + 0] << 24) | ((uint32_t)pBlock[i * 4 + 1] << 16) |
					    ((uint32_t)pBlock[i * 4 + 2] << 8) | ((uint32_t)pBlock[i * 4 + 3]);
			}
			for(int i = 16; i < 64; i++)
			{
				const uint32_t S0 = RotateRight(aWords[i - 15], 7) ^ RotateRight(aWords[i - 15], 18) ^ (aWords[i - 15] >> 3);
				const uint32_t S1 = RotateRight(aWords[i - 2], 17) ^ RotateRight(aWords[i - 2], 19) ^ (aWords[i - 2] >> 10);
				aWords[i] = aWords[i - 16] + S0 + aWords[i - 7] + S1;
			}

			uint32_t A = m_aState[0];
			uint32_t B = m_aState[1];
			uint32_t C = m_aState[2];
			uint32_t D = m_aState[3];
			uint32_t E = m_aState[4];
			uint32_t F = m_aState[5];
			uint32_t G = m_aState[6];
			uint32_t H = m_aState[7];

			for(int i = 0; i < 64; i++)
			{
				const uint32_t S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
				const uint32_t Choice = (E & F) ^ (~E & G);
				const uint32_t Temp1 = H + S1 + Choice + S_CONSTANTS[i] + aWords[i];
				const uint32_t S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
				const uint32_t Majority = (A & B) ^ (A & C) ^ (B & C);
				const uint32_t Temp2 = S0 + Majority;
				H = G;
				G = F;
				F = E;
				E = D + Temp1;
				D = C;
				C = B;
				B = A;
				A = Temp1 + Temp2;
			}

			m_aState[0] += A;
			m_aState[1] += B;
			m_aState[2] += C;
			m_aState[3] += D;
			m_aState[4] += E;
			m_aState[5] += F;
			m_aState[6] += G;
			m_aState[7] += H;
		}

		static const uint32_t S_INITIAL_STATE[8];
		static const uint32_t S_CONSTANTS[64];

		uint32_t m_aState[8];
		uint64_t m_TotalLength;
		unsigned char m_aBuffer[64];
		size_t m_BufferLength;
	};

	const uint32_t CSha256::S_INITIAL_STATE[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

	const uint32_t CSha256::S_CONSTANTS[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

	bool Sha256File(const std::wstring &Path, std::string &OutHex, std::string &Error)
	{
		HANDLE hFile = CreateFileW(LongPath(Path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if(hFile == INVALID_HANDLE_VALUE)
		{
			Error = "could not open file for hashing: " + FormatLastError(GetLastError());
			return false;
		}

		CSha256 Hash;
		Hash.Init();
		std::vector<unsigned char> Buffer(256 * 1024);
		for(;;)
		{
			DWORD Read = 0;
			if(!ReadFile(hFile, Buffer.data(), (DWORD)Buffer.size(), &Read, nullptr))
			{
				Error = "could not read file for hashing: " + FormatLastError(GetLastError());
				CloseHandle(hFile);
				return false;
			}
			if(Read == 0)
				break;
			Hash.Update(Buffer.data(), Read);
		}
		CloseHandle(hFile);

		unsigned char aDigest[32];
		Hash.Finish(aDigest);
		static const char *S_HEX = "0123456789abcdef";
		OutHex.clear();
		for(unsigned char Byte : aDigest)
		{
			OutHex.push_back(S_HEX[Byte >> 4]);
			OutHex.push_back(S_HEX[Byte & 0x0F]);
		}
		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	// zip archive reading
	//////////////////////////////////////////////////////////////////////////

	uint16_t ReadU16(const unsigned char *pData)
	{
		return (uint16_t)((uint16_t)pData[0] | ((uint16_t)pData[1] << 8));
	}

	uint32_t ReadU32(const unsigned char *pData)
	{
		return ((uint32_t)pData[0]) | ((uint32_t)pData[1] << 8) | ((uint32_t)pData[2] << 16) | ((uint32_t)pData[3] << 24);
	}

	uint64_t ReadU64(const unsigned char *pData)
	{
		return ((uint64_t)ReadU32(pData)) | ((uint64_t)ReadU32(pData + 4) << 32);
	}

	struct SZipEntry
	{
		std::wstring m_LogName; // original name with forward slashes, for log output
		uint16_t m_Method = 0;
		uint32_t m_Crc32 = 0;
		uint64_t m_CompressedSize = 0;
		uint64_t m_UncompressedSize = 0;
		uint64_t m_LocalHeaderOffset = 0;
	};

	class CZipArchive
	{
	public:
		~CZipArchive() { Close(); }

		bool Open(const std::wstring &Path, std::string &Error);
		void Close();

		const std::vector<SZipEntry> &Entries() const { return m_Entries; }
		const unsigned char *Data() const { return m_pData; }

		// Resolves the local file header of an entry and returns the offset of the first
		// compressed byte. Fails if the local header disagrees with the central directory.
		bool GetEntryDataOffset(const SZipEntry &Entry, uint64_t &OutOffset, std::string &Error) const;

	private:
		bool ParseCentralDirectory(std::string &Error);
		bool ParseZip64EndOfCentralDirectory(size_t EocdPosition, uint64_t &OutEntries, uint64_t &OutSize, uint64_t &OutOffset, std::string &Error) const;

		HANDLE m_hFile = INVALID_HANDLE_VALUE;
		HANDLE m_hMapping = nullptr;
		const unsigned char *m_pData = nullptr;
		uint64_t m_Size = 0;
		std::vector<SZipEntry> m_Entries;
	};

	void CZipArchive::Close()
	{
		if(m_pData != nullptr)
		{
			UnmapViewOfFile(m_pData);
			m_pData = nullptr;
		}
		if(m_hMapping != nullptr)
		{
			CloseHandle(m_hMapping);
			m_hMapping = nullptr;
		}
		if(m_hFile != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_hFile);
			m_hFile = INVALID_HANDLE_VALUE;
		}
		m_Size = 0;
		m_Entries.clear();
	}

	bool CZipArchive::Open(const std::wstring &Path, std::string &Error)
	{
		Close();

		m_hFile = CreateFileW(LongPath(Path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(m_hFile == INVALID_HANDLE_VALUE)
		{
			Error = "could not open zip file: " + FormatLastError(GetLastError());
			return false;
		}

		LARGE_INTEGER FileSize;
		if(!GetFileSizeEx(m_hFile, &FileSize) || FileSize.QuadPart <= 0)
		{
			Error = "zip file is empty or its size could not be determined";
			return false;
		}
		m_Size = (uint64_t)FileSize.QuadPart;

		m_hMapping = CreateFileMappingW(m_hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if(m_hMapping == nullptr)
		{
			Error = "could not map zip file: " + FormatLastError(GetLastError());
			return false;
		}
		m_pData = (const unsigned char *)MapViewOfFile(m_hMapping, FILE_MAP_READ, 0, 0, 0);
		if(m_pData == nullptr)
		{
			Error = "could not map zip file: " + FormatLastError(GetLastError());
			return false;
		}

		return ParseCentralDirectory(Error);
	}

	bool CZipArchive::ParseZip64EndOfCentralDirectory(size_t EocdPosition, uint64_t &OutEntries, uint64_t &OutSize, uint64_t &OutOffset, std::string &Error) const
	{
		if(EocdPosition < 20)
		{
			Error = "zip64 end of central directory locator is missing";
			return false;
		}
		const unsigned char *pLocator = m_pData + EocdPosition - 20;
		if(ReadU32(pLocator) != 0x07064b50)
		{
			Error = "zip64 end of central directory locator is missing";
			return false;
		}
		const uint64_t Zip64EocdOffset = ReadU64(pLocator + 8);
		if(Zip64EocdOffset > m_Size || m_Size - Zip64EocdOffset < 56)
		{
			Error = "zip64 end of central directory is out of bounds";
			return false;
		}
		const unsigned char *pRecord = m_pData + Zip64EocdOffset;
		if(ReadU32(pRecord) != 0x06064b50)
		{
			Error = "zip64 end of central directory signature is invalid";
			return false;
		}
		OutEntries = ReadU64(pRecord + 32);
		OutSize = ReadU64(pRecord + 40);
		OutOffset = ReadU64(pRecord + 48);
		return true;
	}

	bool CZipArchive::ParseCentralDirectory(std::string &Error)
	{
		// The end of central directory record is at most 22 bytes plus a 65535 byte comment.
		const uint64_t MaxSearch = std::min<uint64_t>(m_Size, 22 + 65535);
		size_t EocdPosition = 0;
		bool Found = false;
		for(uint64_t Offset = 0; Offset + 22 <= MaxSearch; Offset++)
		{
			const size_t Position = (size_t)(m_Size - 22 - Offset);
			if(ReadU32(m_pData + Position) != 0x06054b50)
				continue;
			const uint16_t CommentLength = ReadU16(m_pData + Position + 20);
			if((uint64_t)Position + 22 + CommentLength > m_Size)
				continue;
			EocdPosition = Position;
			Found = true;
			break;
		}
		if(!Found)
		{
			Error = "end of central directory record not found, not a zip file";
			return false;
		}

		uint64_t EntryCount = ReadU16(m_pData + EocdPosition + 10);
		uint64_t CentralDirectorySize = ReadU32(m_pData + EocdPosition + 12);
		uint64_t CentralDirectoryOffset = ReadU32(m_pData + EocdPosition + 16);

		if(EntryCount == 0xFFFF || CentralDirectorySize == 0xFFFFFFFF || CentralDirectoryOffset == 0xFFFFFFFF)
		{
			if(!ParseZip64EndOfCentralDirectory(EocdPosition, EntryCount, CentralDirectorySize, CentralDirectoryOffset, Error))
				return false;
		}

		if(CentralDirectoryOffset > m_Size || CentralDirectorySize > m_Size - CentralDirectoryOffset)
		{
			Error = "central directory is out of bounds";
			return false;
		}
		if(EntryCount > 1000000)
		{
			Error = "zip archive declares an unreasonable number of entries";
			return false;
		}

		m_Entries.clear();
		m_Entries.reserve((size_t)std::min<uint64_t>(EntryCount, 65536));

		uint64_t Position = CentralDirectoryOffset;
		uint64_t TotalUncompressed = 0;
		for(uint64_t Index = 0; Index < EntryCount; Index++)
		{
			if(Position + 46 > m_Size || ReadU32(m_pData + (size_t)Position) != 0x02014b50)
			{
				Error = "central directory entry is truncated or has an invalid signature";
				return false;
			}
			const unsigned char *pHeader = m_pData + (size_t)Position;
			const uint16_t VersionMadeBy = ReadU16(pHeader + 4);
			const uint16_t Flags = ReadU16(pHeader + 8);
			const uint16_t Method = ReadU16(pHeader + 10);
			const uint32_t Crc = ReadU32(pHeader + 16);
			uint64_t CompressedSize = ReadU32(pHeader + 20);
			uint64_t UncompressedSize = ReadU32(pHeader + 24);
			const uint16_t NameLength = ReadU16(pHeader + 28);
			const uint16_t ExtraLength = ReadU16(pHeader + 30);
			const uint16_t CommentLength = ReadU16(pHeader + 32);
			const uint32_t ExternalAttributes = ReadU32(pHeader + 38);
			uint64_t LocalHeaderOffset = ReadU32(pHeader + 42);

			const uint64_t EntryLength = 46ull + NameLength + ExtraLength + CommentLength;
			if(Position + EntryLength > m_Size)
			{
				Error = "central directory entry exceeds the archive size";
				return false;
			}

			const unsigned char *pName = pHeader + 46;
			const unsigned char *pExtra = pName + NameLength;

			// Zip64 extended information: values are only present for the fields that are 0xFFFFFFFF.
			uint64_t ExtraPosition = 0;
			while(ExtraPosition + 4 <= ExtraLength)
			{
				const uint16_t HeaderId = ReadU16(pExtra + ExtraPosition);
				const uint16_t DataSize = ReadU16(pExtra + ExtraPosition + 2);
				if(ExtraPosition + 4 + DataSize > ExtraLength)
					break;
				if(HeaderId == 0x0001)
				{
					uint64_t DataPosition = ExtraPosition + 4;
					const uint64_t DataEnd = DataPosition + DataSize;
					if(UncompressedSize == 0xFFFFFFFF && DataPosition + 8 <= DataEnd)
					{
						UncompressedSize = ReadU64(pExtra + DataPosition);
						DataPosition += 8;
					}
					if(CompressedSize == 0xFFFFFFFF && DataPosition + 8 <= DataEnd)
					{
						CompressedSize = ReadU64(pExtra + DataPosition);
						DataPosition += 8;
					}
					if(LocalHeaderOffset == 0xFFFFFFFF && DataPosition + 8 <= DataEnd)
					{
						LocalHeaderOffset = ReadU64(pExtra + DataPosition);
						DataPosition += 8;
					}
				}
				ExtraPosition += 4ull + DataSize;
			}

			SZipEntry Entry;
			Entry.m_LogName = WideFromUtf8(std::string((const char *)pName, NameLength));
			std::replace(Entry.m_LogName.begin(), Entry.m_LogName.end(), L'\\', L'/');
			Entry.m_Method = Method;
			Entry.m_Crc32 = Crc;
			Entry.m_CompressedSize = CompressedSize;
			Entry.m_UncompressedSize = UncompressedSize;
			Entry.m_LocalHeaderOffset = LocalHeaderOffset;

			// Reject everything that cannot be installed safely. The name itself is validated
			// before extraction so that the error message can mention the offending entry.
			if((Flags & 0x0001) != 0)
			{
				Error = "entry '" + Utf8FromWide(Entry.m_LogName) + "' is encrypted, which is not supported";
				return false;
			}
			if((Flags & 0x0008) != 0)
			{
				Error = "entry '" + Utf8FromWide(Entry.m_LogName) + "' uses a data descriptor, which is not supported";
				return false;
			}
			if(Method != 0 && Method != 8)
			{
				char aBuffer[512];
				snprintf(aBuffer, sizeof(aBuffer), "entry '%s' uses unsupported compression method %u", Utf8FromWide(Entry.m_LogName).c_str(), (unsigned)Method);
				Error = aBuffer;
				return false;
			}
			// Unix host (high byte of version made by == 3): only regular files and directories.
			if((VersionMadeBy >> 8) == 3)
			{
				const uint32_t Mode = ExternalAttributes >> 16;
				const uint32_t FileType = Mode & 0xF000;
				if(FileType != 0 && FileType != 0x8000 && FileType != 0x4000)
				{
					Error = "entry '" + Utf8FromWide(Entry.m_LogName) + "' is not a regular file or directory";
					return false;
				}
			}
			if(Entry.m_UncompressedSize > MAX_ENTRY_UNCOMPRESSED_SIZE)
			{
				Error = "entry '" + Utf8FromWide(Entry.m_LogName) + "' is larger than the supported limit";
				return false;
			}
			TotalUncompressed += Entry.m_UncompressedSize;
			if(TotalUncompressed > MAX_TOTAL_UNCOMPRESSED_SIZE)
			{
				Error = "archive expands to more than the supported limit";
				return false;
			}

			m_Entries.push_back(std::move(Entry));
			Position += EntryLength;
		}

		return true;
	}

	bool CZipArchive::GetEntryDataOffset(const SZipEntry &Entry, uint64_t &OutOffset, std::string &Error) const
	{
		if(Entry.m_LocalHeaderOffset > m_Size || m_Size - Entry.m_LocalHeaderOffset < 30)
		{
			Error = "local file header is out of bounds";
			return false;
		}
		const unsigned char *pHeader = m_pData + (size_t)Entry.m_LocalHeaderOffset;
		if(ReadU32(pHeader) != 0x04034b50)
		{
			Error = "local file header has an invalid signature";
			return false;
		}
		const uint16_t Flags = ReadU16(pHeader + 6);
		const uint16_t Method = ReadU16(pHeader + 8);
		const uint16_t NameLength = ReadU16(pHeader + 26);
		const uint16_t ExtraLength = ReadU16(pHeader + 28);
		if((Flags & 0x0001) != 0)
		{
			Error = "local file header is encrypted";
			return false;
		}
		if((Flags & 0x0008) != 0)
		{
			Error = "local file header uses a data descriptor";
			return false;
		}
		if(Method != Entry.m_Method)
		{
			Error = "local file header disagrees with the central directory about the compression method";
			return false;
		}
		const uint64_t DataOffset = Entry.m_LocalHeaderOffset + 30 + NameLength + ExtraLength;
		if(DataOffset > m_Size || Entry.m_CompressedSize > m_Size - DataOffset)
		{
			Error = "entry data is out of bounds";
			return false;
		}
		OutOffset = DataOffset;
		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	// entry name validation
	//////////////////////////////////////////////////////////////////////////

	bool IsReservedDeviceName(const std::wstring &Segment)
	{
		std::wstring Base = Segment;
		const size_t Dot = Base.find(L'.');
		if(Dot != std::wstring::npos)
			Base = Base.substr(0, Dot);
		Base = ToLowerWide(Base);
		static const wchar_t *S_NAMES[] = {L"con", L"prn", L"aux", L"nul",
			L"com1", L"com2", L"com3", L"com4", L"com5", L"com6", L"com7", L"com8", L"com9",
			L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"};
		for(const wchar_t *pName : S_NAMES)
		{
			if(Base == pName)
				return true;
		}
		return false;
	}

	// Paths that belong to the user and must never be replaced by an update.
	bool IsProtectedUserPath(const std::wstring &SlashedName)
	{
		static const wchar_t *S_PROTECTED_PREFIXES[] = {
			L"cfg/",
			L"demos/",
			L"downloadedmaps/",
			L"screenshots/",
			L"tclient/",
			L"themes/",
			L"update/",
			L"update_backup_",
			L"update_staging_",
		};
		for(const wchar_t *pPrefix : S_PROTECTED_PREFIXES)
		{
			if(StartsWithNoCase(SlashedName, pPrefix))
				return true;
		}

		const size_t Slash = SlashedName.find_last_of(L'/');
		const std::wstring Base = (Slash == std::wstring::npos) ? SlashedName : SlashedName.substr(Slash + 1);
		const std::wstring LowerBase = ToLowerWide(Base);
		if(LowerBase == L"storage.cfg")
			return true;
		if(StartsWithNoCase(LowerBase, L"settings_") && EndsWithNoCase(LowerBase, L".cfg"))
			return true;
		// Runtime artifacts, includes the updater's own log file.
		if(EndsWithNoCase(LowerBase, L".log") || EndsWithNoCase(LowerBase, L".dmp"))
			return true;
		return false;
	}

	enum class EEntryCheck
	{
		OK,
		SKIP_PROTECTED,
		REJECT,
	};

	EEntryCheck CheckEntryName(const std::wstring &Name, bool &OutIsDirectory, std::wstring &OutRelativePath, std::string &Error)
	{
		OutIsDirectory = false;
		OutRelativePath.clear();

		if(Name.empty())
		{
			Error = "entry has an empty name";
			return EEntryCheck::REJECT;
		}
		if(Name.find(L'\0') != std::wstring::npos)
		{
			Error = "entry name contains a null byte";
			return EEntryCheck::REJECT;
		}

		std::wstring Slashed = Name;
		std::replace(Slashed.begin(), Slashed.end(), L'\\', L'/');
		if(Slashed.back() == L'/')
		{
			OutIsDirectory = true;
			Slashed.pop_back();
		}
		if(Slashed.empty())
		{
			Error = "entry has an empty name";
			return EEntryCheck::REJECT;
		}
		if(Slashed[0] == L'/')
		{
			Error = "entry uses an absolute path";
			return EEntryCheck::REJECT;
		}
		if(Slashed.size() >= 2 && Slashed[1] == L':')
		{
			Error = "entry uses a drive letter";
			return EEntryCheck::REJECT;
		}

		std::wstring Result;
		size_t Start = 0;
		while(Start <= Slashed.size())
		{
			size_t End = Slashed.find(L'/', Start);
			if(End == std::wstring::npos)
				End = Slashed.size();
			const std::wstring Segment = Slashed.substr(Start, End - Start);
			if(Segment.empty())
			{
				Error = "entry name contains an empty path segment";
				return EEntryCheck::REJECT;
			}
			if(Segment == L"." || Segment == L"..")
			{
				Error = "entry name contains a relative path segment";
				return EEntryCheck::REJECT;
			}
			for(wchar_t Character : Segment)
			{
				if(Character < 0x20 || Character == L':' || Character == L'*' || Character == L'?' ||
					Character == L'"' || Character == L'<' || Character == L'>' || Character == L'|')
				{
					Error = "entry name contains a character that is not allowed on Windows";
					return EEntryCheck::REJECT;
				}
			}
			// Windows silently strips trailing dots and spaces, which would make different
			// entries collide on disk.
			if(Segment.back() == L'.' || Segment.back() == L' ')
			{
				Error = "entry name segment ends with a dot or a space";
				return EEntryCheck::REJECT;
			}
			if(Segment.size() > 255)
			{
				Error = "entry name segment is too long";
				return EEntryCheck::REJECT;
			}
			if(IsReservedDeviceName(Segment))
			{
				Error = "entry name uses a reserved Windows device name";
				return EEntryCheck::REJECT;
			}
			if(!Result.empty())
				Result.push_back(L'\\');
			Result += Segment;
			if(End == Slashed.size())
				break;
			Start = End + 1;
		}

		if(Result.empty())
		{
			Error = "entry has an empty name";
			return EEntryCheck::REJECT;
		}
		if(Result.size() > 32000)
		{
			Error = "entry name is too long";
			return EEntryCheck::REJECT;
		}

		OutRelativePath = Result;
		if(IsProtectedUserPath(Slashed))
			return EEntryCheck::SKIP_PROTECTED;
		return EEntryCheck::OK;
	}

	//////////////////////////////////////////////////////////////////////////
	// file system helpers
	//////////////////////////////////////////////////////////////////////////

	bool DirectoryExists(const std::wstring &Path)
	{
		const DWORD Attributes = GetFileAttributesW(LongPath(Path).c_str());
		return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	bool FileExists(const std::wstring &Path)
	{
		const DWORD Attributes = GetFileAttributesW(LongPath(Path).c_str());
		return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	// Creates Path and all missing parents. Path must be absolute.
	bool EnsureDirectory(const std::wstring &Path, std::string &Error)
	{
		if(Path.empty())
		{
			Error = "empty directory path";
			return false;
		}
		if(DirectoryExists(Path))
			return true;

		std::wstring Current;
		size_t Start = 0;
		// Keep the "C:" or "\\server\share" part intact.
		if(Path.size() >= 2 && Path[1] == L':')
		{
			Current = Path.substr(0, 2);
			Start = 2;
		}
		else if(Path.size() >= 2 && Path[0] == L'\\' && Path[1] == L'\\')
		{
			const size_t Third = Path.find(L'\\', 2);
			const size_t Fourth = Third == std::wstring::npos ? std::wstring::npos : Path.find(L'\\', Third + 1);
			if(Fourth == std::wstring::npos)
			{
				Error = "invalid UNC directory path";
				return false;
			}
			Current = Path.substr(0, Fourth);
			Start = Fourth;
		}

		while(Start < Path.size())
		{
			while(Start < Path.size() && Path[Start] == L'\\')
				Start++;
			if(Start >= Path.size())
				break;
			size_t End = Path.find(L'\\', Start);
			if(End == std::wstring::npos)
				End = Path.size();
			const std::wstring Segment = Path.substr(Start, End - Start);
			Current = JoinPath(Current, Segment);
			if(!CreateDirectoryW(LongPath(Current).c_str(), nullptr))
			{
				const DWORD LastError = GetLastError();
				if(LastError != ERROR_ALREADY_EXISTS)
				{
					Error = "could not create directory '" + Utf8FromWide(Current) + "': " + FormatLastError(LastError);
					return false;
				}
			}
			Start = End;
		}
		return DirectoryExists(Path);
	}

	bool RemoveTree(const std::wstring &Path, std::string &Error)
	{
		if(!DirectoryExists(Path))
			return true;

		const std::wstring Pattern = JoinPath(Path, L"*");
		WIN32_FIND_DATAW Data;
		HANDLE hFind = FindFirstFileW(LongPath(Pattern).c_str(), &Data);
		if(hFind != INVALID_HANDLE_VALUE)
		{
			do
			{
				if(wcscmp(Data.cFileName, L".") == 0 || wcscmp(Data.cFileName, L"..") == 0)
					continue;
				const std::wstring Child = JoinPath(Path, Data.cFileName);
				if((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					if(!RemoveTree(Child, Error))
					{
						FindClose(hFind);
						return false;
					}
				}
				else
				{
					if(!DeleteFileW(LongPath(Child).c_str()))
					{
						const DWORD LastError = GetLastError();
						if(LastError != ERROR_FILE_NOT_FOUND)
						{
							Error = "could not delete file '" + Utf8FromWide(Child) + "': " + FormatLastError(LastError);
							FindClose(hFind);
							return false;
						}
					}
				}
			} while(FindNextFileW(hFind, &Data));
			FindClose(hFind);
		}
		if(!RemoveDirectoryW(LongPath(Path).c_str()))
		{
			const DWORD LastError = GetLastError();
			if(LastError != ERROR_FILE_NOT_FOUND && LastError != ERROR_PATH_NOT_FOUND)
			{
				Error = "could not remove directory '" + Utf8FromWide(Path) + "': " + FormatLastError(LastError);
				return false;
			}
		}
		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	// command line
	//////////////////////////////////////////////////////////////////////////

	struct SOptions
	{
		unsigned long m_Pid = 0;
		std::wstring m_ZipPath;
		std::string m_Sha256;
		std::wstring m_TargetDir;
		std::wstring m_RestartPath;
		std::wstring m_LogPath;
	};

	void PrintUsage()
	{
		fprintf(stderr,
			"Usage: TClientUpdater.exe --pid <pid> --zip <zip> --sha256 <hex> --target <dir> --restart <exe> --log <file>\n"
			"\n"
			"Exit codes:\n"
			"  0 success\n"
			"  1 usage error\n"
			"  2 invalid zip archive\n"
			"  3 sha256 mismatch\n"
			"  4 timeout waiting for the client process to exit\n"
			"  5 install failed, previous files restored\n"
			"  6 unsafe or unsupported zip entry\n"
			"  7 file system error\n"
			"  8 restart executable could not be started\n");
	}

	bool ParseCommandLine(int argc, wchar_t **argv, SOptions &Options, std::string &Error)
	{
		for(int i = 1; i < argc; i++)
		{
			const std::wstring Argument = argv[i];
			auto NextValue = [&](std::wstring &OutValue) -> bool {
				if(i + 1 >= argc)
				{
					Error = "missing value for argument '" + Utf8FromWide(Argument) + "'";
					return false;
				}
				OutValue = argv[++i];
				return true;
			};

			if(Argument == L"--pid")
			{
				std::wstring Value;
				if(!NextValue(Value))
					return false;
				wchar_t *pEnd = nullptr;
				const unsigned long Parsed = wcstoul(Value.c_str(), &pEnd, 10);
				if(pEnd == Value.c_str() || *pEnd != L'\0')
				{
					Error = "invalid value for --pid";
					return false;
				}
				Options.m_Pid = Parsed;
			}
			else if(Argument == L"--zip")
			{
				if(!NextValue(Options.m_ZipPath))
					return false;
			}
			else if(Argument == L"--sha256")
			{
				std::wstring Value;
				if(!NextValue(Value))
					return false;
				Options.m_Sha256 = Utf8FromWide(Value);
			}
			else if(Argument == L"--target")
			{
				if(!NextValue(Options.m_TargetDir))
					return false;
			}
			else if(Argument == L"--restart")
			{
				if(!NextValue(Options.m_RestartPath))
					return false;
			}
			else if(Argument == L"--log")
			{
				if(!NextValue(Options.m_LogPath))
					return false;
			}
			else if(Argument == L"--help" || Argument == L"-h" || Argument == L"/?")
			{
				Error = "help requested";
				return false;
			}
			else
			{
				Error = "unknown argument '" + Utf8FromWide(Argument) + "'";
				return false;
			}
		}

		if(Options.m_ZipPath.empty())
		{
			Error = "--zip is required";
			return false;
		}
		if(Options.m_Sha256.size() != 64)
		{
			Error = "--sha256 must be 64 hexadecimal characters";
			return false;
		}
		for(char Character : Options.m_Sha256)
		{
			const bool IsHex = (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f') || (Character >= 'A' && Character <= 'F');
			if(!IsHex)
			{
				Error = "--sha256 must be 64 hexadecimal characters";
				return false;
			}
		}
		if(Options.m_TargetDir.empty())
		{
			Error = "--target is required";
			return false;
		}
		if(Options.m_RestartPath.empty())
		{
			Error = "--restart is required";
			return false;
		}
		if(Options.m_LogPath.empty())
		{
			Error = "--log is required";
			return false;
		}
		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	// waiting for the client process
	//////////////////////////////////////////////////////////////////////////

	// Returns true when the process is gone, false on timeout. Errors while opening the process are
	// treated as "already gone" because the client may have exited before this helper started.
	bool WaitForProcessExit(unsigned long Pid, bool &OutTimedOut)
	{
		OutTimedOut = false;
		if(Pid == 0)
		{
			g_Logger.Info("no pid given, not waiting for the client to exit");
			return true;
		}

		const ULONGLONG StartTime = GetTickCount64();
		const DWORD TimeoutMs = GetWaitTimeout();
		for(;;)
		{
			HANDLE hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)Pid);
			if(hProcess != nullptr)
			{
				const ULONGLONG Elapsed = GetTickCount64() - StartTime;
				if(Elapsed >= TimeoutMs)
				{
					CloseHandle(hProcess);
					OutTimedOut = true;
					return false;
				}
				const DWORD Remaining = (DWORD)(TimeoutMs - Elapsed);
				const DWORD Result = WaitForSingleObject(hProcess, Remaining);
				CloseHandle(hProcess);
				if(Result == WAIT_OBJECT_0)
				{
					g_Logger.Info("process %lu exited after %llu ms", Pid, (unsigned long long)(GetTickCount64() - StartTime));
					return true;
				}
				if(Result == WAIT_TIMEOUT)
				{
					OutTimedOut = true;
					return false;
				}
				g_Logger.Warn("waiting for process %lu failed: %s", Pid, FormatLastError(GetLastError()).c_str());
				return true;
			}

			const DWORD LastError = GetLastError();
			if(LastError == ERROR_INVALID_PARAMETER)
			{
				g_Logger.Info("process %lu does not exist anymore", Pid);
				return true;
			}
			if(LastError != ERROR_ACCESS_DENIED)
			{
				g_Logger.Warn("could not open process %lu: %s, assuming it has exited", Pid, FormatLastError(LastError).c_str());
				return true;
			}

			// Access denied means the process exists but we may not synchronize with it. Poll instead.
			if(GetTickCount64() - StartTime >= TimeoutMs)
			{
				OutTimedOut = true;
				return false;
			}
			Sleep(250);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// extraction
	//////////////////////////////////////////////////////////////////////////

	bool WriteAll(HANDLE hFile, const void *pData, size_t Length, std::string &Error)
	{
		const unsigned char *pBytes = (const unsigned char *)pData;
		while(Length > 0)
		{
			const DWORD Chunk = (DWORD)std::min<size_t>(Length, 1u << 20);
			DWORD Written = 0;
			if(!WriteFile(hFile, pBytes, Chunk, &Written, nullptr) || Written != Chunk)
			{
				Error = "could not write file: " + FormatLastError(GetLastError());
				return false;
			}
			pBytes += Chunk;
			Length -= Chunk;
		}
		return true;
	}

	bool ExtractStored(const CZipArchive &Archive, const SZipEntry &Entry, uint64_t DataOffset, HANDLE hFile, std::string &Error)
	{
		uint64_t Remaining = Entry.m_CompressedSize;
		const unsigned char *pData = Archive.Data() + DataOffset;
		uLong Crc = crc32(0L, Z_NULL, 0);
		while(Remaining > 0)
		{
			const size_t Chunk = (size_t)std::min<uint64_t>(Remaining, 1u << 20);
			if(!WriteAll(hFile, pData, Chunk, Error))
				return false;
			Crc = crc32(Crc, pData, (uInt)Chunk);
			pData += Chunk;
			Remaining -= Chunk;
		}
		if(Crc != Entry.m_Crc32)
		{
			Error = "crc32 mismatch";
			return false;
		}
		return true;
	}

	bool ExtractDeflate(const CZipArchive &Archive, const SZipEntry &Entry, uint64_t DataOffset, HANDLE hFile, std::string &Error)
	{
		z_stream Stream;
		memset(&Stream, 0, sizeof(Stream));
		if(inflateInit2(&Stream, -MAX_WBITS) != Z_OK)
		{
			Error = "could not initialize the deflate decoder";
			return false;
		}

		const unsigned char *pInput = Archive.Data() + DataOffset;
		uint64_t Remaining = Entry.m_CompressedSize;
		std::vector<unsigned char> Buffer(256 * 1024);
		uint64_t TotalOutput = 0;
		uLong Crc = crc32(0L, Z_NULL, 0);
		bool Success = false;
		std::string Failure;

		for(;;)
		{
			if(Stream.avail_in == 0 && Remaining > 0)
			{
				const uInt Chunk = (uInt)std::min<uint64_t>(Remaining, 1u << 20);
				Stream.next_in = const_cast<Bytef *>(pInput);
				Stream.avail_in = Chunk;
				pInput += Chunk;
				Remaining -= Chunk;
			}

			Stream.next_out = Buffer.data();
			Stream.avail_out = (uInt)Buffer.size();
			const int Result = inflate(&Stream, Z_NO_FLUSH);
			const size_t Produced = Buffer.size() - Stream.avail_out;
			if(Produced > 0)
			{
				if(!WriteAll(hFile, Buffer.data(), Produced, Failure))
					break;
				Crc = crc32(Crc, Buffer.data(), (uInt)Produced);
				TotalOutput += Produced;
				if(TotalOutput > Entry.m_UncompressedSize)
				{
					Failure = "entry expands beyond its declared size";
					break;
				}
			}

			if(Result == Z_STREAM_END)
			{
				Success = true;
				break;
			}
			if(Result != Z_OK && Result != Z_BUF_ERROR)
			{
				char aBuffer[128];
				snprintf(aBuffer, sizeof(aBuffer), "deflate error %d", Result);
				Failure = aBuffer;
				break;
			}
			if(Result == Z_BUF_ERROR && Stream.avail_in == 0 && Remaining == 0 && Produced == 0)
			{
				Failure = "compressed stream is truncated";
				break;
			}
		}

		inflateEnd(&Stream);

		if(!Success)
		{
			Error = Failure.empty() ? "could not decompress entry" : Failure;
			return false;
		}
		if(TotalOutput != Entry.m_UncompressedSize)
		{
			Error = "decompressed size does not match the central directory";
			return false;
		}
		if(Crc != Entry.m_Crc32)
		{
			Error = "crc32 mismatch";
			return false;
		}
		return true;
	}

	bool ExtractEntry(const CZipArchive &Archive, const SZipEntry &Entry, bool IsDirectory, const std::wstring &DestinationPath, std::string &Error)
	{
		if(IsDirectory)
			return EnsureDirectory(DestinationPath, Error);

		std::string DirectoryError;
		const size_t Slash = DestinationPath.find_last_of(L'\\');
		if(Slash != std::wstring::npos)
		{
			if(!EnsureDirectory(DestinationPath.substr(0, Slash), DirectoryError))
			{
				Error = DirectoryError;
				return false;
			}
		}

		HANDLE hFile = CreateFileW(LongPath(DestinationPath).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if(hFile == INVALID_HANDLE_VALUE)
		{
			Error = "could not create '" + Utf8FromWide(DestinationPath) + "': " + FormatLastError(GetLastError());
			return false;
		}

		uint64_t DataOffset = 0;
		bool Ok = Archive.GetEntryDataOffset(Entry, DataOffset, Error);
		if(Ok)
		{
			if(Entry.m_Method == 0)
				Ok = ExtractStored(Archive, Entry, DataOffset, hFile, Error);
			else
				Ok = ExtractDeflate(Archive, Entry, DataOffset, hFile, Error);
		}

		if(!CloseHandle(hFile) && Ok)
		{
			Error = "could not flush '" + Utf8FromWide(DestinationPath) + "': " + FormatLastError(GetLastError());
			Ok = false;
		}
		if(!Ok)
			DeleteFileW(LongPath(DestinationPath).c_str());
		return Ok;
	}

	//////////////////////////////////////////////////////////////////////////
	// installation
	//////////////////////////////////////////////////////////////////////////

	std::wstring TimestampSuffix()
	{
		SYSTEMTIME Time;
		GetLocalTime(&Time);
		wchar_t aBuffer[64];
		swprintf(aBuffer, sizeof(aBuffer) / sizeof(aBuffer[0]), L"%04u%02u%02u_%02u%02u%02u",
			(unsigned)Time.wYear, (unsigned)Time.wMonth, (unsigned)Time.wDay,
			(unsigned)Time.wHour, (unsigned)Time.wMinute, (unsigned)Time.wSecond);
		return aBuffer;
	}

	struct SPlannedEntry
	{
		const SZipEntry *m_pEntry;
		std::wstring m_RelativePath;
		bool m_IsDirectory;
	};

	struct SInstalledFile
	{
		std::wstring m_TargetPath;
		std::wstring m_BackupPath;
		bool m_HadBackup = false;
	};

	bool StartRestartProcess(const std::wstring &Executable, const std::wstring &WorkingDirectory, std::string &Error)
	{
		std::wstring CommandLine = L"\"" + Executable + L"\"";
		std::vector<wchar_t> CommandLineBuffer(CommandLine.begin(), CommandLine.end());
		CommandLineBuffer.push_back(L'\0');

		STARTUPINFOW StartupInfo;
		memset(&StartupInfo, 0, sizeof(StartupInfo));
		StartupInfo.cb = sizeof(StartupInfo);
		PROCESS_INFORMATION ProcessInfo;
		memset(&ProcessInfo, 0, sizeof(ProcessInfo));

		if(!CreateProcessW(Executable.c_str(), CommandLineBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr, WorkingDirectory.c_str(), &StartupInfo, &ProcessInfo))
		{
			Error = "could not start '" + Utf8FromWide(Executable) + "': " + FormatLastError(GetLastError());
			return false;
		}
		g_Logger.Info("restarted '%s' with pid %lu", Utf8FromWide(Executable).c_str(), (unsigned long)ProcessInfo.dwProcessId);
		CloseHandle(ProcessInfo.hThread);
		CloseHandle(ProcessInfo.hProcess);
		return true;
	}

	// Removes the read-only attribute so that MoveFileEx can replace the file.
	void ClearReadOnly(const std::wstring &Path)
	{
		const DWORD Attributes = GetFileAttributesW(LongPath(Path).c_str());
		if(Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_READONLY) == 0)
			return;
		SetFileAttributesW(LongPath(Path).c_str(), Attributes & ~FILE_ATTRIBUTE_READONLY);
	}

	// Absolute path of this executable. The update archive may contain a new version of the
	// helper itself, and a running executable cannot be replaced in place: it has to be renamed
	// first, which Windows allows.
	std::wstring GetOwnExecutablePath()
	{
		std::vector<wchar_t> Buffer(MAX_PATH);
		for(;;)
		{
			const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), (DWORD)Buffer.size());
			if(Length == 0)
				return std::wstring();
			if(Length < Buffer.size())
				return std::wstring(Buffer.data(), Length);
			Buffer.resize(Buffer.size() * 2);
		}
	}

	bool SamePath(const std::wstring &Left, const std::wstring &Right)
	{
		return ToLowerWide(Left) == ToLowerWide(Right);
	}

	// The previous helper executable is renamed out of the way while the update runs, so it can
	// only be deleted once that process is gone, i.e. on the next start.
	void RemoveStaleSelfBackups(const std::wstring &Directory)
	{
		const std::wstring Pattern = JoinPath(Directory, L"TClientUpdater.exe.old_*");
		WIN32_FIND_DATAW Data;
		HANDLE hFind = FindFirstFileW(LongPath(Pattern).c_str(), &Data);
		if(hFind == INVALID_HANDLE_VALUE)
			return;
		do
		{
			const std::wstring Path = JoinPath(Directory, Data.cFileName);
			if(DeleteFileW(LongPath(Path).c_str()))
				g_Logger.Info("removed stale helper backup '%s'", Utf8FromWide(Data.cFileName).c_str());
		} while(FindNextFileW(hFind, &Data));
		FindClose(hFind);
	}

	int RunUpdate(const SOptions &Options)
	{
		// 1. Validate the input paths.
		const std::wstring ZipPath = GetFullPath(Options.m_ZipPath);
		const std::wstring TargetDir = GetFullPath(Options.m_TargetDir);
		const std::wstring RestartPath = GetFullPath(Options.m_RestartPath);

		g_Logger.Info("zip: %s", Utf8FromWide(ZipPath).c_str());
		g_Logger.Info("target: %s", Utf8FromWide(TargetDir).c_str());
		g_Logger.Info("restart: %s", Utf8FromWide(RestartPath).c_str());
		g_Logger.Info("pid: %lu", Options.m_Pid);

		if(!DirectoryExists(TargetDir))
		{
			g_Logger.Error("target directory does not exist: %s", Utf8FromWide(TargetDir).c_str());
			return EXIT_USAGE;
		}
		if(!FileExists(ZipPath))
		{
			g_Logger.Error("zip file does not exist: %s", Utf8FromWide(ZipPath).c_str());
			return EXIT_USAGE;
		}
		if(!FileExists(RestartPath))
			g_Logger.Warn("restart executable does not exist yet: %s", Utf8FromWide(RestartPath).c_str());

		// 2. Verify the sha256 of the downloaded archive before doing anything else.
		std::string ActualSha256;
		std::string HashError;
		if(!Sha256File(ZipPath, ActualSha256, HashError))
		{
			g_Logger.Error("hashing the zip failed: %s", HashError.c_str());
			return EXIT_FILE_SYSTEM;
		}
		std::string ExpectedSha256 = Options.m_Sha256;
		std::transform(ExpectedSha256.begin(), ExpectedSha256.end(), ExpectedSha256.begin(), [](unsigned char Character) { return (char)tolower(Character); });
		g_Logger.Info("sha256: %s (expected %s)", ActualSha256.c_str(), ExpectedSha256.c_str());
		if(ActualSha256 != ExpectedSha256)
		{
			g_Logger.Error("sha256 mismatch, refusing to install");
			return EXIT_SHA_MISMATCH;
		}

		// 3. Wait for the running client to exit.
		bool TimedOut = false;
		if(!WaitForProcessExit(Options.m_Pid, TimedOut))
		{
			g_Logger.Error("timed out after %lu ms waiting for process %lu to exit", (unsigned long)GetWaitTimeout(), Options.m_Pid);
			return EXIT_PID_TIMEOUT;
		}

		// 4. Read the central directory and validate every entry before touching the target.
		CZipArchive Archive;
		std::string ZipError;
		if(!Archive.Open(ZipPath, ZipError))
		{
			g_Logger.Error("invalid zip archive: %s", ZipError.c_str());
			return EXIT_ZIP_INVALID;
		}
		g_Logger.Info("zip contains %llu entries", (unsigned long long)Archive.Entries().size());

		std::vector<SPlannedEntry> Planned;
		std::set<std::wstring> SeenPaths;
		uint64_t PlannedBytes = 0;
		unsigned Skipped = 0;

		for(const SZipEntry &Entry : Archive.Entries())
		{
			bool IsDirectory = false;
			std::wstring RelativePath;
			std::string NameError;
			const EEntryCheck Check = CheckEntryName(Entry.m_LogName, IsDirectory, RelativePath, NameError);
			if(Check == EEntryCheck::REJECT)
			{
				g_Logger.Error("refusing to install entry '%s': %s", Utf8FromWide(Entry.m_LogName).c_str(), NameError.c_str());
				return EXIT_UNSAFE_ENTRY;
			}
			if(Check == EEntryCheck::SKIP_PROTECTED)
			{
				g_Logger.Warn("skipping protected user path '%s'", Utf8FromWide(Entry.m_LogName).c_str());
				Skipped++;
				continue;
			}
			if(!SeenPaths.insert(ToLowerWide(RelativePath)).second)
			{
				g_Logger.Error("refusing to install entry '%s': the path appears more than once in the archive", Utf8FromWide(Entry.m_LogName).c_str());
				return EXIT_UNSAFE_ENTRY;
			}
			Planned.push_back({&Entry, RelativePath, IsDirectory});
			if(!IsDirectory)
				PlannedBytes += Entry.m_UncompressedSize;
		}

		if(Planned.empty())
		{
			g_Logger.Error("zip contains no files that could be installed");
			return EXIT_ZIP_INVALID;
		}
		g_Logger.Info("planned: %llu entries, %llu skipped, %llu bytes", (unsigned long long)Planned.size(), (unsigned long long)Skipped, (unsigned long long)PlannedBytes);

		// 5. Extract everything into the staging directory first.
		const std::wstring StagingDir = JoinPath(TargetDir, L"update_staging_" + std::to_wstring(Options.m_Pid));
		std::string FileError;
		if(!RemoveTree(StagingDir, FileError))
		{
			g_Logger.Error("could not clean the staging directory: %s", FileError.c_str());
			return EXIT_FILE_SYSTEM;
		}
		if(!EnsureDirectory(StagingDir, FileError))
		{
			g_Logger.Error("could not create the staging directory: %s", FileError.c_str());
			return EXIT_FILE_SYSTEM;
		}
		g_Logger.Info("staging directory: %s", Utf8FromWide(StagingDir).c_str());

		for(const SPlannedEntry &Entry : Planned)
		{
			const std::wstring Destination = JoinPath(StagingDir, Entry.m_RelativePath);
			if(!ExtractEntry(Archive, *Entry.m_pEntry, Entry.m_IsDirectory, Destination, FileError))
			{
				g_Logger.Error("extracting '%s' failed: %s", Utf8FromWide(Entry.m_pEntry->m_LogName).c_str(), FileError.c_str());
				RemoveTree(StagingDir, FileError);
				return EXIT_UNSAFE_ENTRY;
			}
		}
		g_Logger.Info("extraction finished");

		// 6. Back up the files that are about to be replaced. User data never gets here because
		// such entries were skipped above.
		const std::wstring BackupDir = JoinPath(TargetDir, L"update_backup_" + TimestampSuffix());
		std::vector<SInstalledFile> Installed;
		std::vector<SPlannedEntry> FileEntries;
		bool BackupDirectoryCreated = false;

		for(const SPlannedEntry &Entry : Planned)
		{
			if(Entry.m_IsDirectory)
				continue;
			FileEntries.push_back(Entry);

			SInstalledFile Record;
			Record.m_TargetPath = JoinPath(TargetDir, Entry.m_RelativePath);
			if(FileExists(Record.m_TargetPath))
			{
				if(!BackupDirectoryCreated)
				{
					if(!EnsureDirectory(BackupDir, FileError))
					{
						g_Logger.Error("could not create the backup directory: %s", FileError.c_str());
						RemoveTree(StagingDir, FileError);
						return EXIT_FILE_SYSTEM;
					}
					BackupDirectoryCreated = true;
					g_Logger.Info("backup directory: %s", Utf8FromWide(BackupDir).c_str());
				}
				// Mirror the relative path so that files with the same name in different
				// directories do not collide.
				Record.m_BackupPath = JoinPath(BackupDir, Entry.m_RelativePath);
				const size_t Slash = Record.m_BackupPath.find_last_of(L'\\');
				if(Slash != std::wstring::npos)
				{
					if(!EnsureDirectory(Record.m_BackupPath.substr(0, Slash), FileError))
					{
						g_Logger.Error("could not create the backup directory: %s", FileError.c_str());
						RemoveTree(StagingDir, FileError);
						return EXIT_FILE_SYSTEM;
					}
				}
				if(!CopyFileW(LongPath(Record.m_TargetPath).c_str(), LongPath(Record.m_BackupPath).c_str(), FALSE))
				{
					g_Logger.Error("could not back up '%s': %s", Utf8FromWide(Record.m_TargetPath).c_str(), FormatLastError(GetLastError()).c_str());
					RemoveTree(StagingDir, FileError);
					return EXIT_FILE_SYSTEM;
				}
				Record.m_HadBackup = true;
			}
			Installed.push_back(Record);
		}

		// 7. If the archive contains a new version of this helper, move the running executable
		// out of the way first: Windows refuses to replace a mapped image in place.
		const std::wstring OwnExecutable = GetOwnExecutablePath();
		for(const SPlannedEntry &Entry : FileEntries)
		{
			const std::wstring Destination = JoinPath(TargetDir, Entry.m_RelativePath);
			if(OwnExecutable.empty() || !SamePath(Destination, OwnExecutable))
				continue;
			const std::wstring Renamed = OwnExecutable + L".old_" + TimestampSuffix();
			if(!MoveFileExW(LongPath(OwnExecutable).c_str(), LongPath(Renamed).c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				g_Logger.Error("could not move the running helper out of the way: %s", FormatLastError(GetLastError()).c_str());
				RemoveTree(StagingDir, FileError);
				return EXIT_FILE_SYSTEM;
			}
			g_Logger.Info("moved the running helper to '%s'", Utf8FromWide(Renamed).c_str());
		}

		// 8. Move the staged files into place.
		for(size_t Index = 0; Index < FileEntries.size(); Index++)
		{
			const std::wstring Source = JoinPath(StagingDir, FileEntries[Index].m_RelativePath);
			const std::wstring Destination = Installed[Index].m_TargetPath;

			std::string DirectoryError;
			const size_t Slash = Destination.find_last_of(L'\\');
			if(Slash != std::wstring::npos && !EnsureDirectory(Destination.substr(0, Slash), DirectoryError))
			{
				g_Logger.Error("could not create the target directory: %s", DirectoryError.c_str());
				// Fall through to the rollback below.
			}
			else
			{
				ClearReadOnly(Destination);
				if(MoveFileExW(LongPath(Source).c_str(), LongPath(Destination).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
					continue;
				g_Logger.Error("could not replace '%s': %s", Utf8FromWide(Destination).c_str(), FormatLastError(GetLastError()).c_str());
			}

			// Roll back everything that was already replaced.
			g_Logger.Warn("rolling back %llu already installed files", (unsigned long long)Index);
			for(size_t Rollback = Index; Rollback > 0; Rollback--)
			{
				const SInstalledFile &Record = Installed[Rollback - 1];
				ClearReadOnly(Record.m_TargetPath);
				if(Record.m_HadBackup)
				{
					if(MoveFileExW(LongPath(Record.m_BackupPath).c_str(), LongPath(Record.m_TargetPath).c_str(), MOVEFILE_REPLACE_EXISTING))
						g_Logger.Info("restored '%s'", Utf8FromWide(Record.m_TargetPath).c_str());
					else
						g_Logger.Error("could not restore '%s': %s", Utf8FromWide(Record.m_TargetPath).c_str(), FormatLastError(GetLastError()).c_str());
				}
				else
				{
					if(DeleteFileW(LongPath(Record.m_TargetPath).c_str()))
						g_Logger.Info("removed newly created '%s'", Utf8FromWide(Record.m_TargetPath).c_str());
					else
						g_Logger.Error("could not remove '%s': %s", Utf8FromWide(Record.m_TargetPath).c_str(), FormatLastError(GetLastError()).c_str());
				}
			}
			RemoveTree(StagingDir, FileError);
			g_Logger.Error("update failed, previous state restored");
			return EXIT_INSTALL_FAILED;
		}
		g_Logger.Info("installed %llu files", (unsigned long long)FileEntries.size());

		// 9. Remove the staging directory and restart the client.
		if(!RemoveTree(StagingDir, FileError))
			g_Logger.Warn("could not remove the staging directory: %s", FileError.c_str());
		RemoveStaleSelfBackups(TargetDir);

		if(!FileExists(RestartPath))
		{
			g_Logger.Error("restart executable is missing after the update: %s", Utf8FromWide(RestartPath).c_str());
			return EXIT_RESTART_FAILED;
		}
		std::string StartError;
		if(!StartRestartProcess(RestartPath, TargetDir, StartError))
		{
			g_Logger.Error("%s", StartError.c_str());
			return EXIT_RESTART_FAILED;
		}

		g_Logger.Info("update finished successfully");
		return EXIT_OK;
	}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	SOptions Options;
	std::string ParseError;
	if(!ParseCommandLine(argc, argv, Options, ParseError))
	{
		if(ParseError != "help requested")
			fprintf(stderr, "error: %s\n", ParseError.c_str());
		PrintUsage();
		return EXIT_USAGE;
	}

	const std::wstring LogPath = GetFullPath(Options.m_LogPath);
	if(!g_Logger.Open(LogPath))
		fprintf(stderr, "warning: could not open log file '%s': %s\n", Utf8FromWide(LogPath).c_str(), FormatLastError(GetLastError()).c_str());

	g_Logger.Info("TClientUpdater starting (argv[0]=%s)", argc > 0 ? Utf8FromWide(argv[0]).c_str() : "");

	const int Result = RunUpdate(Options);

	g_Logger.Info("TClientUpdater exiting with code %d", Result);
	g_Logger.Close();
	return Result;
}
