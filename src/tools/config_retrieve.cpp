#include <base/dbg.h>
#include <base/fs.h>
#include <base/io.h>

#include <engine/shared/datafile.h>
#include <engine/shared/map_settings.h>
#include <engine/storage.h>

#include <game/mapitems.h>

static void Process(IStorage *pStorage, const char *pMapName, const char *pConfigName)
{
	CDataFileReader Reader;
	if(!Reader.Open(pStorage, pMapName, IStorage::TYPE_ABSOLUTE))
	{
		dbg_msg("config_retrieve", "error opening map '%s'", pMapName);
		return;
	}
	bool ConfigFound = false;
	int Start, Num;
	Reader.GetType(MAPITEMTYPE_INFO, &Start, &Num);
	for(int i = Start; i < Start + Num; i++)
	{
		int Id;
		CMapItemInfoSettings *pItem = (CMapItemInfoSettings *)Reader.GetItem(i, nullptr, &Id);
		int ItemSize = Reader.GetItemSize(i);
		if(!pItem || Id != 0)
			continue;

		if(ItemSize < (int)sizeof(CMapItemInfoSettings))
			break;
		if(!(pItem->m_Settings > -1))
			break;

		ConfigFound = true;
		const int Size = Reader.GetDataSize(pItem->m_Settings);
		const char *pSettings = (const char *)Reader.GetData(pItem->m_Settings);
		IOHANDLE Config = nullptr;
		bool ConfigOpenFailed = false;
		const bool ValidSettings = ForEachMapSetting(pSettings, Size, [&](const char *pCommand, size_t CommandLength) {
			if(ConfigOpenFailed)
				return;
			if(Config == nullptr)
			{
				Config = pStorage->OpenFile(pConfigName, IOFLAG_WRITE, IStorage::TYPE_ABSOLUTE);
				if(!Config)
				{
					dbg_msg("config_retrieve", "error opening config for writing '%s'", pConfigName);
					ConfigOpenFailed = true;
					return;
				}
			}
			io_write(Config, pCommand, CommandLength);
			io_write_newline(Config);
		});
		if(Config != nullptr)
			io_close(Config);
		if(!ValidSettings)
			dbg_msg("config_retrieve", "ignoring invalid map settings data in '%s'", pMapName);
		if(ConfigOpenFailed)
		{
			Reader.Close();
			return;
		}
		break;
	}
	Reader.Close();
	if(!ConfigFound)
	{
		(void)fs_remove(pConfigName);
	}
}
#include "config_common.h"
