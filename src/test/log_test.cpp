#include "test.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/logger.h>
#include <base/str.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

TEST(FutureLogger, FlushesPendingAndForwardsNewMessages)
{
	CFutureLogger Future;
	CLogMessage Message = {};
	Message.m_Level = LEVEL_INFO;
	str_copy(Message.m_aLine, "pending");
	Future.Log(&Message);

	auto pLogger = std::make_shared<CMemoryLogger>();
	Future.Set(pLogger);
	str_copy(Message.m_aLine, "forwarded");
	Future.Log(&Message);
	EXPECT_EQ(pLogger->ConcatenatedLines(), "pending\nforwarded");
}

namespace
{
	class CCountingLogger : public ILogger
	{
	public:
		static constexpr int NUM_MESSAGES = 1024;
		std::array<std::atomic<int>, NUM_MESSAGES> m_aCounts{};
		std::atomic<int> m_Invalid{0};
		std::atomic<bool> m_Finished{false};

		void Log(const CLogMessage *pMessage) override
		{
			const int Index = pMessage->m_LineMessageOffset;
			if(Index >= 0 && Index < NUM_MESSAGES)
				m_aCounts[Index].fetch_add(1, std::memory_order_relaxed);
			else
				m_Invalid.fetch_add(1, std::memory_order_relaxed);
		}

		void GlobalFinish() override
		{
			m_Finished.store(true, std::memory_order_relaxed);
		}
	};

	std::atomic<int> gs_TranslationCalls{0};

	bool TranslateLogLineForTest(char *pBuf, unsigned BufSize, const char *, const char *)
	{
		gs_TranslationCalls.fetch_add(1, std::memory_order_relaxed);
		str_copy(pBuf, "translated", static_cast<int>(BufSize));
		return true;
	}

	class CLineTranslatorScope
	{
	public:
		CLineTranslatorScope()
		{
			log_set_line_translator(TranslateLogLineForTest);
		}
		~CLineTranslatorScope()
		{
			log_set_line_translator(nullptr);
		}
		CLineTranslatorScope(const CLineTranslatorScope &) = delete;
	};
}

TEST(FutureLogger, StartupFailureBindingWritesPendingDiagnostics)
{
	CTestInfo Info;
	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), "-startup-failure.log");
	CFutureLogger Future;
	CLogMessage Message = {};
	Message.m_Level = LEVEL_ERROR;
	str_copy(Message.m_aLine, "startup failed before configuration loaded");
	Message.m_LineLength = str_length(Message.m_aLine);
	Future.Log(&Message);
	IOHANDLE File = io_open(aFilename, IOFLAG_WRITE);
	ASSERT_TRUE(File);
	Future.Set(log_logger_file_limited(File, 1024));
	Future.GlobalFinish();
	Future.GlobalFinish(); // Repeated finish and later destruction must not wait twice.
	str_copy(Message.m_aLine, "late output must be ignored");
	Message.m_LineLength = str_length(Message.m_aLine);
	Future.Log(&Message);
	IOHANDLE Output = io_open(aFilename, IOFLAG_READ);
	ASSERT_TRUE(Output);
	char *pText = io_read_all_str(Output);
	io_close(Output);
	ASSERT_TRUE(pText);
	EXPECT_NE(str_find(pText, "startup failed before configuration loaded"), nullptr);
	EXPECT_EQ(str_find(pText, "late output must be ignored"), nullptr);
	free(pText);
	EXPECT_FALSE(fs_remove(aFilename));
}

TEST(FutureLogger, ConcurrentPublicationDeliversEachMessageOnce)
{
	CFutureLogger Future;
	auto pLogger = std::make_shared<CCountingLogger>();
	std::atomic<int> Ready{0};
	std::atomic<bool> Start{false};
	std::vector<std::thread> vWorkers;
	for(int Worker = 0; Worker < 4; ++Worker)
	{
		vWorkers.emplace_back([&, Worker] {
			CLogMessage Message = {};
			Message.m_Level = LEVEL_INFO;
			Message.m_LineMessageOffset = Worker * 256;
			Future.Log(&Message);
			Ready.fetch_add(1, std::memory_order_release);
			while(!Start.load(std::memory_order_acquire))
				std::this_thread::yield();
			for(int Index = 1; Index < 256; ++Index)
			{
				Message.m_LineMessageOffset = Worker * 256 + Index;
				Future.Log(&Message);
			}
		});
	}
	while(Ready.load(std::memory_order_acquire) != 4)
		std::this_thread::yield();
	Start.store(true, std::memory_order_release);
	Future.Set(pLogger);
	for(auto &Worker : vWorkers)
		Worker.join();
	for(const auto &Count : pLogger->m_aCounts)
		EXPECT_EQ(Count.load(std::memory_order_relaxed), 1);
	EXPECT_EQ(pLogger->m_Invalid.load(), 0);
	Future.GlobalFinish();
	EXPECT_TRUE(pLogger->m_Finished.load());
}

TEST(LoggerAsync, LimitedFileCountsActualNewlines)
{
	CTestInfo Info;
	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), "-newline-budget.log");
	IOHANDLE File = io_open(aFilename, IOFLAG_WRITE);
	ASSERT_TRUE(File);
	static constexpr uint64_t LIMIT = 64 * 1024;
	CLogMessage Message = {};
	Message.m_Level = LEVEL_INFO;
	str_copy(Message.m_aLine, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv");
	Message.m_LineLength = str_length(Message.m_aLine);
	{
		auto pLogger = log_logger_file_limited(File, LIMIT);
		for(int i = 0; i < 1000; ++i)
			pLogger->Log(&Message);
	}
	IOHANDLE Output = io_open(aFilename, IOFLAG_READ);
	ASSERT_TRUE(Output);
	EXPECT_LE(io_length(Output), LIMIT);
	io_close(Output);
	EXPECT_FALSE(fs_remove(aFilename));
}

TEST(LoggerAsync, TinyLimitDoesNotOverflowWithNotice)
{
	CTestInfo Info;
	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), "-tiny-budget.log");
	IOHANDLE File = io_open(aFilename, IOFLAG_WRITE);
	ASSERT_TRUE(File);
	CLogMessage Message = {};
	Message.m_Level = LEVEL_INFO;
	str_copy(Message.m_aLine, "record");
	Message.m_LineLength = str_length(Message.m_aLine);
	{
		auto pLogger = log_logger_file_limited(File, 1);
		pLogger->Log(&Message);
	}
	IOHANDLE Output = io_open(aFilename, IOFLAG_READ);
	ASSERT_TRUE(Output);
	EXPECT_LE(io_length(Output), 1);
	io_close(Output);
	EXPECT_FALSE(fs_remove(aFilename));
}

TEST(LoggerAsync, AutoSessionTranslationDoesNotDependOnCleanupPath)
{
	CTestInfo Info;
	char aAutoFilename[IO_MAX_PATH_LENGTH];
	char aLimitedFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aAutoFilename, sizeof(aAutoFilename), "-auto-session.log");
	Info.Filename(aLimitedFilename, sizeof(aLimitedFilename), "-limited.log");

	gs_TranslationCalls.store(0, std::memory_order_relaxed);
	const CLineTranslatorScope TranslatorScope;

	CLogMessage Message = {};
	Message.m_Level = LEVEL_INFO;
	str_copy(Message.m_aSystem, "test");
	str_copy(Message.m_aLine, "prefix: original");
	Message.m_LineMessageOffset = str_length("prefix: ");
	Message.m_LineLength = str_length(Message.m_aLine);

	IOHANDLE AutoFile = io_open(aAutoFilename, IOFLAG_WRITE);
	ASSERT_TRUE(AutoFile);
	{
		auto pLogger = log_logger_file_limited(AutoFile, 0, "");
		pLogger->Log(&Message);
	}
	EXPECT_EQ(gs_TranslationCalls.load(std::memory_order_relaxed), 1);

	IOHANDLE LimitedFile = io_open(aLimitedFilename, IOFLAG_WRITE);
	ASSERT_TRUE(LimitedFile);
	{
		auto pLogger = log_logger_file_limited(LimitedFile, 0, nullptr);
		pLogger->Log(&Message);
	}
	EXPECT_EQ(gs_TranslationCalls.load(std::memory_order_relaxed), 1);

	IOHANDLE AutoOutputFile = io_open(aAutoFilename, IOFLAG_READ);
	ASSERT_TRUE(AutoOutputFile);
	char *pAutoOutput = io_read_all_str(AutoOutputFile);
	io_close(AutoOutputFile);
	ASSERT_TRUE(pAutoOutput);
	EXPECT_NE(str_find(pAutoOutput, "prefix: translated"), nullptr);
	EXPECT_EQ(str_find(pAutoOutput, "original"), nullptr);
	free(pAutoOutput);

	IOHANDLE LimitedOutputFile = io_open(aLimitedFilename, IOFLAG_READ);
	ASSERT_TRUE(LimitedOutputFile);
	char *pLimitedOutput = io_read_all_str(LimitedOutputFile);
	io_close(LimitedOutputFile);
	ASSERT_TRUE(pLimitedOutput);
	EXPECT_NE(str_find(pLimitedOutput, "prefix: original"), nullptr);
	EXPECT_EQ(str_find(pLimitedOutput, "translated"), nullptr);
	free(pLimitedOutput);

	EXPECT_FALSE(fs_remove(aAutoFilename));
	EXPECT_FALSE(fs_remove(aLimitedFilename));
}
