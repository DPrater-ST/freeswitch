/*
 * Minimal RFC 6455 WebSocket client — see ws_client.h for scope/non-scope.
 *
 * Uses OpenSSL's EVP digest interface (EVP_sha1()), not the legacy SHA1()
 * call, deliberately: this codebase's own CI history
 * (freeswitch-ringrx "fix(ci): demote OpenSSL-3 deprecation warnings (FS
 * -Werror)") shows the direct legacy OpenSSL API surface is exactly what
 * breaks the -Werror build on OpenSSL 3 / newer Debian-derived bases here —
 * EVP_Digest is the non-deprecated, forward-compatible path and avoids
 * reproducing that class of build breakage in a brand-new module.
 */

/* Needed for getaddrinfo/struct addrinfo/strcasecmp under a strict -std=
 * (verified: without this, -std=c11 alone hides all of POSIX.1-2008 and the
 * file fails to compile — caught by an actual standalone compile, not
 * assumed). Defined before any system header is included. */
#define _POSIX_C_SOURCE 200809L

#include "ws_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_HANDSHAKE_BUF (4096)

struct ws_client_s
{
    int fd;
    int connected;
};

/* ---- tiny, self-contained base64 (encode only) — avoids pulling in an APR
 * dependency for one function; RFC 4648 standard alphabet, no line wraps. */
static size_t b64_encode(const uint8_t *in, size_t inlen, char *out /* must be >= 4*ceil(inlen/3)+1 */)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    for (; i + 3 <= inlen; i += 3)
    {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = tbl[(n >> 6) & 0x3F];
        out[o++] = tbl[n & 0x3F];
    }
    size_t rem = inlen - i;
    if (rem == 1)
    {
        uint32_t n = in[i] << 16;
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if (rem == 2)
    {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8);
        out[o++] = tbl[(n >> 18) & 0x3F];
        out[o++] = tbl[(n >> 12) & 0x3F];
        out[o++] = tbl[(n >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int recv_line(int fd, char *buf, size_t buflen, int timeout_ms)
{
    size_t n = 0;
    while (n + 1 < buflen)
    {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0)
            return -1;
        buf[n++] = c;
        if (n >= 2 && buf[n - 2] == '\r' && buf[n - 1] == '\n')
        {
            buf[n - 2] = '\0';
            return (int)(n - 2);
        }
    }
    return -1;
}

static int recv_exact(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t got = 0;
    while (got < len)
    {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;
        ssize_t r = recv(fd, buf + got, len - got, 0);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

ws_client_t *ws_client_connect(const char *host, int port, const char *path, int connect_timeout_ms)
{
    struct addrinfo hints = {0}, *res = NULL, *rp;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return NULL;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next)
    {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        /* non-blocking connect with a real timeout, not the OS default */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0)
        {
            break; /* connected immediately (e.g. localhost) */
        }
        if (errno == EINPROGRESS)
        {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, connect_timeout_ms) > 0)
            {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0)
                    break; /* connected */
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return NULL;

    /* back to blocking mode for the handshake + the caller's steady-state I/O */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* --- client opening handshake (RFC 6455 §4.1) --- */
    uint8_t key_raw[16];
    for (int i = 0; i < 16; i++)
        key_raw[i] = (uint8_t)(rand() & 0xFF); /* handshake nonce, not a security credential */
    char key_b64[32];
    b64_encode(key_raw, sizeof(key_raw), key_b64);

    char req[WS_HANDSHAKE_BUF];
    int reqlen = snprintf(
        req,
        sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path,
        host,
        port,
        key_b64
    );
    if (reqlen < 0 || (size_t)reqlen >= sizeof(req) || send(fd, req, (size_t)reqlen, 0) != reqlen)
    {
        close(fd);
        return NULL;
    }

    /* expected Sec-WebSocket-Accept = base64(SHA1(key + GUID)) */
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", key_b64, WS_GUID);
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!EVP_Digest(concat, strlen(concat), digest, &digest_len, EVP_sha1(), NULL) || digest_len != 20)
    {
        close(fd);
        return NULL;
    }
    char expected_accept[32];
    b64_encode(digest, digest_len, expected_accept);

    /* --- read the HTTP response line-by-line until the blank line --- */
    char line[1024];
    int first = 1;
    int got_101 = 0, accept_ok = 0;
    for (;;)
    {
        int n = recv_line(fd, line, sizeof(line), connect_timeout_ms);
        if (n < 0)
        {
            close(fd);
            return NULL;
        }
        if (first)
        {
            first = 0;
            /* "HTTP/1.1 101 Switching Protocols" */
            if (strstr(line, " 101 ") != NULL)
                got_101 = 1;
            continue;
        }
        if (n == 0)
            break; /* blank line = end of headers */

        char *colon = strchr(line, ':');
        if (colon)
        {
            *colon = '\0';
            char *val = colon + 1;
            while (*val == ' ')
                val++;
            if (strcasecmp(line, "Sec-WebSocket-Accept") == 0 && strcmp(val, expected_accept) == 0)
                accept_ok = 1;
        }
    }

    if (!got_101 || !accept_ok)
    {
        close(fd);
        return NULL;
    }

    ws_client_t *ws = (ws_client_t *)calloc(1, sizeof(*ws));
    if (!ws)
    {
        close(fd);
        return NULL;
    }
    ws->fd = fd;
    ws->connected = 1;
    return ws;
}

ws_status_t ws_client_send(ws_client_t *ws, ws_frame_type_t type, const void *data, size_t len)
{
    if (!ws || !ws->connected)
        return WS_ERR_CLOSED;

    uint8_t opcode;
    switch (type)
    {
    case WS_FRAME_TEXT:
        opcode = 0x1;
        break;
    case WS_FRAME_BINARY:
        opcode = 0x2;
        break;
    case WS_FRAME_CLOSE:
        opcode = 0x8;
        len = 0;
        break;
    default:
        return WS_ERR_PROTOCOL;
    }

    /* header: FIN+opcode byte, then MASK-bit-set length byte(s), then a
     * 4-byte mask, then the masked payload. Client frames MUST be masked
     * (RFC 6455 §5.1) — the mask key itself is a per-frame obfuscation
     * requirement of the protocol, not a security boundary; a fixed non-zero
     * mask would be spec-legal, but we generate a fresh one per frame to
     * match how every real client behaves (some naive server-side WS
     * implementations reject an all-zero mask). */
    uint8_t header[14];
    size_t hlen = 0;
    header[hlen++] = 0x80 | opcode; /* FIN=1 */

    if (len <= 125)
    {
        header[hlen++] = 0x80 | (uint8_t)len;
    }
    else if (len <= 0xFFFF)
    {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (uint8_t)((len >> 8) & 0xFF);
        header[hlen++] = (uint8_t)(len & 0xFF);
    }
    else
    {
        header[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (uint8_t)((((uint64_t)len) >> (i * 8)) & 0xFF);
    }

    uint8_t mask[4];
    for (int i = 0; i < 4; i++)
        mask[i] = (uint8_t)(rand() & 0xFF);
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    if (send(ws->fd, header, hlen, 0) != (ssize_t)hlen)
        return WS_ERR_SEND;

    if (len > 0)
    {
        /* mask the payload in fixed-size chunks so we never allocate a full
         * copy of a large audio buffer just to XOR it */
        uint8_t chunk[4096];
        size_t sent = 0;
        const uint8_t *src = (const uint8_t *)data;
        while (sent < len)
        {
            size_t n = len - sent;
            if (n > sizeof(chunk))
                n = sizeof(chunk);
            for (size_t i = 0; i < n; i++)
                chunk[i] = src[sent + i] ^ mask[(sent + i) % 4];
            if (send(ws->fd, chunk, n, 0) != (ssize_t)n)
                return WS_ERR_SEND;
            sent += n;
        }
    }

    return WS_OK;
}

ws_status_t ws_client_recv(
    ws_client_t *ws,
    int timeout_ms,
    ws_frame_type_t *out_type,
    uint8_t **out_data,
    size_t *out_len
)
{
    if (!ws || !ws->connected)
        return WS_ERR_CLOSED;

    uint8_t *msg = NULL;
    size_t msg_len = 0;
    int msg_opcode = -1; /* opcode of the first frame in a fragmented message */

    for (;;)
    {
        uint8_t hdr[2];
        if (recv_exact(ws->fd, hdr, 2, timeout_ms) != 0)
        {
            free(msg);
            return WS_ERR_RECV;
        }

        int fin = (hdr[0] & 0x80) != 0;
        int opcode = hdr[0] & 0x0F;
        int masked = (hdr[1] & 0x80) != 0;
        uint64_t plen = hdr[1] & 0x7F;

        if (masked)
        {
            /* Server-to-client frames MUST NOT be masked (RFC 6455 §5.1).
             * Treat this as a hard protocol violation rather than silently
             * unmasking — a masked "server" frame here means we're not
             * actually talking to a spec-compliant WS endpoint. */
            free(msg);
            return WS_ERR_PROTOCOL;
        }

        if (plen == 126)
        {
            uint8_t ext[2];
            if (recv_exact(ws->fd, ext, 2, timeout_ms) != 0)
            {
                free(msg);
                return WS_ERR_RECV;
            }
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        }
        else if (plen == 127)
        {
            uint8_t ext[8];
            if (recv_exact(ws->fd, ext, 8, timeout_ms) != 0)
            {
                free(msg);
                return WS_ERR_RECV;
            }
            plen = 0;
            for (int i = 0; i < 8; i++)
                plen = (plen << 8) | ext[i];
        }

        uint8_t *payload = NULL;
        if (plen > 0)
        {
            payload = (uint8_t *)malloc(plen);
            if (!payload || recv_exact(ws->fd, payload, (size_t)plen, timeout_ms) != 0)
            {
                free(payload);
                free(msg);
                return WS_ERR_RECV;
            }
        }

        if (opcode == 0x8) /* close */
        {
            free(payload);
            free(msg);
            ws->connected = 0;
            return WS_ERR_CLOSED;
        }
        if (opcode == 0x9 || opcode == 0xA) /* ping/pong — not expected from rtc-transcription; ignore and keep reading */
        {
            free(payload);
            continue;
        }

        if (opcode != 0x0) /* new message (text/binary), not a continuation */
            msg_opcode = opcode;

        if (plen > 0)
        {
            uint8_t *grown = (uint8_t *)realloc(msg, msg_len + plen + 1 /* NUL slack, see header doc */);
            if (!grown)
            {
                free(payload);
                free(msg);
                return WS_ERR_RECV;
            }
            msg = grown;
            memcpy(msg + msg_len, payload, plen);
            msg_len += plen;
            free(payload);
        }

        if (fin)
            break;
        /* else: loop again for the next continuation frame of this message */
    }

    if (!msg)
    {
        msg = (uint8_t *)malloc(1); /* empty message, still NUL-terminate */
        if (!msg)
            return WS_ERR_RECV;
    }
    msg[msg_len] = '\0';

    *out_type = (msg_opcode == 0x2) ? WS_FRAME_BINARY : WS_FRAME_TEXT;
    *out_data = msg;
    *out_len = msg_len;
    return WS_OK;
}

void ws_client_destroy(ws_client_t *ws)
{
    if (!ws)
        return;
    if (ws->connected && ws->fd >= 0)
    {
        ws_client_send(ws, WS_FRAME_CLOSE, NULL, 0);
    }
    if (ws->fd >= 0)
        close(ws->fd);
    free(ws);
}

int ws_client_fd(ws_client_t *ws)
{
    return ws ? ws->fd : -1;
}
