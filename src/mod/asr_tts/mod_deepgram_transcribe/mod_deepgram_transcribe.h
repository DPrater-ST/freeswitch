#ifndef __MOD_DEEPGRAM_TRANSCRIBE_H__
#define __MOD_DEEPGRAM_TRANSCRIBE_H__

/*
 * A FreeSWITCH media-bug module streaming a call leg's audio to
 * rtc-transcription's self-hosted-Deepgram /ws/transcribe endpoint (an
 * internal ServiceTitan wrapper around Deepgram, not Deepgram's own hosted
 * API) — so this speaks rtc-transcription's own small control protocol
 * (rtc-transcription/CLAUDE.md "Protocol"), not Deepgram's public streaming
 * API. Struct/naming conventions follow deepgram/freeswitch_modules'
 * mod_deepgram_transcribe (https://github.com/deepgram/freeswitch_modules)
 * for consistency with that reference, where the differing target protocol
 * didn't force a difference.
 */

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_BUG_NAME "deepgram_transcribe"
#define TRANSCRIBE_EVENT_RESULTS "deepgram_transcribe::transcription"
#define TRANSCRIBE_EVENT_CONNECT_SUCCESS "deepgram_transcribe::connect"
#define TRANSCRIBE_EVENT_CONNECT_FAIL "deepgram_transcribe::connect_failed"
#define TRANSCRIBE_EVENT_DISCONNECT "deepgram_transcribe::disconnect"

#define MAX_SESSION_ID (256)
#define MAX_HOST_LEN (256)
#define MAX_BUG_LEN (64)

/* Channel variables — a much smaller surface than the Deepgram-direct-API
 * reference module's (no DEEPGRAM_API_KEY/tier/model/etc — rtc-transcription
 * owns those choices server-side). Only the connection target is
 * configurable per-channel. */
#define VAR_DEEPGRAM_TRANSCRIPTION_HOST "DEEPGRAM_TRANSCRIPTION_HOST"
#define VAR_DEEPGRAM_TRANSCRIPTION_PORT "DEEPGRAM_TRANSCRIPTION_PORT"

typedef void (*responseHandler_t)(
    switch_core_session_t *session,
    const char *eventName,
    const char *json,
    const char *bugname,
    int finished
);

struct private_data
{
    switch_mutex_t *mutex;
    char sessionId[MAX_SESSION_ID + 1];
    char bugname[MAX_BUG_LEN + 1];
    uint32_t channels;
    SpeexResamplerState *resampler;
    void *pAudioPipe;
    responseHandler_t responseHandler;
};

typedef struct private_data private_t;

#endif
