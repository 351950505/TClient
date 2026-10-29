#include "translate.h"

#include <base/dbg.h>
#include <base/hash.h>
#include <base/hash_ctxt.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/secure.h>
#include <base/time.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/protocol.h>

#include <game/client/gameclient.h>
#include <game/client/lineinput.h>
#include <game/localization.h>

#include <algorithm>
#include <memory>

static void UrlEncode(const char *pText, char *pOut, size_t Length)
{
	if(Length == 0)
		return;
	size_t OutPos = 0;
	for(const char *p = pText; *p && OutPos < Length - 1; ++p)
	{
		unsigned char c = *(const unsigned char *)p;
		if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
		{
			if(OutPos >= Length - 1)
				break;
			pOut[OutPos++] = c;
		}
		else
		{
			if(OutPos + 3 >= Length)
				break;
			snprintf(pOut + OutPos, 4, "%%%02X", c);
			OutPos += 3;
		}
	}
	pOut[OutPos] = '\0';
}

static std::string UrlEncodeString(const char *pText)
{
	std::string Out;
	for(const unsigned char *p = (const unsigned char *)pText; *p; ++p)
	{
		if(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
			Out += (char)*p;
		else
		{
			char aHex[4];
			str_format(aHex, sizeof(aHex), "%%%02X", *p);
			Out += aHex;
		}
	}
	return Out;
}

static std::string HexEncode(const unsigned char *pData, size_t Size)
{
	static const char s_aHex[] = "0123456789abcdef";
	std::string Out;
	Out.reserve(Size * 2);
	for(size_t i = 0; i < Size; i++)
	{
		Out += s_aHex[pData[i] >> 4];
		Out += s_aHex[pData[i] & 0xf];
	}
	return Out;
}

static std::string Sha256Hex(const std::string &Data)
{
	const SHA256_DIGEST Digest = sha256(Data.data(), Data.size());
	return HexEncode(Digest.data, sizeof(Digest.data));
}

// HMAC-SHA256 (RFC 2104) on top of base/hash_ctxt.h's sha256 primitives,
// which don't provide an HMAC themselves.
static SHA256_DIGEST HmacSha256(const void *pKey, size_t KeySize, const void *pData, size_t DataSize)
{
	constexpr size_t BLOCK_SIZE = 64;
	unsigned char aKey[BLOCK_SIZE] = {0};
	if(KeySize > BLOCK_SIZE)
	{
		const SHA256_DIGEST KeyDigest = sha256(pKey, KeySize);
		mem_copy(aKey, KeyDigest.data, sizeof(KeyDigest.data));
	}
	else if(KeySize > 0)
		mem_copy(aKey, pKey, KeySize);

	unsigned char aInnerPad[BLOCK_SIZE];
	unsigned char aOuterPad[BLOCK_SIZE];
	for(size_t i = 0; i < BLOCK_SIZE; i++)
	{
		aInnerPad[i] = aKey[i] ^ 0x36;
		aOuterPad[i] = aKey[i] ^ 0x5c;
	}

	SHA256_CTX Ctx;
	sha256_init(&Ctx);
	sha256_update(&Ctx, aInnerPad, BLOCK_SIZE);
	sha256_update(&Ctx, pData, DataSize);
	const SHA256_DIGEST Inner = sha256_finish(&Ctx);

	sha256_init(&Ctx);
	sha256_update(&Ctx, aOuterPad, BLOCK_SIZE);
	sha256_update(&Ctx, Inner.data, sizeof(Inner.data));
	return sha256_finish(&Ctx);
}

static SHA256_DIGEST HmacSha256(const SHA256_DIGEST &Key, const char *pData)
{
	return HmacSha256(Key.data, sizeof(Key.data), pData, str_length(pData));
}

// Shared "pick an entry from a code map, else pass through" helper for
// backends whose language codes differ from our ISO 639-1 config values.
struct STranslateCodeMap
{
	const char *m_pOurs;
	const char *m_pTheirs;
};
template<size_t N>
static const char *MapLanguageCode(const STranslateCodeMap (&aMap)[N], const char *pCode)
{
	for(const auto &Entry : aMap)
		if(str_comp_nocase(pCode, Entry.m_pOurs) == 0)
			return Entry.m_pTheirs;
	return pCode;
}
template<size_t N>
static const char *UnmapLanguageCode(const STranslateCodeMap (&aMap)[N], const char *pCode)
{
	for(const auto &Entry : aMap)
		if(str_comp_nocase(pCode, Entry.m_pTheirs) == 0)
			return Entry.m_pOurs;
	return pCode;
}

const char *ITranslateBackend::EncodeTarget(const char *pTarget) const
{
	if(!pTarget || pTarget[0] == '\0')
		return DefaultConfig::TcTranslateTarget;
	return pTarget;
}

bool ITranslateBackend::CompareTargets(const char *pA, const char *pB) const
{
	if(pA == pB) // if(!pA && !pB)
		return true;
	if(!pA || !pB)
		return false;
	if(str_comp_nocase(EncodeTarget(pA), EncodeTarget(pB)) == 0)
		return true;
	return false;
}

class ITranslateBackendHttp : public ITranslateBackend
{
protected:
	using ITranslateBackend::ITranslateBackend;
	std::shared_ptr<IHttpRequest> m_pHttpRequest = nullptr;
	// Set by a backend constructor that can't build a request (missing
	// credentials etc.) -- Update() then fails immediately with this text.
	char m_aInitError[256] = "";
	virtual bool ParseResponse(CTranslateResponse &Out) = 0;

	// Builds a POST request (headers can be added by the caller before
	// RunHttpRequest) without starting it.
	void PrepareHttpPost(const char *pUrl, const char *pContentType, const void *pBody, size_t BodySize)
	{
		auto pPost = HttpGet(pUrl);
		pPost->LogProgress(HTTPLOG::FAILURE);
		pPost->FailOnErrorStatus(false);
		pPost->Timeout(CTimeout{10000, 30000, 500, 10});
		pPost->HeaderString("Content-Type", pContentType);
		pPost->Post((const unsigned char *)pBody, BodySize);
		m_pHttpRequest = std::move(pPost);
	}
	void RunHttpRequest(IHttp &Http) { Http.Run(m_pHttpRequest); }
	virtual bool ParseHttpError() const { return false; }

	void CreateHttpRequest(IHttp &Http, const char *pUrl)
	{
		auto pGet = HttpGet(pUrl);
		pGet->LogProgress(HTTPLOG::FAILURE);
		pGet->FailOnErrorStatus(false);
		pGet->Timeout(CTimeout{10000, 0, 500, 10});

		m_pHttpRequest = std::move(pGet);
		Http.Run(m_pHttpRequest);
	}

	// Same as CreateHttpRequest, but POSTs pBody as a
	// application/x-www-form-urlencoded body instead of encoding everything
	// into the URL's query string -- keeps the URL itself short (the fixed
	// CHttpRequest::m_aUrl buffer is 256 bytes, easily exceeded by a long
	// chat message otherwise) and matches what the endpoint actually expects.
	void CreateHttpRequestPost(IHttp &Http, const char *pUrl, const char *pBody)
	{
		auto pPost = HttpGet(pUrl);
		pPost->LogProgress(HTTPLOG::FAILURE);
		pPost->FailOnErrorStatus(false);
		pPost->Timeout(CTimeout{10000, 0, 500, 10});
		pPost->HeaderString("Content-Type", "application/x-www-form-urlencoded");
		pPost->Post((const unsigned char *)pBody, str_length(pBody));

		m_pHttpRequest = std::move(pPost);
		Http.Run(m_pHttpRequest);
	}

public:
	std::optional<bool> Update(CTranslateResponse &Out) override
	{
		if(m_aInitError[0] != '\0')
		{
			str_copy(Out.m_Text, m_aInitError);
			return false;
		}
		dbg_assert(m_pHttpRequest != nullptr, "m_pHttpRequest is nullptr");
		if(m_pHttpRequest->State() == EHttpState::RUNNING || m_pHttpRequest->State() == EHttpState::QUEUED)
			return std::nullopt;
		if(m_pHttpRequest->State() == EHttpState::ABORTED)
		{
			str_copy(Out.m_Text, "Aborted");
			return false;
		}
		if(m_pHttpRequest->State() != EHttpState::DONE)
		{
			str_copy(Out.m_Text, "Curl error, see console");
			return false;
		}
		if(m_pHttpRequest->StatusCode() != 200 && !ParseHttpError())
		{
			str_format(Out.m_Text, sizeof(Out.m_Text), "Got http code %d", m_pHttpRequest->StatusCode());
			return false;
		}
		return ParseResponse(Out);
	}
	~ITranslateBackendHttp() override
	{
		if(m_pHttpRequest)
			m_pHttpRequest->Abort();
	}
};

class CTranslateBackendLibretranslate : public ITranslateBackendHttp
{
private:
	bool ParseResponseJson(const json_value *pObj, CTranslateResponse &Out)
	{
		if(!pObj)
		{
			str_copy(Out.m_Text, "Response is not JSON");
			return false;
		}

		if(pObj->type != json_object)
		{
			str_copy(Out.m_Text, "Response is not object");
			return false;
		}

		const json_value *pError = json_object_get(pObj, "error");
		if(pError != &json_value_none)
		{
			if(pError->type != json_string)
				str_copy(Out.m_Text, "Error is not string");
			else
				str_copy(Out.m_Text, pError->u.string.ptr);
			return false;
		}

		const json_value *pTranslatedText = json_object_get(pObj, "translatedText");
		if(pTranslatedText == &json_value_none)
		{
			str_copy(Out.m_Text, "No translatedText");
			return false;
		}
		if(pTranslatedText->type != json_string)
		{
			str_copy(Out.m_Text, "translatedText is not string");
			return false;
		}

		const json_value *pDetectedLanguage = json_object_get(pObj, "detectedLanguage");
		if(pDetectedLanguage == &json_value_none)
		{
			str_copy(Out.m_Text, "No pDetectedLanguage");
			return false;
		}
		if(pDetectedLanguage->type != json_object)
		{
			str_copy(Out.m_Text, "pDetectedLanguage is not object");
			return false;
		}

		const json_value *pConfidence = json_object_get(pDetectedLanguage, "confidence");
		if(pConfidence == &json_value_none || ((pConfidence->type == json_double && pConfidence->u.dbl == 0.0f) ||
							      (pConfidence->type == json_integer && pConfidence->u.integer == 0)))
		{
			str_copy(Out.m_Text, "Unknown language");
			return false;
		}

		const json_value *pLanguage = json_object_get(pDetectedLanguage, "language");
		if(pLanguage == &json_value_none)
		{
			str_copy(Out.m_Text, "No language");
			return false;
		}
		if(pLanguage->type != json_string)
		{
			str_copy(Out.m_Text, "language is not string");
			return false;
		}

		str_copy(Out.m_Text, pTranslatedText->u.string.ptr);
		str_copy(Out.m_Language, pLanguage->u.string.ptr);

		return true;
	}

protected:
	bool ParseResponse(CTranslateResponse &Out) override
	{
		json_value *pObj = m_pHttpRequest->ResultJson();
		bool Res = ParseResponseJson(pObj, Out);
		json_value_free(pObj);
		return Res;
	}
	bool ParseHttpError() const override { return true; }

public:
	CTranslateBackendLibretranslate(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		CJsonStringWriter Json = CJsonStringWriter();
		Json.BeginObject();
		Json.WriteAttribute("q");
		Json.WriteStrValue(pText);
		Json.WriteAttribute("source");
		Json.WriteStrValue("auto");
		Json.WriteAttribute("target");
		Json.WriteStrValue(EncodeTarget(pTargetLang));
		Json.WriteAttribute("format");
		Json.WriteStrValue("text");
		if(g_Config.m_TcTranslateKey[0] != '\0')
		{
			Json.WriteAttribute("api_key");
			Json.WriteStrValue(g_Config.m_TcTranslateKey);
		}
		Json.EndObject();
		CreateHttpRequest(Http, g_Config.m_TcTranslateEndpoint[0] == '\0' ? "localhost:5000/translate" : g_Config.m_TcTranslateEndpoint);
		const char *pJson = Json.GetOutputString().c_str();
		m_pHttpRequest->PostJson(pJson);
	}
};

// Google's secondary free endpoint (clients5 / dict-chrome-ex). Same
// unofficial-status caveat as the primary one, but it is a different host and
// a different response shape (a flat array of strings), so it keeps working
// when translate.googleapis.com is blocked or rate limited. Verified working
// 2026-10-29: GET returns ["translated","detected-src"] for one string and a
// single joined string for multi-line input.
class CTranslateBackendGoogleAlt : public ITranslateBackendHttp
{
private:
	bool ParseResponse(CTranslateResponse &Out) override
	{
		json_value *pObj = m_pHttpRequest->ResultJson();
		bool Res = false;
		if(pObj && pObj->type == json_array && json_array_length(pObj) >= 1)
		{
			const json_value *pFirst = json_array_get(pObj, 0);
			if(pFirst && pFirst->type == json_string)
			{
				str_copy(Out.m_Text, pFirst->u.string.ptr);
				Res = Out.m_Text[0] != '\0';
			}
			if(json_array_length(pObj) >= 2)
			{
				const json_value *pLang = json_array_get(pObj, 1);
				if(pLang && pLang->type == json_string)
					str_copy(Out.m_Language, pLang->u.string.ptr);
			}
			if(!Res)
				str_copy(Out.m_Text, "Empty translation");
		}
		else
		{
			str_copy(Out.m_Text, "Unexpected response format");
		}
		json_value_free(pObj);
		return Res;
	}

public:
	CTranslateBackendGoogleAlt(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		// GET with query string: this endpoint does not accept a form body.
		// The URL buffer is 256 bytes, so long messages are truncated rather
		// than silently failing -- chat lines are capped well below that.
		char aText[192];
		UrlEncode(pText, aText, sizeof(aText));
		char aUrl[256];
		const char *pBase = g_Config.m_TcTranslateEndpoint[0] != '\0' ? g_Config.m_TcTranslateEndpoint : "https://clients5.google.com/translate_a/t";
		str_format(aUrl, sizeof(aUrl), "%s?client=dict-chrome-ex&sl=auto&tl=%s&q=%s", pBase, EncodeTarget(pTargetLang), aText);
		CreateHttpRequest(Http, aUrl);
	}
};

// Talks directly to Google's free, unofficial (undocumented) translate
// endpoint -- this is the same endpoint the now-defunct FreeTranslateAPI
// (ftapi.pythonanywhere.com) used to wrap, without needing a third-party
// mirror to stay up. See https://github.com/TaterClient/TClient/pull/203
// for context on why the old FTApi-wrapping backend was removed instead of
// just re-pointed: its hosting is gone, and anyone who wants a self-hosted
// option already has the LibreTranslate backend for that.
class CTranslateBackendGoogle : public ITranslateBackendHttp
{
private:
	bool ParseResponseJson(const json_value *pObj, CTranslateResponse &Out)
	{
		if(!pObj)
		{
			str_copy(Out.m_Text, "Response is not JSON");
			return false;
		}
		if(pObj->type != json_array || json_array_length(pObj) < 3)
		{
			str_copy(Out.m_Text, "Unexpected response format");
			return false;
		}

		const json_value *pSentences = json_array_get(pObj, 0);
		if(!pSentences || pSentences->type != json_array)
		{
			str_copy(Out.m_Text, "No translated sentences");
			return false;
		}

		char aText[sizeof(Out.m_Text)] = "";
		const int NumSentences = json_array_length(pSentences);
		for(int i = 0; i < NumSentences; i++)
		{
			const json_value *pSentence = json_array_get(pSentences, i);
			if(!pSentence || pSentence->type != json_array || json_array_length(pSentence) < 1)
				continue;
			const json_value *pChunk = json_array_get(pSentence, 0);
			if(!pChunk || pChunk->type != json_string)
				continue;
			str_append(aText, pChunk->u.string.ptr);
		}
		if(aText[0] == '\0')
		{
			str_copy(Out.m_Text, "Empty translation");
			return false;
		}

		const json_value *pLanguage = json_array_get(pObj, 2);
		if(pLanguage && pLanguage->type == json_string)
			str_copy(Out.m_Language, pLanguage->u.string.ptr);

		str_copy(Out.m_Text, aText);
		return true;
	}

protected:
	bool ParseResponse(CTranslateResponse &Out) override
	{
		json_value *pObj = m_pHttpRequest->ResultJson();
		bool Res = ParseResponseJson(pObj, Out);
		json_value_free(pObj);
		return Res;
	}

public:
	CTranslateBackendGoogle(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		// Query params go in the POST body, not the URL -- keeps the URL
		// itself short and fixed (see CreateHttpRequestPost) regardless of
		// how long pText is.
		char aBody[4096];
		str_format(aBody, sizeof(aBody), "client=gtx&sl=auto&dt=t&tl=%s&q=", EncodeTarget(pTargetLang));

		UrlEncode(pText, aBody + strlen(aBody), sizeof(aBody) - strlen(aBody));

		const char *pEndpoint = g_Config.m_TcTranslateEndpoint[0] != '\0' ? g_Config.m_TcTranslateEndpoint : "https://translate.googleapis.com/translate_a/single";
		CreateHttpRequestPost(Http, pEndpoint, aBody);
	}
};

// Common "parse JSON body, hand to ParseResponseJson, free" wrapper shared
// by the backends below.
#define TRANSLATE_JSON_PARSE_RESPONSE() \
	bool ParseResponse(CTranslateResponse &Out) override \
	{ \
		json_value *pObj = m_pHttpRequest->ResultJson(); \
		const bool Res = ParseResponseJson(pObj, Out); \
		json_value_free(pObj); \
		return Res; \
	}

// Tencent Cloud Machine Translation (TMT) TextTranslate, signed with
// TC3-HMAC-SHA256. https://cloud.tencent.com/document/api/551/15619
class CTranslateBackendTencent : public ITranslateBackendHttp
{
private:
	static constexpr const char *HOST = "tmt.tencentcloudapi.com";
	static constexpr const char *SERVICE = "tmt";
	static constexpr const char *CONTENT_TYPE = "application/json; charset=utf-8";

	bool ParseResponseJson(const json_value *pObj, CTranslateResponse &Out)
	{
		if(!pObj || pObj->type != json_object)
		{
			str_copy(Out.m_Text, "Response is not JSON object");
			return false;
		}
		const json_value *pResponse = json_object_get(pObj, "Response");
		if(pResponse == &json_value_none || pResponse->type != json_object)
		{
			str_copy(Out.m_Text, "No Response object");
			return false;
		}
		const json_value *pError = json_object_get(pResponse, "Error");
		if(pError != &json_value_none)
		{
			const json_value *pCode = json_object_get(pError, "Code");
			const json_value *pMessage = json_object_get(pError, "Message");
			str_format(Out.m_Text, sizeof(Out.m_Text), "%s: %s",
				pCode->type == json_string ? pCode->u.string.ptr : "UnknownError",
				pMessage->type == json_string ? pMessage->u.string.ptr : "TencentCloud request failed");
			return false;
		}
		const json_value *pTargetText = json_object_get(pResponse, "TargetText");
		if(pTargetText->type != json_string)
		{
			str_copy(Out.m_Text, "No TargetText");
			return false;
		}
		const json_value *pSource = json_object_get(pResponse, "Source");
		if(pSource->type == json_string)
			str_copy(Out.m_Language, pSource->u.string.ptr);
		str_copy(Out.m_Text, pTargetText->u.string.ptr);
		return true;
	}

protected:
	TRANSLATE_JSON_PARSE_RESPONSE()
	bool ParseHttpError() const override { return true; }

public:
	const char *EncodeTarget(const char *pTarget) const override
	{
		const char *pCode = ITranslateBackend::EncodeTarget(pTarget);
		if(str_comp_nocase(pCode, "zh-cn") == 0)
			return "zh";
		if(str_comp_nocase(pCode, "zh-tw") == 0)
			return "zh-TW";
		return pCode;
	}

	CTranslateBackendTencent(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		const char *pSecretId = g_Config.m_TcTranslateTencentSecretId;
		const char *pSecretKey = g_Config.m_TcTranslateTencentSecretKey;
		if(pSecretId[0] == '\0' || pSecretKey[0] == '\0')
		{
			str_copy(m_aInitError, "Configure the TencentCloud SecretId and SecretKey");
			return;
		}

		CJsonStringWriter Json;
		Json.BeginObject();
		Json.WriteAttribute("ProjectId");
		Json.WriteIntValue(0);
		Json.WriteAttribute("Source");
		Json.WriteStrValue("auto");
		Json.WriteAttribute("SourceText");
		Json.WriteStrValue(pText);
		Json.WriteAttribute("Target");
		Json.WriteStrValue(EncodeTarget(pTargetLang));
		Json.EndObject();
		const std::string Payload = Json.GetOutputString();

		const int64_t Timestamp = time_timestamp();
		const time_t TimeValue = (time_t)Timestamp;
		std::tm UtcTime{};
#if defined(CONF_FAMILY_WINDOWS)
		gmtime_s(&UtcTime, &TimeValue);
#else
		gmtime_r(&TimeValue, &UtcTime);
#endif
		char aDate[16];
		strftime(aDate, sizeof(aDate), "%Y-%m-%d", &UtcTime);
		char aTimestamp[32];
		str_format(aTimestamp, sizeof(aTimestamp), "%lld", (long long)Timestamp);

		const std::string SignedHeaders = "content-type;host";
		const std::string CanonicalRequest = std::string("POST\n/\n\n") +
						     "content-type:" + CONTENT_TYPE + "\nhost:" + HOST + "\n\n" +
						     SignedHeaders + "\n" + Sha256Hex(Payload);
		const std::string CredentialScope = std::string(aDate) + "/" + SERVICE + "/tc3_request";
		const std::string StringToSign = std::string("TC3-HMAC-SHA256\n") + aTimestamp + "\n" + CredentialScope + "\n" + Sha256Hex(CanonicalRequest);

		const std::string SecretPrefix = std::string("TC3") + pSecretKey;
		const SHA256_DIGEST SecretDate = HmacSha256(SecretPrefix.data(), SecretPrefix.size(), aDate, str_length(aDate));
		const SHA256_DIGEST SecretService = HmacSha256(SecretDate, SERVICE);
		const SHA256_DIGEST SecretSigning = HmacSha256(SecretService, "tc3_request");
		const SHA256_DIGEST Signature = HmacSha256(SecretSigning, StringToSign.c_str());

		const std::string Authorization = std::string("TC3-HMAC-SHA256 Credential=") + pSecretId + "/" + CredentialScope +
						  ", SignedHeaders=" + SignedHeaders +
						  ", Signature=" + HexEncode(Signature.data, sizeof(Signature.data));

		PrepareHttpPost("https://tmt.tencentcloudapi.com/", CONTENT_TYPE, Payload.data(), Payload.size());
		m_pHttpRequest->HeaderString("Authorization", Authorization.c_str());
		m_pHttpRequest->HeaderString("X-TC-Action", "TextTranslate");
		m_pHttpRequest->HeaderString("X-TC-Version", "2018-03-21");
		m_pHttpRequest->HeaderString("X-TC-Region", g_Config.m_TcTranslateTencentRegion[0] != '\0' ? g_Config.m_TcTranslateTencentRegion : "ap-guangzhou");
		m_pHttpRequest->HeaderString("X-TC-Timestamp", aTimestamp);
		RunHttpRequest(Http);
	}
};

// Baidu Translate open platform (general text translation), MD5-signed.
// https://fanyi-api.baidu.com/doc/21
static const STranslateCodeMap s_aBaiduCodes[] = {
	{"zh", "zh"}, {"zh-TW", "cht"}, {"ja", "jp"}, {"ko", "kor"}, {"fr", "fra"}, {"es", "spa"},
	{"ar", "ara"}, {"bg", "bul"}, {"da", "dan"}, {"et", "est"}, {"fi", "fin"}, {"ro", "rom"},
	{"sl", "slo"}, {"sv", "swe"}, {"vi", "vie"}, {"uk", "ukr"}};

class CTranslateBackendBaidu : public ITranslateBackendHttp
{
private:
	bool ParseResponseJson(const json_value *pObj, CTranslateResponse &Out)
	{
		if(!pObj || pObj->type != json_object)
		{
			str_copy(Out.m_Text, "Response is not JSON object");
			return false;
		}
		const json_value *pError = json_object_get(pObj, "error_code");
		if(pError != &json_value_none)
		{
			const int Code = pError->type == json_string ? str_toint(pError->u.string.ptr) : pError->type == json_integer ? (int)pError->u.integer : -1;
			if(Code != 52000)
			{
				const json_value *pMsg = json_object_get(pObj, "error_msg");
				str_format(Out.m_Text, sizeof(Out.m_Text), "Baidu error %d: %s", Code, pMsg->type == json_string ? pMsg->u.string.ptr : "");
				return false;
			}
		}
		const json_value *pResults = json_object_get(pObj, "trans_result");
		if(pResults->type != json_array || pResults->u.array.length == 0)
		{
			str_copy(Out.m_Text, "No trans_result");
			return false;
		}
		char aText[sizeof(Out.m_Text)] = "";
		for(unsigned int i = 0; i < pResults->u.array.length; ++i)
		{
			const json_value *pItem = pResults->u.array.values[i];
			const json_value *pDst = pItem->type == json_object ? json_object_get(pItem, "dst") : &json_value_none;
			if(pDst->type != json_string)
			{
				str_copy(Out.m_Text, "trans_result item has no dst");
				return false;
			}
			if(i > 0)
				str_append(aText, "\n");
			str_append(aText, pDst->u.string.ptr);
		}
		const json_value *pFrom = json_object_get(pObj, "from");
		if(pFrom->type == json_string)
			str_copy(Out.m_Language, UnmapLanguageCode(s_aBaiduCodes, pFrom->u.string.ptr));
		str_copy(Out.m_Text, aText);
		return true;
	}

protected:
	TRANSLATE_JSON_PARSE_RESPONSE()
	bool ParseHttpError() const override { return true; }

public:
	const char *EncodeTarget(const char *pTarget) const override
	{
		return MapLanguageCode(s_aBaiduCodes, ITranslateBackend::EncodeTarget(pTarget));
	}

	CTranslateBackendBaidu(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		const char *pAppId = g_Config.m_TcTranslateBaiduAppId;
		const char *pKey = g_Config.m_TcTranslateBaiduKey;
		if(pAppId[0] == '\0' || pKey[0] == '\0')
		{
			str_copy(m_aInitError, "Configure the Baidu APP ID and key");
			return;
		}
		// Fresh salt per request; the key itself is only used for the
		// signature, never sent.
		unsigned int SaltValue;
		secure_random_fill(&SaltValue, sizeof(SaltValue));
		char aSalt[16];
		str_format(aSalt, sizeof(aSalt), "%u", SaltValue);
		// sign = MD5(appid + q + salt + key), over the raw (not URL-encoded) text
		const std::string SignInput = std::string(pAppId) + pText + aSalt + pKey;
		char aSign[MD5_MAXSTRSIZE];
		md5_str(md5(SignInput.data(), SignInput.size()), aSign, sizeof(aSign));

		const std::string Body = "q=" + UrlEncodeString(pText) + "&from=auto&to=" + UrlEncodeString(EncodeTarget(pTargetLang)) +
					 "&appid=" + UrlEncodeString(pAppId) + "&salt=" + aSalt + "&sign=" + aSign;
		PrepareHttpPost("https://fanyi-api.baidu.com/api/trans/vip/translate", "application/x-www-form-urlencoded; charset=UTF-8", Body.data(), Body.size());
		RunHttpRequest(Http);
	}
};

// DeepL API v2. Free-tier keys end with ":fx" and use api-free.deepl.com.
class CTranslateBackendDeepl : public ITranslateBackendHttp
{
private:
	mutable char m_aTarget[16] = "";

	bool ParseResponseJson(const json_value *pObj, CTranslateResponse &Out)
	{
		if(!pObj || pObj->type != json_object)
		{
			str_copy(Out.m_Text, "Response is not JSON object");
			return false;
		}
		const json_value *pTranslations = json_object_get(pObj, "translations");
		if(pTranslations->type != json_array || pTranslations->u.array.length == 0)
		{
			str_copy(Out.m_Text, "No translations");
			return false;
		}
		const json_value *pTranslation = pTranslations->u.array.values[0];
		const json_value *pText = pTranslation->type == json_object ? json_object_get(pTranslation, "text") : &json_value_none;
		if(pText->type != json_string)
		{
			str_copy(Out.m_Text, "No translation text");
			return false;
		}
		const json_value *pDetected = json_object_get(pTranslation, "detected_source_language");
		if(pDetected->type == json_string)
		{
			str_utf8_tolower(pDetected->u.string.ptr, Out.m_Language, sizeof(Out.m_Language));
		}
		str_copy(Out.m_Text, pText->u.string.ptr);
		return true;
	}

protected:
	bool ParseResponse(CTranslateResponse &Out) override
	{
		const int StatusCode = m_pHttpRequest->StatusCode();
		if(StatusCode == 403)
		{
			str_copy(Out.m_Text, "DeepL: invalid API key (free keys end with :fx)");
			return false;
		}
		if(StatusCode == 429)
		{
			str_copy(Out.m_Text, "DeepL: too many requests, try again later");
			return false;
		}
		if(StatusCode == 456)
		{
			str_copy(Out.m_Text, "DeepL: character quota exceeded");
			return false;
		}
		json_value *pObj = m_pHttpRequest->ResultJson();
		bool Res;
		if(StatusCode != 200)
		{
			const json_value *pMessage = pObj && pObj->type == json_object ? json_object_get(pObj, "message") : &json_value_none;
			if(pMessage->type == json_string)
				str_format(Out.m_Text, sizeof(Out.m_Text), "DeepL HTTP %d: %.200s", StatusCode, pMessage->u.string.ptr);
			else
				str_format(Out.m_Text, sizeof(Out.m_Text), "DeepL HTTP %d", StatusCode);
			Res = false;
		}
		else
			Res = ParseResponseJson(pObj, Out);
		json_value_free(pObj);
		return Res;
	}
	bool ParseHttpError() const override { return true; }

public:
	const char *EncodeTarget(const char *pTarget) const override
	{
		// Returns an upper-cased DeepL code. CompareTargets calls this
		// twice in one expression, but str_comp_nocase makes both encodings
		// compare equal anyway, so the base default is used for comparison.
		const char *pCode = ITranslateBackend::EncodeTarget(pTarget);
		if(str_comp_nocase(pCode, "zh") == 0 || str_comp_nocase(pCode, "zh-cn") == 0)
			return "ZH";
		if(str_comp_nocase(pCode, "zh-tw") == 0)
			return "ZH-HANT";
		if(str_comp_nocase(pCode, "en") == 0)
			return "EN-US";
		if(str_comp_nocase(pCode, "pt") == 0)
			return "PT-BR";
		str_copy(m_aTarget, pCode);
		for(char *p = m_aTarget; *p; ++p)
			*p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
		return m_aTarget;
	}
	// EncodeTarget may return the shared m_aTarget buffer, so comparing two
	// encodings directly would always match -- compare our own codes instead.
	bool CompareTargets(const char *pA, const char *pB) const override
	{
		if(!pA || !pB)
			return pA == pB;
		return str_comp_nocase(ITranslateBackend::EncodeTarget(pA), ITranslateBackend::EncodeTarget(pB)) == 0;
	}

	CTranslateBackendDeepl(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		const char *pApiKey = g_Config.m_TcTranslateDeeplKey;
		if(pApiKey[0] == '\0')
		{
			str_copy(m_aInitError, "Configure the DeepL API key");
			return;
		}
		const char *pUrl = str_endswith_nocase(pApiKey, ":fx") ? "https://api-free.deepl.com/v2/translate" : "https://api.deepl.com/v2/translate";

		CJsonStringWriter Json;
		Json.BeginObject();
		Json.WriteAttribute("text");
		Json.BeginArray();
		Json.WriteStrValue(pText);
		Json.EndArray();
		Json.WriteAttribute("target_lang");
		Json.WriteStrValue(EncodeTarget(pTargetLang));
		Json.EndObject();
		const std::string Payload = Json.GetOutputString();

		PrepareHttpPost(pUrl, "application/json", Payload.data(), Payload.size());
		char aAuthorization[256];
		str_format(aAuthorization, sizeof(aAuthorization), "DeepL-Auth-Key %s", pApiKey);
		m_pHttpRequest->HeaderString("Authorization", aAuthorization);
		RunHttpRequest(Http);
	}
};

// MyMemory (api.mymemory.translated.net): free, anonymous, no key. It has
// no source auto-detection, so the source is guessed from the script.
class CTranslateBackendMymemory : public ITranslateBackendHttp
{
private:
	static constexpr int MAX_QUERY_BYTES = 500;

	static const char *EncodeLangCode(const char *pCode)
	{
		if(str_comp_nocase(pCode, "zh") == 0 || str_comp_nocase(pCode, "zh-cn") == 0)
			return "zh-CN";
		if(str_comp_nocase(pCode, "zh-tw") == 0)
			return "zh-TW";
		return pCode;
	}

	static const char *GuessSource(const char *pText)
	{
		int Kana = 0, Hangul = 0, Cyrillic = 0, Han = 0, Arabic = 0, Greek = 0;
		const char *p = pText;
		int Code;
		while((Code = str_utf8_decode(&p)) > 0)
		{
			if((Code >= 0x3040 && Code <= 0x30FF))
				Kana++;
			else if(Code >= 0xAC00 && Code <= 0xD7AF)
				Hangul++;
			else if(Code >= 0x0400 && Code <= 0x04FF)
				Cyrillic++;
			else if(Code >= 0x4E00 && Code <= 0x9FFF)
				Han++;
			else if(Code >= 0x0600 && Code <= 0x06FF)
				Arabic++;
			else if(Code >= 0x0370 && Code <= 0x03FF)
				Greek++;
		}
		if(Kana > 0)
			return "ja";
		if(Hangul > 0)
			return "ko";
		if(Cyrillic > 0)
			return "ru";
		if(Han > 0)
			return "zh-CN";
		if(Arabic > 0)
			return "ar";
		if(Greek > 0)
			return "el";
		return "en";
	}

	bool ParseResponseJson(const json_value *pObj, CTranslateResponse &Out)
	{
		if(!pObj || pObj->type != json_object)
		{
			str_copy(Out.m_Text, "Response is not JSON object");
			return false;
		}
		int Status = 200;
		const json_value *pStatus = json_object_get(pObj, "responseStatus");
		if(pStatus->type == json_integer)
			Status = (int)pStatus->u.integer;
		else if(pStatus->type == json_double)
			Status = (int)pStatus->u.dbl;
		else if(pStatus->type == json_string)
			Status = str_toint(pStatus->u.string.ptr);

		const json_value *pData = json_object_get(pObj, "responseData");
		const json_value *pTranslated = pData->type == json_object ? json_object_get(pData, "translatedText") : &json_value_none;
		if(pTranslated->type != json_string)
		{
			const json_value *pDetails = json_object_get(pObj, "responseDetails");
			str_format(Out.m_Text, sizeof(Out.m_Text), "MyMemory error %d: %.200s", Status, pDetails->type == json_string ? pDetails->u.string.ptr : "no translatedText");
			return false;
		}
		// Quota/invalid-pair errors arrive as "MYMEMORY WARNING" text
		if(str_startswith_nocase(pTranslated->u.string.ptr, "MYMEMORY WARNING"))
		{
			str_format(Out.m_Text, sizeof(Out.m_Text), "MyMemory: %.200s", pTranslated->u.string.ptr);
			return false;
		}
		if(Status != 200)
		{
			str_format(Out.m_Text, sizeof(Out.m_Text), "MyMemory error %d: %.200s", Status, pTranslated->u.string.ptr);
			return false;
		}
		str_copy(Out.m_Text, pTranslated->u.string.ptr);
		return true;
	}

protected:
	TRANSLATE_JSON_PARSE_RESPONSE()
	bool ParseHttpError() const override { return true; }

public:
	const char *EncodeTarget(const char *pTarget) const override
	{
		return EncodeLangCode(ITranslateBackend::EncodeTarget(pTarget));
	}

	CTranslateBackendMymemory(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		if(str_length(pText) > MAX_QUERY_BYTES)
		{
			str_copy(m_aInitError, "MyMemory input exceeds 500 bytes");
			return;
		}
		const char *pSource = GuessSource(pText);
		str_copy(m_aSource, pSource);
		// Form POST keeps the URL short (CHttpRequest's URL buffer is fixed).
		const std::string Body = "q=" + UrlEncodeString(pText) + "&langpair=" + UrlEncodeString(pSource) + "%7C" + UrlEncodeString(EncodeTarget(pTargetLang));
		PrepareHttpPost("https://api.mymemory.translated.net/get", "application/x-www-form-urlencoded", Body.data(), Body.size());
		RunHttpRequest(Http);
	}

	std::optional<bool> Update(CTranslateResponse &Out) override
	{
		const std::optional<bool> Res = ITranslateBackendHttp::Update(Out);
		if(Res.has_value() && *Res)
		{
			// Report the guessed source in our own code style
			if(str_comp_nocase(m_aSource, "zh-CN") == 0)
				str_copy(Out.m_Language, "zh");
			else
				str_copy(Out.m_Language, m_aSource);
		}
		return Res;
	}

private:
	char m_aSource[16] = "";
};

// Any OpenAI-compatible chat completions API (OpenAI, DeepSeek, Zhipu,
// local Ollama/LM Studio, ...). Endpoint may be a base URL (".../v1") or
// the full ".../chat/completions" URL.
class CTranslateBackendLlm : public ITranslateBackendHttp
{
private:
	bool ParseResponseJson(const json_value *pObj, CTranslateResponse &Out)
	{
		if(!pObj || pObj->type != json_object)
		{
			str_format(Out.m_Text, sizeof(Out.m_Text), "LLM: response is not JSON (HTTP %d), check the endpoint", m_pHttpRequest->StatusCode());
			return false;
		}
		const json_value *pError = json_object_get(pObj, "error");
		if(pError != &json_value_none && pError->type != json_null)
		{
			const json_value *pMessage = pError->type == json_object ? json_object_get(pError, "message") : pError;
			str_format(Out.m_Text, sizeof(Out.m_Text), "LLM HTTP %d: %.200s", m_pHttpRequest->StatusCode(), pMessage->type == json_string ? pMessage->u.string.ptr : "request failed");
			return false;
		}
		const json_value *pChoices = json_object_get(pObj, "choices");
		if(pChoices->type != json_array || pChoices->u.array.length == 0)
		{
			const json_value *pMsg = json_object_get(pObj, "msg"); // Zhipu style error
			str_format(Out.m_Text, sizeof(Out.m_Text), "LLM HTTP %d: %.200s", m_pHttpRequest->StatusCode(), pMsg->type == json_string ? pMsg->u.string.ptr : "no choices in response");
			return false;
		}
		const json_value *pChoice = pChoices->u.array.values[0];
		const json_value *pMessage = pChoice->type == json_object ? json_object_get(pChoice, "message") : &json_value_none;
		const json_value *pContent = pMessage->type == json_object ? json_object_get(pMessage, "content") : &json_value_none;
		if(pContent->type != json_string)
		{
			str_copy(Out.m_Text, "LLM: no message content");
			return false;
		}
		const char *pStart = str_utf8_skip_whitespaces(pContent->u.string.ptr);
		str_copy(Out.m_Text, pStart);
		str_utf8_trim_right(Out.m_Text);
		if(Out.m_Text[0] == '\0')
		{
			str_copy(Out.m_Text, "LLM: empty translation");
			return false;
		}
		return true;
	}

protected:
	TRANSLATE_JSON_PARSE_RESPONSE()
	bool ParseHttpError() const override { return true; }

public:
	CTranslateBackendLlm(IHttp &Http, const char *pText, const char *pTargetLang, const char *pName) :
		ITranslateBackendHttp(pName)
	{
		const char *pEndpoint = g_Config.m_TcTranslateLlmEndpoint[0] != '\0' ? g_Config.m_TcTranslateLlmEndpoint : "https://api.openai.com/v1";
		if(!str_startswith_nocase(pEndpoint, "https://") && !str_startswith_nocase(pEndpoint, "http://"))
		{
			str_copy(m_aInitError, "LLM endpoint must be an http(s) URL, e.g. https://api.example.com/v1");
			return;
		}
		const char *pApiKey = g_Config.m_TcTranslateLlmKey;
		const bool Local = str_find_nocase(pEndpoint, "://localhost") || str_find_nocase(pEndpoint, "://127.0.0.1");
		if(pApiKey[0] == '\0' && !Local)
		{
			str_copy(m_aInitError, "Configure the LLM API key");
			return;
		}

		// Normalize: strip trailing '/', append /chat/completions unless given
		char aUrl[512];
		str_copy(aUrl, pEndpoint);
		while(str_length(aUrl) > 0 && aUrl[str_length(aUrl) - 1] == '/')
			aUrl[str_length(aUrl) - 1] = '\0';
		if(!str_endswith_nocase(aUrl, "/chat/completions"))
			str_append(aUrl, "/chat/completions");

		char aSystem[512];
		str_format(aSystem, sizeof(aSystem),
			"You are a translation assistant for DDNet (DDraceNetwork) game chat. "
			"Translate the user's message into the language with ISO 639 code \"%s\". "
			"Keep game terms, player names and emoticons as they are. "
			"Output ONLY the translated text, nothing else.",
			EncodeTarget(pTargetLang));

		CJsonStringWriter Json;
		Json.BeginObject();
		Json.WriteAttribute("model");
		Json.WriteStrValue(g_Config.m_TcTranslateLlmModel[0] != '\0' ? g_Config.m_TcTranslateLlmModel : DefaultConfig::TcTranslateLlmModel);
		Json.WriteAttribute("messages");
		Json.BeginArray();
		Json.BeginObject();
		Json.WriteAttribute("role");
		Json.WriteStrValue("system");
		Json.WriteAttribute("content");
		Json.WriteStrValue(aSystem);
		Json.EndObject();
		Json.BeginObject();
		Json.WriteAttribute("role");
		Json.WriteStrValue("user");
		Json.WriteAttribute("content");
		Json.WriteStrValue(pText);
		Json.EndObject();
		Json.EndArray();
		Json.EndObject();
		const std::string Payload = Json.GetOutputString();

		PrepareHttpPost(aUrl, "application/json", Payload.data(), Payload.size());
		m_pHttpRequest->Timeout(CTimeout{10000, 60000, 100, 30});
		if(pApiKey[0] != '\0')
		{
			char aAuthorization[300];
			str_format(aAuthorization, sizeof(aAuthorization), "Bearer %s", pApiKey);
			m_pHttpRequest->HeaderString("Authorization", aAuthorization);
		}
		RunHttpRequest(Http);
	}
};

#undef TRANSLATE_JSON_PARSE_RESPONSE

// Per-backend supported-language lists for the settings menu -- Google
// supports far more languages than a typical self-hosted LibreTranslate
// instance, so these are deliberately not one shared list. Both are curated
// subsets (Google supports 100+ languages; LibreTranslate's exact set
// depends on which models a given self-hosted instance has installed), not
// a claim of exhaustive coverage.
static const std::vector<STranslateLanguage> s_vGoogleLanguages = {
	{"Arabic", "ar"},
	{"Bulgarian", "bg"},
	{"Chinese (Simplified)", "zh"},
	{"Chinese (Traditional)", "zh-TW"},
	{"Croatian", "hr"},
	{"Czech", "cs"},
	{"Danish", "da"},
	{"Dutch", "nl"},
	{"English", "en"},
	{"Estonian", "et"},
	{"Finnish", "fi"},
	{"French", "fr"},
	{"German", "de"},
	{"Greek", "el"},
	{"Hebrew", "he"},
	{"Hindi", "hi"},
	{"Hungarian", "hu"},
	{"Indonesian", "id"},
	{"Italian", "it"},
	{"Japanese", "ja"},
	{"Korean", "ko"},
	{"Latvian", "lv"},
	{"Lithuanian", "lt"},
	{"Norwegian", "no"},
	{"Persian", "fa"},
	{"Polish", "pl"},
	{"Portuguese", "pt"},
	{"Romanian", "ro"},
	{"Russian", "ru"},
	{"Serbian", "sr"},
	{"Slovak", "sk"},
	{"Slovenian", "sl"},
	{"Spanish", "es"},
	{"Swedish", "sv"},
	{"Thai", "th"},
	{"Turkish", "tr"},
	{"Ukrainian", "uk"},
	{"Vietnamese", "vi"},
};

static const std::vector<STranslateLanguage> s_vLibretranslateLanguages = {
	{"Arabic", "ar"},
	{"Chinese (Simplified)", "zh"},
	{"Czech", "cs"},
	{"Dutch", "nl"},
	{"English", "en"},
	{"Finnish", "fi"},
	{"French", "fr"},
	{"German", "de"},
	{"Greek", "el"},
	{"Hebrew", "he"},
	{"Hindi", "hi"},
	{"Hungarian", "hu"},
	{"Indonesian", "id"},
	{"Italian", "it"},
	{"Japanese", "ja"},
	{"Korean", "ko"},
	{"Polish", "pl"},
	{"Portuguese", "pt"},
	{"Russian", "ru"},
	{"Spanish", "es"},
	{"Swedish", "sv"},
	{"Turkish", "tr"},
	{"Ukrainian", "uk"},
	{"Vietnamese", "vi"},
};

// Tencent TMT / Baidu / DeepL / MyMemory support fewer targets than
// Google; these are the commonly used ones each actually accepts.
static const std::vector<STranslateLanguage> s_vTencentLanguages = {
	{"Arabic", "ar"},
	{"Chinese (Simplified)", "zh"},
	{"Chinese (Traditional)", "zh-TW"},
	{"English", "en"},
	{"French", "fr"},
	{"German", "de"},
	{"Hindi", "hi"},
	{"Indonesian", "id"},
	{"Italian", "it"},
	{"Japanese", "ja"},
	{"Korean", "ko"},
	{"Portuguese", "pt"},
	{"Russian", "ru"},
	{"Spanish", "es"},
	{"Thai", "th"},
	{"Turkish", "tr"},
	{"Vietnamese", "vi"},
};

static const std::vector<STranslateLanguage> s_vBaiduLanguages = {
	{"Arabic", "ar"},
	{"Bulgarian", "bg"},
	{"Chinese (Simplified)", "zh"},
	{"Chinese (Traditional)", "zh-TW"},
	{"Czech", "cs"},
	{"Danish", "da"},
	{"Dutch", "nl"},
	{"English", "en"},
	{"Estonian", "et"},
	{"Finnish", "fi"},
	{"French", "fr"},
	{"German", "de"},
	{"Greek", "el"},
	{"Hungarian", "hu"},
	{"Italian", "it"},
	{"Japanese", "ja"},
	{"Korean", "ko"},
	{"Polish", "pl"},
	{"Portuguese", "pt"},
	{"Romanian", "ro"},
	{"Russian", "ru"},
	{"Slovenian", "sl"},
	{"Spanish", "es"},
	{"Swedish", "sv"},
	{"Thai", "th"},
	{"Ukrainian", "uk"},
	{"Vietnamese", "vi"},
};

static const std::vector<STranslateLanguage> s_vDeeplLanguages = {
	{"Arabic", "ar"},
	{"Bulgarian", "bg"},
	{"Chinese (Simplified)", "zh"},
	{"Chinese (Traditional)", "zh-TW"},
	{"Czech", "cs"},
	{"Danish", "da"},
	{"Dutch", "nl"},
	{"English", "en"},
	{"Estonian", "et"},
	{"Finnish", "fi"},
	{"French", "fr"},
	{"German", "de"},
	{"Greek", "el"},
	{"Hungarian", "hu"},
	{"Indonesian", "id"},
	{"Italian", "it"},
	{"Japanese", "ja"},
	{"Korean", "ko"},
	{"Latvian", "lv"},
	{"Lithuanian", "lt"},
	{"Norwegian", "nb"},
	{"Polish", "pl"},
	{"Portuguese", "pt"},
	{"Romanian", "ro"},
	{"Russian", "ru"},
	{"Slovak", "sk"},
	{"Slovenian", "sl"},
	{"Spanish", "es"},
	{"Swedish", "sv"},
	{"Turkish", "tr"},
	{"Ukrainian", "uk"},
};

const std::array<STranslateBackendInfo, 8> g_aTranslateBackends = {{
	{"google", "Google", s_vGoogleLanguages, false},
	{"googlealt", "Google (备用端点)", s_vGoogleLanguages, true},
	{"libretranslate", "LibreTranslate", s_vLibretranslateLanguages, true},
	{"tencent", "腾讯云", s_vTencentLanguages, false, {
									  {"SecretId:", g_Config.m_TcTranslateTencentSecretId, sizeof(g_Config.m_TcTranslateTencentSecretId), "", false},
									  {"SecretKey:", g_Config.m_TcTranslateTencentSecretKey, sizeof(g_Config.m_TcTranslateTencentSecretKey), "", true},
									  {"地域:", g_Config.m_TcTranslateTencentRegion, sizeof(g_Config.m_TcTranslateTencentRegion), "ap-guangzhou", false},
								  }},
	{"baidu", "百度", s_vBaiduLanguages, false, {
							    {"APP ID:", g_Config.m_TcTranslateBaiduAppId, sizeof(g_Config.m_TcTranslateBaiduAppId), "", false},
							    {"密钥:", g_Config.m_TcTranslateBaiduKey, sizeof(g_Config.m_TcTranslateBaiduKey), "", true},
						    }},
	{"deepl", "DeepL", s_vDeeplLanguages, false, {
							     {"DeepL API 密钥:", g_Config.m_TcTranslateDeeplKey, sizeof(g_Config.m_TcTranslateDeeplKey), "免费版以 :fx 结尾", true},
						     }},
	{"mymemory", "MyMemory", s_vGoogleLanguages, false},
	{"llm", "LLM (OpenAI 兼容)", s_vGoogleLanguages, false, {
									 {"LLM 端点:", g_Config.m_TcTranslateLlmEndpoint, sizeof(g_Config.m_TcTranslateLlmEndpoint), "https://api.openai.com/v1", false},
									 {"LLM API 密钥:", g_Config.m_TcTranslateLlmKey, sizeof(g_Config.m_TcTranslateLlmKey), "本地端点可留空", true},
									 {"模型:", g_Config.m_TcTranslateLlmModel, sizeof(g_Config.m_TcTranslateLlmModel), "gpt-4o-mini", false},
								 }},
}};

int TranslateLanguageIndex(const std::vector<STranslateLanguage> &vLanguages, const char *pCode)
{
	for(size_t i = 0; i < vLanguages.size(); i++)
		if(str_comp_nocase(pCode, vLanguages[i].m_pCode) == 0)
			return (int)i;
	for(size_t i = 0; i < vLanguages.size(); i++)
		if(str_comp_nocase("en", vLanguages[i].m_pCode) == 0)
			return (int)i;
	return 0;
}

// 需要密钥的后端在没配密钥时无法工作。用户选到这类后端又没填 key 时，
// 与其每次翻译都失败（此前只有一句 Configure the ... 错误），不如自动回退到
// 免密钥的 Google。返回 true 表示发生了回退，调用方据此提示一次。
bool TranslateBackendNeedsMissingCredentials(const char *pBackend)
{
	if(str_comp_nocase(pBackend, "deepl") == 0)
		return g_Config.m_TcTranslateDeeplKey[0] == '\0';
	if(str_comp_nocase(pBackend, "baidu") == 0)
		return g_Config.m_TcTranslateBaiduAppId[0] == '\0' || g_Config.m_TcTranslateBaiduKey[0] == '\0';
	if(str_comp_nocase(pBackend, "tencent") == 0)
		return g_Config.m_TcTranslateTencentSecretId[0] == '\0' || g_Config.m_TcTranslateTencentSecretKey[0] == '\0';
	if(str_comp_nocase(pBackend, "llm") == 0)
	{
		// LLM 端点留空会默认打到 api.openai.com，没有 key 一样失败；只有显式
		// 指向本机端点时才允许无 key。
		const char *pEndpoint = g_Config.m_TcTranslateLlmEndpoint;
		if(pEndpoint[0] == '\0')
			return true;
		const bool Local = str_find_nocase(pEndpoint, "://localhost") || str_find_nocase(pEndpoint, "://127.0.0.1");
		return g_Config.m_TcTranslateLlmKey[0] == '\0' && !Local;
	}
	return false;
}

static std::unique_ptr<ITranslateBackend> CreateTranslateBackend(IHttp &Http, const char *pText, const char *pTargetLang)
{
	// 缺密钥 → 回退 Google（免密钥），避免整个功能不可用。
	if(TranslateBackendNeedsMissingCredentials(g_Config.m_TcTranslateBackend))
		return std::make_unique<CTranslateBackendGoogle>(Http, pText, pTargetLang, "Google");

	for(const STranslateBackendInfo &Backend : g_aTranslateBackends)
	{
		if(str_comp_nocase(g_Config.m_TcTranslateBackend, Backend.m_pValue) != 0)
			continue;
		if(str_comp_nocase(Backend.m_pValue, "libretranslate") == 0)
			return std::make_unique<CTranslateBackendLibretranslate>(Http, pText, pTargetLang, Backend.m_pName);
		if(str_comp_nocase(Backend.m_pValue, "google") == 0)
			return std::make_unique<CTranslateBackendGoogle>(Http, pText, pTargetLang, Backend.m_pName);
		if(str_comp_nocase(Backend.m_pValue, "googlealt") == 0)
			return std::make_unique<CTranslateBackendGoogleAlt>(Http, pText, pTargetLang, Backend.m_pName);
		if(str_comp_nocase(Backend.m_pValue, "tencent") == 0)
			return std::make_unique<CTranslateBackendTencent>(Http, pText, pTargetLang, Backend.m_pName);
		if(str_comp_nocase(Backend.m_pValue, "baidu") == 0)
			return std::make_unique<CTranslateBackendBaidu>(Http, pText, pTargetLang, Backend.m_pName);
		if(str_comp_nocase(Backend.m_pValue, "deepl") == 0)
			return std::make_unique<CTranslateBackendDeepl>(Http, pText, pTargetLang, Backend.m_pName);
		if(str_comp_nocase(Backend.m_pValue, "mymemory") == 0)
			return std::make_unique<CTranslateBackendMymemory>(Http, pText, pTargetLang, Backend.m_pName);
		if(str_comp_nocase(Backend.m_pValue, "llm") == 0)
			return std::make_unique<CTranslateBackendLlm>(Http, pText, pTargetLang, Backend.m_pName);
	}
	return nullptr;
}

void CTranslate::ConTranslate(IConsole::IResult *pResult, void *pUserData)
{
	const char *pName;
	if(pResult->NumArguments() == 0)
		pName = nullptr;
	else
		pName = pResult->GetString(0);

	CTranslate *pThis = static_cast<CTranslate *>(pUserData);
	pThis->Translate(pName);
}

void CTranslate::ConTranslateId(IConsole::IResult *pResult, void *pUserData)
{
	CTranslate *pThis = static_cast<CTranslate *>(pUserData);
	pThis->Translate(pResult->GetInteger(0));
}

static bool IsMostlyChineseOrNoText(const char *pText);

void CTranslate::ConTranslateRecent(IConsole::IResult *pResult, void *pUserData)
{
	CTranslate *pThis = static_cast<CTranslate *>(pUserData);
	if(pResult->NumArguments() > 0)
	{
		const int Count = std::clamp(pResult->GetInteger(0), 1, 16);
		const int Old = g_Config.m_TcTranslateRecentCount;
		g_Config.m_TcTranslateRecentCount = Count;
		pThis->TranslateRecent();
		g_Config.m_TcTranslateRecentCount = Old;
		return;
	}
	pThis->TranslateRecent();
}

void CTranslate::TranslateRecent()
{
	// 从最新一条往回扫，挑出非中文、非系统、非自己的消息，最多翻
	// g_Config.m_TcTranslateRecentCount 条。已经翻过（有 response）的跳过，
	// 避免按一次键把同一批重复排队。
	const int MaxCount = std::clamp(g_Config.m_TcTranslateRecentCount, 1, 16);
	int Queued = 0;
	for(int i = 0; i < CChat::MAX_LINES && Queued < MaxCount; i++)
	{
		CChat::CLine *pLine = &GameClient()->m_Chat.m_aLines[((GameClient()->m_Chat.m_CurrentLine - i) + CChat::MAX_LINES) % CChat::MAX_LINES];
		if(!pLine->m_Initialized || pLine->m_aText[0] == '\0')
			continue;
		if(pLine->m_pTranslateResponse != nullptr)
			continue;
		if(pLine->m_ClientId == CChat::CLIENT_MSG || pLine->m_ClientId == CChat::SERVER_MSG)
			continue;
		bool Local = false;
		for(const int Id : GameClient()->m_aLocalIds)
			if(Id >= 0 && Id == pLine->m_ClientId)
				Local = true;
		if(Local)
			continue;
		if(IsMostlyChineseOrNoText(pLine->m_aText))
			continue;
		Translate(*pLine, true);
		Queued++;
	}
	if(Queued == 0)
		GameClient()->m_Chat.Echo("No untranslated non-Chinese messages found");
}

void CTranslate::OnConsoleInit()
{
	Console()->Register("translate", "?r[name]", CFGFLAG_CLIENT, ConTranslate, this, "Translate last message (of a given name)");
	Console()->Register("translate_id", "v[id]", CFGFLAG_CLIENT, ConTranslateId, this, "Translate last message of the person with this id");
	Console()->Register("tc_translate_recent", "?i[count]", CFGFLAG_CLIENT, ConTranslateRecent, this, "Translate the most recent non-Chinese chat lines");
}

void CTranslate::Translate(int Id, bool ShowProgress)
{
	if(Id < 0 || Id > (int)std::size(GameClient()->m_aClients))
	{
		GameClient()->m_Chat.Echo("Not a valid ID");
		return;
	}
	const auto &Player = GameClient()->m_aClients[Id];
	if(!Player.m_Active)
	{
		GameClient()->m_Chat.Echo("ID not connected");
		return;
	}
	Translate(Player.m_aName, ShowProgress);
}

void CTranslate::Translate(const char *pName, bool ShowProgress)
{
	CChat::CLine *pLineBest = nullptr;
	if(GameClient()->m_Chat.m_CurrentLine > 0)
	{
		int ScoreBest = -1;
		for(int i = 0; i < CChat::MAX_LINES; i++)
		{
			CChat::CLine *pLine = &GameClient()->m_Chat.m_aLines[((GameClient()->m_Chat.m_CurrentLine - i) + CChat::MAX_LINES) % CChat::MAX_LINES];
			if(pLine->m_pTranslateResponse != nullptr)
				continue;
			if(pLine->m_ClientId == CChat::CLIENT_MSG)
				continue;
			for(int Id : GameClient()->m_aLocalIds)
				if(pLine->m_ClientId == Id)
					continue;
			int Score = 0;
			if(pName)
			{
				if(pLine->m_ClientId == CChat::SERVER_MSG)
					continue;
				if(str_comp(pLine->m_aName, pName) == 0)
					Score = 2;
				else if(str_comp_nocase(pLine->m_aName, pName) == 0)
					Score = 1;
				else
					continue;
			}
			if(Score > ScoreBest)
			{
				ScoreBest = Score;
				pLineBest = pLine;
			}
		}
	}
	if(!pLineBest || pLineBest->m_aText[0] == '\0')
	{
		GameClient()->m_Chat.Echo("No message to translate");
		return;
	}

	Translate(*pLineBest, ShowProgress);
}

void CTranslate::Translate(CChat::CLine &Line, bool ShowProgress)
{
	if(m_vJobs.size() > 15)
	{
		return;
	}

	CTranslateJob Job;
	Job.m_pLine = &Line;
	Job.m_pTranslateResponse = std::make_shared<CTranslateResponse>();
	Job.m_pLine->m_pTranslateResponse = Job.m_pTranslateResponse;

	Job.m_pBackend = CreateTranslateBackend(*Http(), Job.m_pLine->m_aText, g_Config.m_TcTranslateTarget);
	if(!Job.m_pBackend)
	{
		GameClient()->m_Chat.Echo("Invalid translate backend");
		return;
	}

	if(ShowProgress)
	{
		str_format(Job.m_pTranslateResponse->m_Text, sizeof(Job.m_pTranslateResponse->m_Text), TCLocalize("%s translating to %s", "translate"), Job.m_pBackend->Name(), g_Config.m_TcTranslateTarget);
		Job.m_pLine->m_Time = time();
	}
	else
	{
		Job.m_pTranslateResponse->m_Text[0] = '\0';
	}

	m_vJobs.emplace_back(std::move(Job));

	if(ShowProgress)
		GameClient()->m_Chat.RebuildChat();
}

void CTranslate::OnRender()
{
	const auto Time = time();
	auto ForEach = [&](CTranslateJob &Job) {
		if(Job.m_IsOutgoing)
		{
			const std::optional<bool> Done = Job.m_pBackend->Update(*Job.m_pTranslateResponse);
			if(!Done.has_value())
				return false; // Keep ongoing tasks
			if(*Done && Job.m_pTranslateResponse->m_Text[0] != '\0')
				GameClient()->m_Chat.SendChat(Job.m_OutgoingTeam, Job.m_pTranslateResponse->m_Text);
			else
			{
				// Translation failed (or came back empty): don't swallow the message, send it untranslated
				char aBuf[sizeof(Job.m_pTranslateResponse->m_Text) + 64];
				str_format(aBuf, sizeof(aBuf), TCLocalize("Translating your message failed, sending it untranslated: %s", "translate"), Job.m_pTranslateResponse->m_Text);
				GameClient()->m_Chat.Echo(aBuf);
				GameClient()->m_Chat.SendChat(Job.m_OutgoingTeam, Job.m_aOutgoingOriginalText);
			}
			return true;
		}
		if(Job.m_pLine->m_pTranslateResponse != Job.m_pTranslateResponse)
			return true; // Not the same line anymore
		const std::optional<bool> Done = Job.m_pBackend->Update(*Job.m_pTranslateResponse);
		if(!Done.has_value())
			return false; // Keep ongoing tasks
		if(*Done)
		{
			if(str_comp_nocase(Job.m_pLine->m_aText, Job.m_pTranslateResponse->m_Text) == 0) // Check for no translation difference
				Job.m_pTranslateResponse->m_Text[0] = '\0';
		}
		else
		{
			char aBuf[sizeof(Job.m_pTranslateResponse->m_Text)];
			str_format(aBuf, sizeof(aBuf), TCLocalize("%s to %s failed: %s", "translate"), Job.m_pBackend->Name(), g_Config.m_TcTranslateTarget, Job.m_pTranslateResponse->m_Text);
			Job.m_pTranslateResponse->m_Error = true;
			str_copy(Job.m_pTranslateResponse->m_Text, aBuf);
		}
		Job.m_pLine->m_Time = Time;
		GameClient()->m_Chat.RebuildChat();
		return true;
	};
	m_vJobs.erase(std::remove_if(m_vJobs.begin(), m_vJobs.end(), ForEach), m_vJobs.end());
}

// 汉字占「汉字+字母」的一半以上算中文；没有任何文字（纯表情/数字/符号）也视为不需翻译
static bool IsMostlyChineseOrNoText(const char *pText)
{
	int Han = 0, Letters = 0;
	const char *p = pText;
	int Code;
	while((Code = str_utf8_decode(&p)) > 0)
	{
		if((Code >= 0x4E00 && Code <= 0x9FFF) || (Code >= 0x3400 && Code <= 0x4DBF) || (Code >= 0xF900 && Code <= 0xFAFF) || (Code >= 0x20000 && Code <= 0x2FA1F))
			Han++;
		else if((Code >= 'a' && Code <= 'z') || (Code >= 'A' && Code <= 'Z') || (Code >= 0xC0 && Code <= 0x24F) || (Code >= 0x370 && Code <= 0x52F) || (Code >= 0x3040 && Code <= 0x30FF) || (Code >= 0xAC00 && Code <= 0xD7AF) || (Code >= 0x0600 && Code <= 0x06FF) || (Code >= 0x0E00 && Code <= 0x0E7F))
			Letters++;
	}
	if(Han + Letters == 0)
		return true;
	return Han * 2 >= Han + Letters;
}

void CTranslate::AutoTranslate(CChat::CLine &Line)
{
	if(!g_Config.m_TcTranslateAuto)
		return;
	// 只翻玩家发言：系统/服务器消息（ClientId<0）不处理
	if(Line.m_ClientId < 0)
		return;
	if(IsMostlyChineseOrNoText(Line.m_aText))
		return;
	for(const int Id : GameClient()->m_aLocalIds)
	{
		if(Id >= 0 && Id == Line.m_ClientId)
			return;
	}
	Translate(Line, false);
}

bool CTranslate::ChatDoTranslateOutgoing(int Team, const char *pText)
{
	// Don't translate server commands (e.g. "/w name msg"), translating would break their syntax
	if(!g_Config.m_TcTranslateOutgoing || pText[0] == '\0' || pText[0] == '/')
		return false;

	if(m_vJobs.size() > 15)
	{
		// Too many jobs in flight: don't queue another, let the caller send this one untranslated
		return false;
	}

	CTranslateJob Job;
	Job.m_IsOutgoing = true;
	Job.m_OutgoingTeam = Team;
	str_copy(Job.m_aOutgoingOriginalText, pText);
	Job.m_pTranslateResponse = std::make_shared<CTranslateResponse>();

	Job.m_pBackend = CreateTranslateBackend(*Http(), pText, g_Config.m_TcTranslateOutgoingTarget);
	if(!Job.m_pBackend)
	{
		GameClient()->m_Chat.Echo("Invalid translate backend");
		return false;
	}

	m_vJobs.emplace_back(std::move(Job));
	return true;
}
