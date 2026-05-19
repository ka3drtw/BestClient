#include "reshade_runtime.h"

#include <base/math.h>
#include <base/system.h>

#include <engine/storage.h>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace
{
bool BestClientReadBinaryTextFile(IStorage *pStorage, const char *pFilename, std::string &Text)
{
	char aPath[IO_MAX_PATH_LENGTH];
	pStorage->GetBinaryPath(pFilename, aPath, sizeof(aPath));

	char *pFileText = pStorage->ReadFileStr(aPath, IStorage::TYPE_ABSOLUTE);
	if(pFileText == nullptr)
		return false;

	Text = pFileText;
	free(pFileText);
	return true;
}
}

float BestClientReShadeDeepFryQualityValue(int QualityPercent)
{
	const float ClampedPercent = std::clamp((float)QualityPercent, 0.0f, 100.0f);
	return mix(0.5f, 10.0f, ClampedPercent / 100.0f);
}

float BestClientReShadeDeepFryRedsValue(int RedsPercent)
{
	return std::clamp((float)RedsPercent / 100.0f, 0.0f, 1.0f);
}

bool BestClientReShadeRuntimeApplyDeepFry(IStorage *pStorage, bool Enabled, int QualityPercent, int RedsPercent, char *pError, int ErrorSize)
{
	(void)pStorage;
	(void)Enabled;
	(void)QualityPercent;
	(void)RedsPercent;
	if(pError != nullptr && ErrorSize > 0)
		pError[0] = '\0';
	return false;
}

bool BestClientReShadeRuntimeCommitDeepFry(IStorage *pStorage, bool Enabled, int QualityPercent, int RedsPercent, char *pError, int ErrorSize)
{
	(void)Enabled;
	(void)QualityPercent;
	(void)RedsPercent;

	if(pError != nullptr && ErrorSize > 0)
		pError[0] = '\0';

	if(pStorage == nullptr)
	{
		str_copy(pError, "Storage interface is unavailable.", ErrorSize);
		return false;
	}

	std::string ReShadeLogText;
	if(!BestClientReadBinaryTextFile(pStorage, "ReShade.log", ReShadeLogText))
	{
		str_copy(pError, "ReShade.log is not available yet.", ErrorSize);
		return false;
	}

	if(str_find_nocase(ReShadeLogText.c_str(), "Skipped loading add-on from 'E:\\BestClientAlpha\\build\\bestclient_reshade_live.addon' because this build of ReShade has only limited add-on functionality.") != nullptr ||
		str_find_nocase(ReShadeLogText.c_str(), "limited add-on functionality") != nullptr)
	{
		str_copy(pError, "The current session is still running with the old limited vulkan-1.dll. Close DDNet so the new ReShade runtime can be copied in.", ErrorSize);
		return false;
	}

	if(str_find_nocase(ReShadeLogText.c_str(), "[BestClient/ReShadeAddon] Add-on initialized.") != nullptr)
		return true;

	if(str_find_nocase(ReShadeLogText.c_str(), "Failed to load add-on") != nullptr ||
		str_find_nocase(ReShadeLogText.c_str(), "Failed to register an event") != nullptr)
	{
		str_copy(pError, "ReShade found the add-on, but it failed to initialize.", ErrorSize);
		return false;
	}

	str_copy(pError, "ReShade add-on has not initialized in this session yet.", ErrorSize);
	return false;
}
