#include "test.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/str.h>

#include <engine/client/auto_session_log.h>

#include <gtest/gtest.h>

#include <array>

TEST(AutoSessionLog, RecognizesLegacyAndPidFilenames)
{
	EXPECT_TRUE(TClientAutoSessionLog::IsFilename("ddnet-log-2026-11-15_23-59-59.log"));
	EXPECT_TRUE(TClientAutoSessionLog::IsFilename("ddnet-log-2026-11-15_23-59-59-1.log"));
	EXPECT_TRUE(TClientAutoSessionLog::IsFilename("ddnet-log-2026-11-15_23-59-59-429496.log"));
}

TEST(AutoSessionLog, RejectsInvalidFilenames)
{
	static constexpr std::array<const char *, 12> INVALID_FILENAMES = {
		"ddnet-log-2026-11-15_23-59-59-.log",
		"ddnet-log-2026-11-15_23-59-59--42.log",
		"ddnet-log-2026-11-15_23-59-59-+42.log",
		"ddnet-log-2026-11-15_23-59-59-42-extra.log",
		"ddnet-log-2026-11-15_23-59-59-42.log.bak",
		"ddnet-log-2026-11-15_23-59-59-abc.log",
		"ddnet-log-2026-11-15_23-59-59.log.bak",
		"ddnet-log-2026-11-15_23-59-59",
		"ddnet-log-2026-11-15-23-59-59.log",
		"ddnet-log-2026-11-15_23-59.log",
		"ddnet-log-2026-11-15_23-59-5x.log",
		"other-log-2026-11-15_23-59-59.log",
	};
	for(const char *pFilename : INVALID_FILENAMES)
		EXPECT_FALSE(TClientAutoSessionLog::IsFilename(pFilename)) << pFilename;
}

TEST(AutoSessionLog, FormatsUniquePidFilenames)
{
	char aFirst[64];
	char aSecond[64];
	TClientAutoSessionLog::FormatFilename(aFirst, sizeof(aFirst), "2026-11-15_23-59-59", 1234);
	TClientAutoSessionLog::FormatFilename(aSecond, sizeof(aSecond), "2026-11-15_23-59-59", 5678);

	EXPECT_STREQ(aFirst, "ddnet-log-2026-11-15_23-59-59-1234.log");
	EXPECT_STREQ(aSecond, "ddnet-log-2026-11-15_23-59-59-5678.log");
	EXPECT_STRNE(aFirst, aSecond);
	EXPECT_TRUE(TClientAutoSessionLog::IsFilename(aFirst));
	EXPECT_TRUE(TClientAutoSessionLog::IsFilename(aSecond));
}

TEST(AutoSessionLog, StartupFailureCreatesMissingDirectory)
{
	CTestInfo Info;
	Info.m_DeleteTestStorageFilesOnSuccess = true;
	char aDirectory[IO_MAX_PATH_LENGTH];
	str_format(aDirectory, sizeof(aDirectory), "%s/install/data/log", Info.m_aFilename);
	const char *pFilename = "ddnet-log-2026-11-16_19-00-00-123.log";
	IOHANDLE File = TClientAutoSessionLog::OpenFileInDirectory(aDirectory, pFilename);
	ASSERT_TRUE(File);
	EXPECT_FALSE(io_close(File));
	EXPECT_TRUE(fs_is_dir(aDirectory));
	EXPECT_FALSE(TClientAutoSessionLog::OpenFileInDirectory(aDirectory, "../../settings.cfg"));
}

TEST(AutoSessionLog, OpensInstallPathFirst)
{
	CTestInfo Info;
	Info.m_DeleteTestStorageFilesOnSuccess = true;
	ASSERT_FALSE(fs_makedir(Info.m_aFilename));

	char aInstallPath[IO_MAX_PATH_LENGTH];
	char aStoragePath[IO_MAX_PATH_LENGTH];
	str_format(aInstallPath, sizeof(aInstallPath), "%s/install.log", Info.m_aFilename);
	str_format(aStoragePath, sizeof(aStoragePath), "%s/storage.log", Info.m_aFilename);

	const TClientAutoSessionLog::SOpenResult Result = TClientAutoSessionLog::OpenFile(aInstallPath, aStoragePath, IOFLAG_WRITE);
	ASSERT_TRUE(Result.m_File);
	EXPECT_EQ(Result.m_Location, TClientAutoSessionLog::EOpenLocation::INSTALL);
	EXPECT_FALSE(io_close(Result.m_File));
	EXPECT_TRUE(fs_is_file(aInstallPath));
	EXPECT_FALSE(fs_is_file(aStoragePath));
}

TEST(AutoSessionLog, FallsBackAfterActualInstallOpenFailure)
{
	CTestInfo Info;
	Info.m_DeleteTestStorageFilesOnSuccess = true;
	ASSERT_FALSE(fs_makedir(Info.m_aFilename));

	char aInstallPath[IO_MAX_PATH_LENGTH];
	char aStoragePath[IO_MAX_PATH_LENGTH];
	str_format(aInstallPath, sizeof(aInstallPath), "%s/install.log", Info.m_aFilename);
	str_format(aStoragePath, sizeof(aStoragePath), "%s/storage.log", Info.m_aFilename);
	ASSERT_FALSE(fs_makedir(aInstallPath)); // A directory cannot be opened as a writable file.

	int BeforeStorageOpenCalls = 0;
	const TClientAutoSessionLog::SOpenResult Result = TClientAutoSessionLog::OpenFile(
		aInstallPath,
		aStoragePath,
		IOFLAG_WRITE,
		[&BeforeStorageOpenCalls] { ++BeforeStorageOpenCalls; });
	ASSERT_TRUE(Result.m_File);
	EXPECT_EQ(Result.m_Location, TClientAutoSessionLog::EOpenLocation::STORAGE);
	EXPECT_EQ(BeforeStorageOpenCalls, 1);
	EXPECT_FALSE(io_close(Result.m_File));
	EXPECT_TRUE(fs_is_dir(aInstallPath));
	EXPECT_TRUE(fs_is_file(aStoragePath));
}
