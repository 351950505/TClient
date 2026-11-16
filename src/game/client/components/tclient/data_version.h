#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_DATA_VERSION_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_DATA_VERSION_H

#include <base/io.h>
#include <base/str.h>

// Validates data/tclient/data_version.txt against the running executable so a
// half-updated install (new DDNet.exe over an old data/tclient) warns instead
// of failing in obscure ways later on. The packaging writes the file on every
// release and it travels inside data/tclient with every update package.
//
// Only the TClient domain is checked: upstream DDNet data files do not travel
// with our update packages, so a DDNet line would false-positive on every
// installing client that updated in place.

#include <engine/shared/linereader.h>

#include <game/localization.h>
#include <game/version.h>

#define DATA_VERSION_PATH "tclient/data_version.txt"

inline void CheckDataVersion(char *pError, int Length, IOHANDLE File)
{
	if(!File)
	{
		str_format(pError, Length, TCLocalize("%s not found", DATA_VERSION_PATH), DATA_VERSION_PATH);
		return;
	}

	CLineReader LineReader;
	LineReader.OpenFile(File);

	bool FoundTClient = false;
	bool VersionOk = false;
	const char *pLine;
	while((pLine = LineReader.Get()))
	{
		char aDomain[16];
		const char *pRest = str_next_token(pLine, " ", aDomain, sizeof(aDomain));
		if(!pRest || str_comp(aDomain, "TClient") != 0)
			continue;
		char aVersion[24];
		str_next_token(pRest, " ", aVersion, sizeof(aVersion));
		FoundTClient = true;
		VersionOk = aVersion[0] != 0 && str_comp(aVersion, TCLIENT_APP_VERSION) == 0;
		break;
	}
	io_close(File);

	if(!FoundTClient)
	{
		str_format(pError, Length, TCLocalize("%s not found", DATA_VERSION_PATH), DATA_VERSION_PATH);
		return;
	}
	if(!VersionOk)
		str_format(pError, Length, TCLocalize("%s version mismatch", DATA_VERSION_PATH), DATA_VERSION_PATH);
}

#endif
