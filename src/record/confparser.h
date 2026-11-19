#pragma once

#ifdef __cplusplus
extern "C"{
#endif /* End of #ifdef __cplusplus */

#include "record.h"

void conf_loadViParams(char* confFile, ViParams_t* viPara);
void conf_saveViParams(char* confFile, ViParams_t* viPara);

void conf_loadVencParams(char* confFile, VencParams_t* vePara);
void conf_saveVencParams(char* confFile, VencParams_t* vePara);

void conf_loadVencParamsForLive(char* confFile, VencParams_t* vePara);

void conf_loadAiParams(char* confFile, AiParams_t* aiPara);
void conf_saveAiParams(char* confFile, AiParams_t* aiPara);

void conf_loadAencParams(char* confFile, AencParams_t* aePara);
void conf_saveAencParams(char* confFile, AencParams_t* aePara);

void conf_loadRecordParams(char* confFile, RecordParams_t* recPara);
void conf_saveRecordParams(char* confFile, RecordParams_t* recPara);

void rec_dbg_log(const char* fmt, ...);

/* live stream audio (RTSP AAC track): [live] audio, default off */
bool conf_loadLiveAudio(char* confFile);
/* [live] audio_offset_ms: added to the stream's audio timestamps (positive: audio later), -500..500, default 0 */
int  conf_loadLiveAudioOffset(char* confFile);

#ifdef __cplusplus
}
#endif /* End of #ifdef __cplusplus */
