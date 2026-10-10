#include "http.h"
#include "log.h"
#include <string.h>
#include <pspdebug.h>
#include <psphttp.h>
#include <pspssl.h>
#include <psputility_netmodules.h>
#include <psputility_modules.h>
#include <psputility.h>
#include <pspsysmem.h>

/* Present in the pspsdk sceHttp stub but not declared by psphttp.h. */
int sceHttpsGetSslError(int id, int *err_code, unsigned int *detail);
int sceHttpsDisableOption(unsigned int flags);

#define HTTPS_FLAG_SERVER_VERIFY 0x01u
#define HTTP_TIMEOUT_US (10u * 1000u * 1000u)

static int http_ready;
static int https_ready;
static int https_verify = 1;

int http_init(void)
{
    if (http_ready) return 0;

    static const int modules[] = {
        PSP_NET_MODULE_PARSEURI, PSP_NET_MODULE_PARSEHTTP,
        PSP_NET_MODULE_HTTP, PSP_NET_MODULE_SSL
    };
    log_printf("HTTP: free mem %u KB (max block %u KB)\n",
               (unsigned int)(sceKernelTotalFreeMemSize() / 1024),
               (unsigned int)(sceKernelMaxFreeMemSize() / 1024));
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        int ret = sceUtilityLoadNetModule(modules[i]);
        if (ret < 0 && ret != (int)0x80110801) {
            log_printf("HTTP: load net module %d failed 0x%08X\n",
                       modules[i], (unsigned int)ret);
            /* Give the memory back so the AP socket path still works. */
            while (i-- > 0) sceUtilityUnloadNetModule(modules[i]);
            return ret;
        }
    }

    int ret = sceSslInit(0x28000);
    if (ret < 0) {
        log_printf("HTTP: sceSslInit 0x%08X\n", (unsigned int)ret);
        return ret;
    }
    ret = sceHttpInit(0x25800);
    if (ret < 0) {
        log_printf("HTTP: sceHttpInit 0x%08X\n", (unsigned int)ret);
        sceSslEnd();
        return ret;
    }
    http_ready = 1;
    log_printf("HTTP: stack up, free mem %u KB\n",
               (unsigned int)(sceKernelTotalFreeMemSize() / 1024));

    ret = sceHttpsInit(0, 0, 0, 0);
    if (ret < 0) {
        /* Plain HTTP still works without the HTTPS layer. */
        log_printf("HTTP: sceHttpsInit 0x%08X (HTTPS off)\n", (unsigned int)ret);
        return 0;
    }
    https_ready = 1;
    ret = sceHttpsLoadDefaultCert(0, 0);
    if (ret < 0)
        log_printf("HTTP: LoadDefaultCert 0x%08X\n", (unsigned int)ret);
    return 0;
}

void http_term(void)
{
    if (https_ready) sceHttpsEnd();
    if (http_ready) {
        sceHttpEnd();
        sceSslEnd();
    }
    https_ready = http_ready = 0;
}

static void report_ssl_error(int req)
{
    int err = 0;
    unsigned int detail = 0;
    if (sceHttpsGetSslError(req, &err, &detail) >= 0)
        log_printf("HTTP: SSL error 0x%08X detail 0x%08X\n",
                             (unsigned int)err, detail);
}

static int open_once(http_stream *s, const char *url, uint64_t *content_length)
{
    s->tmpl = s->conn = s->req = -1;

    s->tmpl = sceHttpCreateTemplate((char *)"PSP-Spotify/1.0", 1, 1);
    if (s->tmpl < 0) return s->tmpl;
    sceHttpSetResolveTimeOut(s->tmpl, HTTP_TIMEOUT_US);
    sceHttpSetConnectTimeOut(s->tmpl, HTTP_TIMEOUT_US);
    sceHttpSetSendTimeOut(s->tmpl, HTTP_TIMEOUT_US);
    sceHttpSetRecvTimeOut(s->tmpl, HTTP_TIMEOUT_US);
    sceHttpEnableRedirect(s->tmpl);
    sceHttpDisableCookie(s->tmpl);

    s->conn = sceHttpCreateConnectionWithURL(s->tmpl, url, 0);
    if (s->conn < 0) return s->conn;

    s->req = sceHttpCreateRequestWithURL(s->conn, PSP_HTTP_METHOD_GET, (char *)url, 0);
    if (s->req < 0) return s->req;

    int ret = sceHttpSendRequest(s->req, NULL, 0);
    if (ret < 0) {
        if (strncmp(url, "https://", 8) == 0) report_ssl_error(s->req);
        return ret;
    }

    int status = 0;
    ret = sceHttpGetStatusCode(s->req, &status);
    if (ret < 0) return ret;

    SceULong64 len = 0;
    *content_length = sceHttpGetContentLength(s->req, &len) >= 0 ? (uint64_t)len : 0;
    return status;
}

int http_stream_open(http_stream *s, const char *url, uint64_t *content_length)
{
    if (!http_ready) return -1;
    *content_length = 0;
    int is_https = strncmp(url, "https://", 8) == 0;
    if (is_https && !https_ready) {
        log_printf("HTTP: HTTPS layer not available\n");
        return -2;
    }

    int ret = open_once(s, url, content_length);
    if (ret < 0 && is_https && https_verify) {
        /* The 6.61 CA store predates the roots Spotify uses (GlobalSign R3,
         * DigiCert G2). Retry once without server verification so the probe
         * shows whether the TLS 1.2 handshake itself works. */
        log_printf("HTTP: 0x%08X with cert verify, retrying without\n",
                             (unsigned int)ret);
        http_stream_close(s);
        sceHttpsDisableOption(HTTPS_FLAG_SERVER_VERIFY);
        https_verify = 0;
        ret = open_once(s, url, content_length);
    }
    if (ret < 0) {
        int net_errno = 0;
        if (s->req >= 0 && sceHttpGetNetworkErrno(s->req, &net_errno) >= 0 && net_errno)
            log_printf("HTTP: net errno %d\n", net_errno);
        http_stream_close(s);
    }
    return ret;
}

int http_stream_read(http_stream *s, void *buf, unsigned int len)
{
    return sceHttpReadData(s->req, buf, len);
}

void http_stream_close(http_stream *s)
{
    if (s->req >= 0) sceHttpDeleteRequest(s->req);
    if (s->conn >= 0) sceHttpDeleteConnection(s->conn);
    if (s->tmpl >= 0) sceHttpDeleteTemplate(s->tmpl);
    s->tmpl = s->conn = s->req = -1;
}
