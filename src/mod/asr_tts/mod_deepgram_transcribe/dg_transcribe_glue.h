#ifndef __DG_GLUE_H__
#define __DG_GLUE_H__

/**
 * "Glue": C++ implementation callable from C.
 */

switch_status_t dg_transcribe_session_init(
    switch_core_session_t *session,
    responseHandler_t responseHandler,
    uint32_t samples_per_second,
    uint32_t channels,
    const char *host,
    int port,
    char *bugname,
    void **ppUserData
);
switch_status_t dg_transcribe_session_stop(switch_core_session_t *session, int channelIsClosing, char *bugname);
switch_bool_t dg_transcribe_frame(switch_media_bug_t *bug, void *user_data);

#endif
