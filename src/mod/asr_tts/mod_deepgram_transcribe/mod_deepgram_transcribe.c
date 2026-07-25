/*
 *
 * mod_deepgram_transcribe.c -- FreeSWITCH module streaming a call leg's audio
 * to rtc-transcription's self-hosted-Deepgram /ws/transcribe endpoint.
 *
 */
#include "mod_deepgram_transcribe.h"
#include "dg_transcribe_glue.h"

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_deepgram_transcribe_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_deepgram_transcribe_load);
SWITCH_MODULE_DEFINITION(mod_deepgram_transcribe, mod_deepgram_transcribe_load, mod_deepgram_transcribe_shutdown, NULL);

#define DEFAULT_DEEPGRAM_TRANSCRIPTION_PORT (80)

static switch_status_t do_stop(switch_core_session_t *session, char *bugname);

static void responseHandler(
    switch_core_session_t *session,
    const char *eventName,
    const char *json,
    const char *bugname,
    int finished
)
{
    switch_event_t *event;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "responseHandler %s, body %s.\n", eventName, json);

    switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
    switch_channel_event_set_data(channel, event);
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "deepgram");
    switch_event_add_header_string(
        event,
        SWITCH_STACK_BOTTOM,
        "transcription-session-finished",
        finished ? "true" : "false"
    );

    if (bugname)
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);

    if (json)
        switch_event_add_body(event, "%s", json);

    switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    switch_core_session_t *session = switch_core_media_bug_get_session(bug);

    switch (type)
    {
    case SWITCH_ABC_TYPE_INIT:
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_INIT.\n");
        break;

    case SWITCH_ABC_TYPE_CLOSE: {
        private_t *cb = (private_t *)switch_core_media_bug_get_user_data(bug);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_CLOSE.\n");

        dg_transcribe_session_stop(session, 1, cb->bugname);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
    }
    break;

    case SWITCH_ABC_TYPE_READ:

        return dg_transcribe_frame(bug, user_data);
        break;

    case SWITCH_ABC_TYPE_WRITE:
    default:
        break;
    }

    return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, char *bugname)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug;
    switch_status_t status;
    switch_codec_implementation_t read_impl = {0};
    void *pUserData;
    uint32_t samples_per_second;
    const char *host;
    const char *portVar;
    int port = DEFAULT_DEEPGRAM_TRANSCRIPTION_PORT;

    if (switch_channel_get_private(channel, bugname))
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing bug from previous transcribe\n");
        do_stop(session, bugname);
    }

    host = switch_channel_get_variable(channel, VAR_DEEPGRAM_TRANSCRIPTION_HOST);
    if (zstr(host))
    {
        switch_log_printf(
            SWITCH_CHANNEL_CHANNEL_LOG(channel),
            SWITCH_LOG_ERROR,
            "%s: variable %s not set.\n",
            switch_channel_get_name(channel),
            VAR_DEEPGRAM_TRANSCRIPTION_HOST
        );
        return SWITCH_STATUS_FALSE;
    }
    if ((portVar = switch_channel_get_variable(channel, VAR_DEEPGRAM_TRANSCRIPTION_PORT)))
    {
        port = atoi(portVar);
    }

    switch_core_session_get_read_impl(session, &read_impl);

    if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS)
    {
        return SWITCH_STATUS_FALSE;
    }

    samples_per_second =
        !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;

    if (SWITCH_STATUS_FALSE == dg_transcribe_session_init(
                                   session,
                                   responseHandler,
                                   samples_per_second,
                                   flags & SMBF_STEREO ? 2 : 1,
                                   host,
                                   port,
                                   bugname,
                                   &pUserData
                               ))
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing deepgram transcribe session.\n");
        return SWITCH_STATUS_FALSE;
    }

    if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) !=
        SWITCH_STATUS_SUCCESS)
    {
        return status;
    }

    switch_channel_set_private(channel, bugname, bug);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "added media bug for deepgram transcribe\n");

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char *bugname)
{
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug   = switch_channel_get_private(channel, bugname);

    if (bug)
    {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_INFO,
            "do_stop: Received user command command to stop transcribe.\n"
        );
        status = dg_transcribe_session_stop(session, 0, bugname);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "do_stop: stopped transcribe.\n");
    }

    return status;
}

#define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] [stereo] [bugname]"

SWITCH_STANDARD_API(deepgram_transcribe_function)
{
    char *mycmd = NULL, *argv[4] = {0};
    int argc                      = 0;
    switch_status_t status        = SWITCH_STATUS_FALSE;
    switch_media_bug_flag_t flags = SMBF_READ_STREAM /* | SMBF_WRITE_STREAM | SMBF_READ_PING */;

    if (!zstr(cmd) && (mycmd = strdup(cmd)))
    {
        argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
    }

    if (zstr(cmd) || argc < 2 || zstr(argv[0]))
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s.\n", cmd);
        stream->write_function(stream, "-USAGE: %s\n", TRANSCRIBE_API_SYNTAX);
        goto done;
    }
    else
    {
        switch_core_session_t *lsession = NULL;

        if ((lsession = switch_core_session_locate(argv[0])))
        {
            if (!strcasecmp(argv[1], "stop"))
            {
                char *bugname = argc > 2 ? argv[2] : MY_BUG_NAME;
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_INFO,
                    "stop transcribing %s\n",
                    bugname
                );
                status = do_stop(lsession, bugname);
            }
            else if (!strcasecmp(argv[1], "start"))
            {
                char *bugname = argc > 3 ? argv[3] : MY_BUG_NAME;
                if (argc > 2 && !strcmp(argv[2], "stereo"))
                {
                    flags |= SMBF_WRITE_STREAM;
                    flags |= SMBF_STEREO;
                }
                else if (argc > 2)
                {
                    /* no "stereo" token supplied — argv[2] is actually the bugname */
                    bugname = argv[2];
                }
                switch_log_printf(
                    SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_INFO,
                    "start transcribing %s\n",
                    bugname
                );
                status = start_capture(lsession, flags, bugname);
            }
            switch_core_session_rwunlock(lsession);
        }
    }

    if (status == SWITCH_STATUS_SUCCESS)
    {
        stream->write_function(stream, "+OK Success\n");
    }
    else
    {
        stream->write_function(stream, "-ERR Operation Failed\n");
    }

done:

    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_deepgram_transcribe_load)
{
    switch_api_interface_t *api_interface;

    /* create/register custom event message type */
    if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_RESULTS) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_ERROR,
            "Couldn't register subclass %s!\n",
            TRANSCRIBE_EVENT_RESULTS
        );
        return SWITCH_STATUS_TERM;
    }
    if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_CONNECT_SUCCESS) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_ERROR,
            "Couldn't register subclass %s!\n",
            TRANSCRIBE_EVENT_CONNECT_SUCCESS
        );
        return SWITCH_STATUS_TERM;
    }
    if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_CONNECT_FAIL) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_ERROR,
            "Couldn't register subclass %s!\n",
            TRANSCRIBE_EVENT_CONNECT_FAIL
        );
        return SWITCH_STATUS_TERM;
    }
    if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_DISCONNECT) != SWITCH_STATUS_SUCCESS)
    {
        switch_log_printf(
            SWITCH_CHANNEL_LOG,
            SWITCH_LOG_ERROR,
            "Couldn't register subclass %s!\n",
            TRANSCRIBE_EVENT_DISCONNECT
        );
        return SWITCH_STATUS_TERM;
    }

    /* connect internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    SWITCH_ADD_API(
        api_interface,
        "uuid_deepgram_transcribe",
        "Deepgram (rtc-transcription) Streaming Transcription API",
        deepgram_transcribe_function,
        TRANSCRIBE_API_SYNTAX
    );
    switch_console_set_complete("add uuid_deepgram_transcribe start [stereo] [bugname]");
    switch_console_set_complete("add uuid_deepgram_transcribe stop");

    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_deepgram_transcribe_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_deepgram_transcribe_shutdown)
{
    switch_event_free_subclass(TRANSCRIBE_EVENT_RESULTS);
    switch_event_free_subclass(TRANSCRIBE_EVENT_CONNECT_SUCCESS);
    switch_event_free_subclass(TRANSCRIBE_EVENT_CONNECT_FAIL);
    switch_event_free_subclass(TRANSCRIBE_EVENT_DISCONNECT);
    return SWITCH_STATUS_SUCCESS;
}
