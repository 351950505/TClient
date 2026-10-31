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
	// allow_in_streamer_mode 只给"国旗/语言选择器"用：那是用户自己挑国旗的界面，
	// 隐藏了就没法操作。其余所有渲染点在主播模式下由 Render 内部直接吞掉。
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
