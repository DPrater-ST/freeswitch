#ifndef __WS_CLIENT_H__
#define __WS_CLIENT_H__

/*
 * Minimal RFC 6455 WebSocket CLIENT implementation.
 *
 * Scoped deliberately narrow to what rtc-transcription's /ws/transcribe
 * protocol actually needs (see rtc-transcription/CLAUDE.md "Protocol"):
 *   - a client-mode opening handshake (Sec-WebSocket-Key/Accept)
 *   - sending masked TEXT frames (JSON control messages) and masked BINARY
 *     frames (raw PCM audio) — client-to-server frames MUST be masked
 *   - receiving unmasked TEXT frames (JSON response/event messages) from the
 *     server, with continuation-frame reassembly for robustness
 *
 * NOT implemented (out of scope for this module's needs, not silently
 * assumed safe — see RTC-12586 for the follow-up if ever needed):
 *   - wss:// / TLS. rtc-transcription is reached over plain ws:// on an
 *     internal, VPC-local path once the AWS-side engine
 *     (voicefabric-transcription-aws-plan.md) is stood up; if a future
 *     deployment needs TLS, wrap the socket in FreeSWITCH's already-linked
 *     OpenSSL before handing the fd to ws_client_attach() rather than
 *     extending this file — kept as a clean seam (fd-based) for that reason.
 *   - permessage-deflate / any WebSocket extension negotiation.
 *   - ping/pong keepalive (rtc-transcription's protocol doesn't require it;
 *     add if a future deployment's LB/proxy needs it to hold the connection).
 *   - server-to-client masked frames (masking is client-to-server only per
 *     RFC 6455 §5.1 — a masked frame arriving from the server is treated as
 *     a protocol error and the connection is torn down).
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ws_client_s ws_client_t;

typedef enum
{
    WS_OK = 0,
    WS_ERR_CONNECT = -1,
    WS_ERR_HANDSHAKE = -2,
    WS_ERR_SEND = -3,
    WS_ERR_RECV = -4,
    WS_ERR_CLOSED = -5,
    WS_ERR_PROTOCOL = -6
} ws_status_t;

typedef enum
{
    WS_FRAME_TEXT,
    WS_FRAME_BINARY,
    WS_FRAME_CLOSE
} ws_frame_type_t;

/* Connect + perform the client opening handshake against ws://host:port/path.
 * `host` may be a hostname or dotted IP; resolved via getaddrinfo. Returns a
 * new client on success (caller owns it — ws_client_destroy() when done), or
 * NULL on failure (check errno / logs at the call site — this file logs
 * nothing itself, it's a plain library, all switch_log_printf calls live in
 * the glue layer that owns the switch_core_session_t context). */
ws_client_t *ws_client_connect(const char *host, int port, const char *path, int connect_timeout_ms);

/* Send one complete, unfragmented masked frame. `len` is ignored for
 * WS_FRAME_CLOSE (a zero-length close frame is sent). Returns WS_OK or an
 * error. Thread-safe with respect to ws_client_recv() (send and receive use
 * independent buffers/state), but NOT safe to call concurrently with itself
 * from two threads — the glue layer serializes writes under its own mutex,
 * matching the existing mod_azure_transcribe convention. */
ws_status_t ws_client_send(ws_client_t *ws, ws_frame_type_t type, const void *data, size_t len);

/* Blocking receive of the next complete (reassembled) message. On WS_OK,
 * the out params (type, data, len) describe the message; the data pointer is
 * owned by the caller (malloc'd — free() it) and is NUL-terminated one byte
 * past len for convenience when the message is text (not counted in len).
 * Returns WS_ERR_CLOSED if the server closed the connection cleanly,
 * WS_ERR_PROTOCOL if the server sent a masked frame (a client-only
 * requirement per RFC 6455 — treated as a hard protocol violation) or any
 * other frame this client doesn't support (fragmented control frames,
 * reserved opcodes). `timeout_ms` <= 0 blocks indefinitely. */
ws_status_t ws_client_recv(
    ws_client_t *ws,
    int timeout_ms,
    ws_frame_type_t *out_type,
    uint8_t **out_data,
    size_t *out_len
);

/* Send a close frame (best-effort) and release all resources, including the
 * underlying socket. Safe to call after any error; idempotent. */
void ws_client_destroy(ws_client_t *ws);

/* The underlying socket fd, for a caller that wants to select()/poll() it
 * alongside other work instead of calling ws_client_recv() with a timeout
 * directly. -1 if not connected. */
int ws_client_fd(ws_client_t *ws);

#ifdef __cplusplus
}
#endif

#endif
