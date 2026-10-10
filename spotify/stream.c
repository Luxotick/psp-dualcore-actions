#include "stream.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspiofilemgr.h>
#include <pspdebug.h>

#define PSP_HTONS(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))

int spotify_stream_download(const char *hostname, const char *url_path, const char *save_path)
{
    log_printf("STREAM: Resolving %s...\n", hostname);

    int rid = 0;
    char res_buf[1024];
    int ret = sceNetResolverCreate(&rid, res_buf, sizeof(res_buf));
    if (ret < 0) {
        log_printf("STREAM: Resolver create failed (%d)\n", ret);
        return ret;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = PSP_HTONS(80);

    ret = sceNetResolverStartNtoA(rid, hostname, &server_addr.sin_addr, 5, 3);
    sceNetResolverDelete(rid);
    if (ret < 0) {
        log_printf("STREAM: DNS resolve failed (%d)\n", ret);
        return ret;
    }

    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_printf("STREAM: Socket create failed (%d)\n", sock);
        return sock;
    }

    log_printf("STREAM: Connecting to %s:80...\n", hostname);
    ret = sceNetInetConnect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        log_printf("STREAM: Connect failed (%d)\n", ret);
        sceNetInetClose(sock);
        return ret;
    }

    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: PSP-Spotify/1.0\r\n"
             "Connection: close\r\n\r\n",
             url_path, hostname);

    ret = sceNetInetSend(sock, req, strlen(req), 0);
    if (ret < 0) {
        log_printf("STREAM: Send HTTP request failed (%d)\n", ret);
        sceNetInetClose(sock);
        return ret;
    }

    SceUID f = sceIoOpen(save_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f < 0) {
        log_printf("STREAM: Failed to open %s for writing (%d)\n", save_path, f);
        sceNetInetClose(sock);
        return f;
    }

    char buf[4096];
    int header_done = 0;
    size_t total_bytes = 0;

    log_printf("STREAM: Receiving audio stream ");

    for (;;) {
        int r = sceNetInetRecv(sock, buf, sizeof(buf), 0);
        if (r <= 0) break;

        if (!header_done) {
            char *hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end) {
                header_done = 1;
                size_t hdr_len = (size_t)(hdr_end - buf) + 4;
                size_t body_len = (size_t)r - hdr_len;
                if (body_len > 0) {
                    sceIoWrite(f, hdr_end + 4, body_len);
                    total_bytes += body_len;
                }
            }
        } else {
            sceIoWrite(f, buf, (size_t)r);
            total_bytes += (size_t)r;
        }

        /* Print dot every 32 KB */
        if ((total_bytes % 32768) < (size_t)r) {
            log_printf(".");
        }
    }

    sceIoClose(f);
    sceNetInetClose(sock);

    log_printf(" OK!\nSTREAM: Saved %u KB to %s\n",
                         (unsigned int)(total_bytes / 1024), save_path);
    return 0;
}
