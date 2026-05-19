#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_RESHADE_RUNTIME_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_RESHADE_RUNTIME_H

class IStorage;

float BestClientReShadeDeepFryQualityValue(int QualityPercent);
float BestClientReShadeDeepFryRedsValue(int RedsPercent);
bool BestClientReShadeRuntimeCommitDeepFry(IStorage *pStorage, bool Enabled, int QualityPercent, int RedsPercent, char *pError, int ErrorSize);

#endif
