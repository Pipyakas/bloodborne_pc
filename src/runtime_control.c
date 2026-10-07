/* BB_CONTROL=<port>: a line protocol on 127.0.0.1:<port> for agents and test scripts that drive
 * the game without its window (tools/mcp/bbport_mcp.py; with BB_HIDDEN=1 the window is never
 * shown). Port 0 picks a free one; the log line "Runtime: control on 127.0.0.1:<port>" names it.
 * One command per line, one reply line each: "ok ..." or "error <reason>". Clients are served
 * one after another.
 *
 *   ping                           ok pid=<pid>
 *   status                         ok pad_open=<0|1> pad_reads=<n> presents=<n> text_input=<0|1>
 *   pad [tokens]                   holds BB_PAD_FILE tokens (runtime_pad.c); none releases all
 *   press <frames> <tokens>        holds them for <frames> pad reads, then releases; replies after
 *   wait <frames>                  replies after <frames> more presented frames
 *   screenshot <max_width> <path>  the next presented frame as PNG (0: full size): ok <w> <h>
 *   text <utf8>                    confirms the open text entry (IME dialog) with the text
 *   set <key> <value>              applies one bbport.ini setting live, like the menus (not saved)
 *   quit                           ends the process */
#define _GNU_SOURCE
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
typedef SOCKET Socket;
#define close_socket closesocket
#define MSG_NOSIGNAL 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Socket;
#define INVALID_SOCKET (-1)
#define close_socket close
#endif

static void reply(Socket client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void reply(Socket client, const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    send(client, line, n, MSG_NOSIGNAL);
}

/* Splits the first word off `*rest`. */
static const char *word(char **rest) {
    char *p = *rest;
    while (*p == ' ') ++p;
    char *start = p;
    while (*p && *p != ' ') ++p;
    if (*p) *p++ = 0;
    while (*p == ' ') ++p;
    *rest = p;
    return start;
}

static void command(Socket client, char *line) {
    char *rest = line;
    const char *name = word(&rest);
    if (!strcmp(name, "ping")) {
#ifdef _WIN32
        reply(client, "ok pid=%lu", (unsigned long)GetCurrentProcessId());
#else
        reply(client, "ok pid=%ld", (long)getpid());
#endif
    } else if (!strcmp(name, "status")) {
        int open = 0;
        uint64_t reads = 0;
        runtime_pad_status(&open, &reads);
        reply(client, "ok pad_open=%d pad_reads=%llu presents=%llu text_input=%d", open,
              (unsigned long long)reads, (unsigned long long)bbgpu_present_count(),
              bbgpu_text_input_active());
    } else if (!strcmp(name, "pad")) {
        runtime_pad_set(rest);
        reply(client, "ok");
    } else if (!strcmp(name, "press")) {
        const unsigned frames = (unsigned)strtoul(word(&rest), NULL, 10);
        if (!frames || frames > 100000) { reply(client, "error press <frames> <tokens>"); return; }
        if (runtime_pad_press(rest, frames, 5000 + frames * 100)) reply(client, "error the game did not read the pad");
        else reply(client, "ok");
    } else if (!strcmp(name, "wait")) {
        const unsigned long frames = strtoul(word(&rest), NULL, 10);
        const uint64_t target = bbgpu_present_count() + frames;
        const uint64_t deadline = host_monotonic_ns() + (5000 + (uint64_t)frames * 200) * 1000000u;
        while (bbgpu_present_count() < target && host_monotonic_ns() < deadline) host_sleep_until_ns(host_monotonic_ns() + 2000000);
        if (bbgpu_present_count() < target) reply(client, "error no frames presented");
        else reply(client, "ok presents=%llu", (unsigned long long)bbgpu_present_count());
    } else if (!strcmp(name, "screenshot")) {
        const int max_width = atoi(word(&rest));
        int width = 0, height = 0;
        if (!*rest) reply(client, "error screenshot <max_width> <path>");
        else if (bbgpu_capture_png(rest, max_width, 5000, &width, &height)) reply(client, "error no frame captured");
        else reply(client, "ok %d %d", width, height);
    } else if (!strcmp(name, "text")) {
        if (bbgpu_text_input_submit(rest)) reply(client, "ok");
        else reply(client, "error no text entry is open");
    } else if (!strcmp(name, "ui")) {
        // Test the actual SDL controller event path without desktop focus or guest-pad input.
        char control[32], extra;
        int value;
        if (sscanf(rest,"%31s %d %c",control,&value,&extra) != 2 ||
            !bbgpu_ui_gamepad_event(control,value)) reply(client,"error ui <control> <value>");
        else reply(client,"ok");
    } else if (!strcmp(name, "ime")) {
        // Opens the same centered input box the PS4 system IME shows, through the entry point
        // the guest uses (sceImeDialogInit), so it can be tested without playing to a prompt.
        if (bbgpu_text_input_begin(rest, "Enter a name")) reply(client, "ok");
        else reply(client, "error the text dialog could not open");
    } else if (!strcmp(name, "set")) {
        const char *key = word(&rest);
        if (!*key || !*rest) reply(client, "error set <key> <value>");
        else {
            bbgpu_set_setting(key, rest);
            printf("Runtime: setting %s=%s (control channel)\n", key, rest);
            reply(client, "ok");
        }
    } else if (!strcmp(name, "quit")) {
        reply(client, "ok");
        puts("Runtime: quit through the control channel");
        fflush(stdout);
        _exit(0);
    } else {
        reply(client, "error unknown command %.64s", name);
    }
}

static void serve(Socket client) {
    char buffer[8192];
    size_t used = 0;
    for (;;) {
        const int n = recv(client, buffer + used, (int)(sizeof(buffer) - 1 - used), 0);
        if (n <= 0) return;
        used += (size_t)n;
        buffer[used] = 0;
        char *start = buffer, *end;
        while ((end = strchr(start, '\n'))) {
            *end = 0;
            if (end > start && end[-1] == '\r') end[-1] = 0;
            if (*start) command(client, start);
            start = end + 1;
        }
        used -= (size_t)(start - buffer);
        memmove(buffer, start, used);
        if (used == sizeof(buffer) - 1) { reply(client, "error line too long"); used = 0; }
    }
}

static void *control_thread(void *argument) {
    const Socket listener = (Socket)(uintptr_t)argument;
    for (;;) {
        const Socket client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        serve(client);
        close_socket(client);
    }
    return NULL;
}

void runtime_control_start(void) {
    const char *port = getenv("BB_CONTROL");
    if (!port || !*port) return;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { puts("Runtime: control: no Winsock"); return; }
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)atoi(port));
    socklen_t length = sizeof(address);
    HostThread thread;
    if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listener, 4) || getsockname(listener, (struct sockaddr *)&address, &length) ||
        host_thread_start(&thread, 0, control_thread, (void *)(uintptr_t)listener)) {
        printf("Runtime: control: cannot listen on 127.0.0.1:%s\n", port);
        if (listener != INVALID_SOCKET) close_socket(listener);
        return;
    }
    host_thread_detach(thread);
    printf("Runtime: control on 127.0.0.1:%u\n", (unsigned)ntohs(address.sin_port));
}
