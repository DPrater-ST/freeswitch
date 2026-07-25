#include <cstdlib>
#include <cstring>

#include <switch.h>
#include <switch_json.h>

#include <g711.h>

#include <memory>
#include <string>
#include <thread>

#include "mod_deepgram_transcribe.h"
#include "simple_buffer.h"
#include "ws_client.h"

#define CHUNKSIZE_PCM (320) /* 20ms @ 8kHz 16-bit mono, post-resample */
#define CHUNKSIZE_ULAW (CHUNKSIZE_PCM / 2)
#define CONNECT_TIMEOUT_MS (3000)
#define READ_LOOP_TIMEOUT_MS (5000)
#define DEEPGRAM_WS_PATH "/ws/transcribe"

class DeepgramStreamer
{
  public:
    DeepgramStreamer(
        const char *sessionId,
        const char *bugname,
        const char *host,
        int port,
        responseHandler_t responseHandler
    )
        : m_sessionId(sessionId), m_bugname(bugname), m_host(host), m_port(port), m_responseHandler(responseHandler),
          m_finished(false), m_connected(false), m_ws(nullptr), m_audioBuffer(CHUNKSIZE_ULAW, 15)
    {
    }

    ~DeepgramStreamer()
    {
    }

    /* Spawns the single background thread that owns the WebSocket for the
     * life of this streamer: connect + handshake + "start" + flush any
     * buffered audio + read loop until closed. */
    void connect()
    {
        m_thread = std::thread([this] { runConnection(); });
    }

    bool write(void *data, uint32_t datalen)
    {
        if (m_finished)
            return false;

        uint32_t nSamples = datalen / 2;
        if (datalen == 0 || nSamples > SWITCH_RECOMMENDED_BUFFER_SIZE)
        {
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_WARNING,
                "DeepgramStreamer::write got unexpected datalen %u, dropping\n",
                datalen
            );
            return false;
        }

        uint8_t encoded[SWITCH_RECOMMENDED_BUFFER_SIZE];
        int16_t *samples = static_cast<int16_t *>(data);
        for (uint32_t i = 0; i < nSamples; i++)
        {
            encoded[i] = linear_to_ulaw(samples[i]);
        }

        /* Not connected yet: buffer the already mu-law-encoded bytes (what
         * actually goes over the wire) so the flush in runConnection() is a
         * plain send with no re-encoding. SimpleBuffer.add() silently no-ops
         * on a non-chunk-sized frame — only the pre-connect buffering path
         * needs exact 20ms chunks; a connected direct send below has no
         * such restriction. */
        if (!m_connected)
        {
            if (datalen % CHUNKSIZE_PCM == 0)
                m_audioBuffer.add(encoded, nSamples);
            return true;
        }

        if (m_ws && ws_client_send(m_ws, WS_FRAME_BINARY, encoded, nSamples) != WS_OK)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "DeepgramStreamer::write send failed\n");
        }
        return true;
    }

    void finish()
    {
        if (m_finished)
            return;
        m_finished = true;

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "DeepgramStreamer::finish (%p)\n", this);

        if (m_ws)
        {
            static const char *end_msg = "{\"command\":\"end\"}";
            ws_client_send(m_ws, WS_FRAME_TEXT, end_msg, strlen(end_msg));
        }

        if (m_thread.joinable())
            m_thread.join();

        if (m_ws)
        {
            ws_client_destroy(m_ws);
            m_ws = nullptr;
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "DeepgramStreamer::finish complete (%p)\n", this);
    }

  private:
    void fireSimple(const char *eventName)
    {
        switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());
        if (!psession)
            return;
        m_responseHandler(psession, eventName, NULL, m_bugname.c_str(), m_finished);
        switch_core_session_rwunlock(psession);
    }

    void fireConnectFail(const char *detail)
    {
        switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());
        if (!psession)
            return;
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "type", "error");
        cJSON_AddStringToObject(json, "error", detail);
        char *jsonString = cJSON_PrintUnformatted(json);
        m_responseHandler(psession, TRANSCRIBE_EVENT_CONNECT_FAIL, jsonString, m_bugname.c_str(), m_finished);
        free(jsonString);
        cJSON_Delete(json);
        switch_core_session_rwunlock(psession);
    }

    void flushBuffered()
    {
        int nItems = m_audioBuffer.getNumItems();
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_DEBUG,
            "DeepgramStreamer %p connected, %d buffered chunks to flush\n",
            this,
            nItems
        );
        char *p;
        while ((p = m_audioBuffer.getNextChunk()) != nullptr)
        {
            ws_client_send(m_ws, WS_FRAME_BINARY, p, CHUNKSIZE_ULAW);
        }
    }

    void runConnection()
    {
        ws_client_t *ws = ws_client_connect(m_host.c_str(), m_port, DEEPGRAM_WS_PATH, CONNECT_TIMEOUT_MS);
        if (!ws)
        {
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_ERROR,
                "DeepgramStreamer: failed to connect/handshake to %s:%d%s\n",
                m_host.c_str(),
                m_port,
                DEEPGRAM_WS_PATH
            );
            fireConnectFail("connect failed");
            return;
        }

        static const char *start_msg = "{\"command\":\"start\",\"arguments\":{}}";
        if (ws_client_send(ws, WS_FRAME_TEXT, start_msg, strlen(start_msg)) != WS_OK)
        {
            fireConnectFail("failed to send start command");
            ws_client_destroy(ws);
            return;
        }

        ws_frame_type_t type;
        uint8_t *data;
        size_t len;
        if (ws_client_recv(ws, CONNECT_TIMEOUT_MS, &type, &data, &len) != WS_OK)
        {
            fireConnectFail("no start response from rtc-transcription");
            ws_client_destroy(ws);
            return;
        }
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_DEBUG,
            "DeepgramStreamer: start response: %s\n",
            (char *)data
        );
        free(data);

        m_ws = ws;
        m_connected = true;
        flushBuffered();
        fireSimple(TRANSCRIBE_EVENT_CONNECT_SUCCESS);

        while (true)
        {
            ws_status_t st = ws_client_recv(ws, READ_LOOP_TIMEOUT_MS, &type, &data, &len);
            if (st == WS_OK)
            {
                if (type == WS_FRAME_TEXT)
                {
                    switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());
                    if (psession)
                    {
                        m_responseHandler(
                            psession,
                            TRANSCRIBE_EVENT_RESULTS,
                            (char *)data,
                            m_bugname.c_str(),
                            m_finished
                        );
                        switch_core_session_rwunlock(psession);
                    }
                }
                free(data);
                continue;
            }

            if (st == WS_ERR_CLOSED)
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "DeepgramStreamer: server closed cleanly\n");
                break;
            }

            if (m_finished)
                break;

            /* transient read error/timeout while still active: log and keep waiting */
            switch_log_printf(
                SWITCH_CHANNEL_LOG,
                SWITCH_LOG_DEBUG,
                "DeepgramStreamer: recv status %d, still active, continuing\n",
                st
            );
        }

        fireSimple(TRANSCRIBE_EVENT_DISCONNECT);
    }

    std::string m_sessionId;
    std::string m_bugname;
    std::string m_host;
    int m_port;
    responseHandler_t m_responseHandler;
    bool m_finished;
    bool m_connected;
    ws_client_t *m_ws;
    std::thread m_thread;
    SimpleBuffer m_audioBuffer;
};

static void reaper(private_t *cb)
{
    std::shared_ptr<DeepgramStreamer> pStreamer;
    pStreamer.reset((DeepgramStreamer *)cb->pAudioPipe);
    cb->pAudioPipe = nullptr;

    std::thread t([pStreamer] { pStreamer->finish(); });
    t.detach();
}

static void killcb(private_t *cb)
{
    if (cb)
    {
        if (cb->pAudioPipe)
        {
            DeepgramStreamer *p = (DeepgramStreamer *)cb->pAudioPipe;
            delete p;
            cb->pAudioPipe = NULL;
        }
        if (cb->resampler)
        {
            speex_resampler_destroy(cb->resampler);
            cb->resampler = NULL;
        }
    }
}

extern "C"
{
    switch_status_t dg_transcribe_session_init(
        switch_core_session_t *session,
        responseHandler_t responseHandler,
        uint32_t samples_per_second,
        uint32_t channels,
        const char *host,
        int port,
        char *bugname,
        void **ppUserData
    )
    {
        DeepgramStreamer *streamer = NULL;
        switch_channel_t *channel  = switch_core_session_get_channel(session);
        switch_memory_pool_t *pool = switch_core_session_get_pool(session);
        switch_codec_t *read_codec = switch_core_session_get_read_codec(session);
        uint32_t sampleRate        = read_codec->implementation->actual_samples_per_second;
        const char *sessionId      = switch_core_session_get_uuid(session);
        private_t *cb              = (private_t *)switch_core_session_alloc(session, sizeof(*cb));

        switch_log_printf(
            SWITCH_CHANNEL_CHANNEL_LOG(channel),
            SWITCH_LOG_DEBUG,
            "%s: dg_transcribe_session_init: host=%s port=%d\n",
            switch_channel_get_name(channel),
            host,
            port
        );

        memset(cb, 0, sizeof(*cb));
        strncpy(cb->sessionId, sessionId, MAX_SESSION_ID);
        strncpy(cb->bugname, bugname, MAX_BUG_LEN);

        cb->channels         = channels;
        cb->responseHandler  = responseHandler;

        if (switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS)
        {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mutex\n");
            return SWITCH_STATUS_FALSE;
        }

        /* determine if we need to resample the audio to 8khz, the rate
         * rtc-transcription's mu-law encoding expects */
        if (sampleRate != 8000)
        {
            int speex_err;
            cb->resampler = speex_resampler_init(1, sampleRate, 8000, SWITCH_RESAMPLE_QUALITY, &speex_err);
            if (0 != speex_err)
            {
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "%s: Error initializing resampler: %s.\n",
                    switch_channel_get_name(channel),
                    speex_resampler_strerror(speex_err)
                );
                return SWITCH_STATUS_FALSE;
            }
        }

        try
        {
            streamer = new DeepgramStreamer(sessionId, bugname, host, port, responseHandler);
            cb->pAudioPipe = streamer;
            streamer->connect();
        }
        catch (std::exception &e)
        {
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_ERROR,
                "%s: Error initializing deepgram streamer: %s.\n",
                switch_channel_get_name(channel),
                e.what()
            );
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = cb;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t dg_transcribe_session_stop(switch_core_session_t *session, int channelIsClosing, char *bugname)
    {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_media_bug_t *bug   = (switch_media_bug_t *)switch_channel_get_private(channel, bugname);

        if (bug)
        {
            private_t *cb = (private_t *)switch_core_media_bug_get_user_data(bug);

            switch_mutex_lock(cb->mutex);
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "dg_transcribe_session_stop: locked session\n"
            );

            switch_channel_set_private(channel, bugname, NULL);
            if (!channelIsClosing)
                switch_core_media_bug_remove(session, &bug);

            DeepgramStreamer *streamer = (DeepgramStreamer *)cb->pAudioPipe;
            if (streamer)
                reaper(cb);
            killcb(cb);
            switch_mutex_unlock(cb->mutex);
            switch_log_printf(
                SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG,
                "dg_transcribe_session_stop: unlocked session\n"
            );

            return SWITCH_STATUS_SUCCESS;
        }

        switch_log_printf(
            SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_INFO,
            "%s Bug is not attached.\n",
            switch_channel_get_name(channel)
        );
        return SWITCH_STATUS_FALSE;
    }

    switch_bool_t dg_transcribe_frame(switch_media_bug_t *bug, void *user_data)
    {
        switch_core_session_t *session = switch_core_media_bug_get_session(bug);
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = {};
        private_t *cb         = (private_t *)user_data;

        frame.data   = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS)
        {
            DeepgramStreamer *streamer = (DeepgramStreamer *)cb->pAudioPipe;
            if (streamer)
            {
                while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS &&
                       !switch_test_flag((&frame), SFF_CNG))
                {
                    if (frame.datalen)
                    {
                        if (cb->resampler)
                        {
                            spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
                            spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
                            spx_uint32_t in_len  = frame.samples;

                            speex_resampler_process_interleaved_int(
                                cb->resampler,
                                (const spx_int16_t *)frame.data,
                                (spx_uint32_t *)&in_len,
                                &out[0],
                                &out_len
                            );
                            streamer->write(&out[0], sizeof(spx_int16_t) * out_len);
                        }
                        else
                        {
                            streamer->write(frame.data, frame.datalen);
                        }
                    }
                }
            }
            switch_mutex_unlock(cb->mutex);
        }
        return SWITCH_TRUE;
    }
}
