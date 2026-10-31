/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_COUNTRYFLAGS_H
#define GAME_CLIENT_COMPONENTS_COUNTRYFLAGS_H

#include <engine/graphics.h>
#include <engine/shared/protocol.h>

#include <game/client/component.h>

#include <cstddef>
#include <vector>

class CCountryFlags : public CComponent
{
public:
	class CCountryFlag
	{
	public:
		/**
		 * Country code in ISO 3166-1 numeric.
		 */
		int m_CountryCode;
		char m_aCountryCodeString[16];
		IGraphics::CTextureHandle m_Texture;

		bool operator<(const CCountryFlag &Other) const;
	};

	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;

	size_t Num() const;
	const CCountryFlag &GetByCountryCode(int CountryCode) const;
	const CCountryFlag &GetByIndex(size_t Index) const;
	// 主播模式兜底：国旗属于身份信息，主开关开着时所有渲染点一律不画。
	// allow_in_streamer_mode 保留为显式逃生口，但当前没有任何调用点使用——
	// 连国旗/语言选择器也只显示国家代码文字，不显示旗面。
	void Render(const CCountryFlag &Flag, ColorRGBA Color, float x, float y, float w, float h, bool AllowInStreamerMode = false);
	void Render(int CountryCode, ColorRGBA Color, float x, float y, float w, float h, bool AllowInStreamerMode = false);

private:
	std::vector<CCountryFlag> m_vCountryFlags;
	size_t m_aCountryCodeToIndexTable[CountryCode::MAXIMUM - CountryCode::MINIMUM + 1];

	int m_FlagsQuadContainerIndex;

	static bool ValidateCountryCodeString(const char *pString);
	void LoadCountryflagsIndexfile();
};
#endif
