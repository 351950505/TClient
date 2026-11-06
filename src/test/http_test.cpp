#include "test.h"

#include <base/fs.h>
#include <base/hash.h>
#include <base/str.h>

#include <engine/http.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

class CHttpFileRequest : public IHttpRequest
{
public:
	explicit CHttpFileRequest(IStorage *pStorage) :
		IHttpRequest("https://example.invalid/update.zip")
	{
		WriteToFile(pStorage, "update.zip", IStorage::TYPE_SAVE);
		SkipByFileTime(false);
		LogProgress(HTTPLOG::NONE);
		ExpectSha256(sha256("expected", 8));
		AllowResume(true);
	}

	void Header(const char *) override {}
	bool Begin() { return BeforeInit(); }
	size_t Feed(const char *pData) { return OnData(pData, str_length(pData)); }
	void Complete(EHttpState State, int StatusCode = 200)
	{
		m_StatusCode = StatusCode;
		OnCompletionInternal(State);
	}
	int64_t ResumeOffset() const { return m_ResumeFrom; }
	bool HasPartialFile() const { return fs_is_file(m_aDestAbsoluteTmp); }
	bool HasDestinationFile() const { return fs_is_file(m_aDestAbsolute); }
};

class CHttpResume : public ::testing::Test
{
protected:
	CTestInfo m_Info;
	std::unique_ptr<IStorage> m_pStorage;

	void SetUp() override
	{
		m_pStorage = m_Info.CreateTestStorage();
		ASSERT_NE(m_pStorage, nullptr);
		m_Info.m_DeleteTestStorageFilesOnSuccess = true;
	}
};

TEST_F(CHttpResume, HashMismatchDiscardsPartialFile)
{
	{
		CHttpFileRequest Request(m_pStorage.get());
		ASSERT_TRUE(Request.Begin());
		EXPECT_EQ(Request.Feed("poisoned"), 8);
		Request.Complete(EHttpState::DONE);
		EXPECT_EQ(Request.State(), EHttpState::ERROR);
		EXPECT_FALSE(Request.HasPartialFile());
		EXPECT_FALSE(Request.HasDestinationFile());
	}
	CHttpFileRequest Retry(m_pStorage.get());
	ASSERT_TRUE(Retry.Begin());
	EXPECT_EQ(Retry.ResumeOffset(), 0);
	EXPECT_EQ(Retry.Feed("expected"), 8);
	Retry.Complete(EHttpState::DONE);
	EXPECT_EQ(Retry.State(), EHttpState::DONE);
	EXPECT_TRUE(Retry.HasDestinationFile());
}

TEST_F(CHttpResume, InterruptedTransferResumesWithCompleteHash)
{
	{
		CHttpFileRequest Request(m_pStorage.get());
		ASSERT_TRUE(Request.Begin());
		EXPECT_EQ(Request.Feed("expe"), 4);
		Request.Complete(EHttpState::ERROR);
		EXPECT_TRUE(Request.HasPartialFile());
		EXPECT_FALSE(Request.HasDestinationFile());
	}
	CHttpFileRequest Retry(m_pStorage.get());
	ASSERT_TRUE(Retry.Begin());
	EXPECT_EQ(Retry.ResumeOffset(), 4);
	EXPECT_EQ(Retry.Feed("cted"), 4);
	Retry.Complete(EHttpState::DONE);
	EXPECT_EQ(Retry.State(), EHttpState::DONE);
	EXPECT_TRUE(Retry.HasDestinationFile());
	EXPECT_FALSE(Retry.HasPartialFile());
}

TEST_F(CHttpResume, RejectedRangeDiscardsPartialFile)
{
	{
		CHttpFileRequest Request(m_pStorage.get());
		ASSERT_TRUE(Request.Begin());
		EXPECT_EQ(Request.Feed("expe"), 4);
		Request.Complete(EHttpState::ERROR);
	}
	{
		CHttpFileRequest Retry(m_pStorage.get());
		ASSERT_TRUE(Retry.Begin());
		EXPECT_EQ(Retry.ResumeOffset(), 4);
		Retry.Complete(EHttpState::ERROR, 416);
		EXPECT_FALSE(Retry.HasPartialFile());
	}
	CHttpFileRequest FullRetry(m_pStorage.get());
	ASSERT_TRUE(FullRetry.Begin());
	EXPECT_EQ(FullRetry.ResumeOffset(), 0);
	EXPECT_EQ(FullRetry.Feed("expected"), 8);
	FullRetry.Complete(EHttpState::DONE);
	EXPECT_EQ(FullRetry.State(), EHttpState::DONE);
}

TEST_F(CHttpResume, AbortedTransferKeepsPartialFile)
{
	CHttpFileRequest Request(m_pStorage.get());
	ASSERT_TRUE(Request.Begin());
	EXPECT_EQ(Request.Feed("expe"), 4);
	Request.Complete(EHttpState::ABORTED);
	EXPECT_EQ(Request.State(), EHttpState::ABORTED);
	EXPECT_TRUE(Request.HasPartialFile());
	EXPECT_FALSE(Request.HasDestinationFile());
}

TEST_F(CHttpResume, NonResumableTransferDiscardsPartialFile)
{
	CHttpFileRequest Request(m_pStorage.get());
	Request.AllowResume(false);
	ASSERT_TRUE(Request.Begin());
	EXPECT_EQ(Request.Feed("expe"), 4);
	Request.Complete(EHttpState::ERROR);
	EXPECT_FALSE(Request.HasPartialFile());
	EXPECT_FALSE(Request.HasDestinationFile());
}
