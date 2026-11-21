/* Background Windows named-pipe JSON control server (\\.\pipe\longshot-js). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_pipe.h"
#include "host_bridge.h"
#include "../host/js_api.h"
#include "../host/js_internal.h"

static HANDLE g_pipe_thread = NULL;
static HANDLE g_stop_event = NULL;
static volatile LONG g_pipe_running = 0;

/* Minimal safe JSON string extractor for {"cmd":"...","id":...,"code":"...","filename":"...","since":...}. */
static const char *skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *find_json_field(const char *json, const char *key) {
    const size_t klen = strlen(key);
    const char *p = json;
    int in_str = 0;
    while (*p) {
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (!in_str && (size_t)(p - start) == klen && !strncmp(start, key, klen)) {
                if (*p == '"') p++;
                p = skip_ws(p);
                if (*p == ':') return skip_ws(p + 1);
            }
            if (*p == '"') p++;
            continue;
        }
        p++;
    }
    return NULL;
}

static char *dup_json_string(const char *val_start) {
    if (!val_start || *val_start != '"') return NULL;
    const char *p = val_start + 1;
    size_t cap = strlen(p) + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case '"': case '\\': case '/': out[o++] = *p++; break;
            case 'b': out[o++] = '\b'; p++; break;
            case 'f': out[o++] = '\f'; p++; break;
            case 'n': out[o++] = '\n'; p++; break;
            case 'r': out[o++] = '\r'; p++; break;
            case 't': out[o++] = '\t'; p++; break;
            case 'u': {
                unsigned code = 0;
                if (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
                    isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    code = (unsigned)strtoul(hex, NULL, 16);
                    p += 5;
                    if (code < 0x80) out[o++] = (char)code;
                    else if (code < 0x800) {
                        out[o++] = (char)(0xC0 | (code >> 6));
                        out[o++] = (char)(0x80 | (code & 0x3F));
                    } else {
                        out[o++] = (char)(0xE0 | (code >> 12));
                        out[o++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (code & 0x3F));
                    }
                } else {
                    out[o++] = *p++;
                }
                break;
            }
            default: out[o++] = *p++; break;
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return out;
}

static void extract_json_id(const char *json, char *id_buf, size_t cap) {
    const char *v = find_json_field(json, "id");
    if (!v) {
        snprintf(id_buf, cap, "null");
        return;
    }
    if (*v == '"') {
        const char *p = v + 1;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p += 2;
            else p++;
        }
        size_t len = (size_t)(p - v + (*p == '"' ? 1 : 0));
        if (len >= cap) len = cap - 1;
        memcpy(id_buf, v, len);
        id_buf[len] = 0;
        return;
    }
    size_t len = 0;
    while (v[len] && !strchr(",}] \t\r\n", v[len]) && len + 1 < cap) len++;
    if (!len) snprintf(id_buf, cap, "null");
    else memcpy(id_buf, v, len), id_buf[len] = 0;
}

static void write_pipe_reply(HANDLE pipe, const char *s, DWORD len) {
    DWORD written = 0;
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return;
    if (!WriteFile(pipe, s, len, &written, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            HANDLE hs[2] = {ov.hEvent, g_stop_event};
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, 5000);
            if (w == WAIT_OBJECT_0) GetOverlappedResult(pipe, &ov, &written, FALSE);
            else CancelIoEx(pipe, &ov);
        }
    }
    CloseHandle(ov.hEvent);
}

static void send_json_error(HANDLE pipe, const char *id, const char *code, const char *msg) {
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "{\"id\":%s,\"ok\":false,\"frame\":%lld,\"error\":\"%s\",\"message\":\"%s\"}\n",
                     id, (long long)hos_js_asi_present_count(), code, msg);
    if (n > 0) write_pipe_reply(pipe, buf, (DWORD)n);
}

static void handle_line(HANDLE pipe, const char *line) {
    char id[64];
    extract_json_id(line, id, sizeof id);

    char *cmd = dup_json_string(find_json_field(line, "cmd"));
    if (!cmd) {
        send_json_error(pipe, id, "bad_req", "missing cmd");
        return;
    }

    const int64_t frame = hos_js_asi_present_count();

    if (!strcmp(cmd, "status")) {
        char buf[512];
        int64_t r = 0, d = 0; int w = 0, h = 0;
        if (g_exe.rx_control_frame_info) g_exe.rx_control_frame_info(&r, &d, &w, &h);
        int n = snprintf(buf, sizeof buf,
                         "{\"id\":%s,\"ok\":true,\"frame\":%lld,\"ready\":%s,\"width\":%d,\"height\":%d,\"asi\":true}\n",
                         id, (long long)frame, hos_js_is_ready() ? "true" : "false", w, h);
        if (n > 0) write_pipe_reply(pipe, buf, (DWORD)n);
    } else if (!strcmp(cmd, "js_eval")) {
        char *code = dup_json_string(find_json_field(line, "code"));
        char *filename = dup_json_string(find_json_field(line, "filename"));
        if (!code) {
            send_json_error(pipe, id, "bad_arg", "code string required");
        } else {
            char *out = (char *)malloc(65536);
            char *resp = (char *)malloc(65536 + 256);
            if (out && resp) {
                hos_js_eval_json(code, filename ? filename : "<control>", out, 65536);
                int n = snprintf(resp, 65536 + 256, "{\"id\":%s,\"ok\":true,\"frame\":%lld,\"result\":%s}\n",
                                 id, (long long)frame, out);
                if (n > 0) write_pipe_reply(pipe, resp, (DWORD)n);
            }
            free(out);
            free(resp);
        }
        free(code);
        free(filename);
    } else if (!strcmp(cmd, "js_reload")) {
        const int ok = hos_js_reload();
        char *out = (char *)malloc(65536);
        char *resp = (char *)malloc(65536 + 256);
        if (out && resp && hos_js_list_json(out, 65536)) {
            int n = snprintf(resp, 65536 + 256,
                             "{\"id\":%s,\"ok\":true,\"frame\":%lld,\"reloaded\":%s,\"result\":%s}\n",
                             id, (long long)frame, ok ? "true" : "false", out);
            if (n > 0) write_pipe_reply(pipe, resp, (DWORD)n);
        } else {
            char buf[256];
            int n = snprintf(buf, sizeof buf, "{\"id\":%s,\"ok\":true,\"frame\":%lld,\"reloaded\":%s}\n",
                             id, (long long)frame, ok ? "true" : "false");
            if (n > 0) write_pipe_reply(pipe, buf, (DWORD)n);
        }
        free(out);
        free(resp);
    } else if (!strcmp(cmd, "js_list")) {
        char *out = (char *)malloc(65536);
        char *resp = (char *)malloc(65536 + 256);
        if (out && resp) {
            hos_js_list_json(out, 65536);
            int n = snprintf(resp, 65536 + 256, "{\"id\":%s,\"ok\":true,\"frame\":%lld,\"result\":%s}\n",
                             id, (long long)frame, out);
            if (n > 0) write_pipe_reply(pipe, resp, (DWORD)n);
        }
        free(out);
        free(resp);
    } else if (!strcmp(cmd, "js_log")) {
        const char *sv = find_json_field(line, "since");
        int since = sv ? atoi(sv) : 0;
        char *out = (char *)malloc(65536);
        char *resp = (char *)malloc(65536 + 256);
        if (out && resp) {
            hos_js_log_json(since, out, 65536);
            int n = snprintf(resp, 65536 + 256, "{\"id\":%s,\"ok\":true,\"frame\":%lld,\"result\":%s}\n",
                             id, (long long)frame, out);
            if (n > 0) write_pipe_reply(pipe, resp, (DWORD)n);
        }
        free(out);
        free(resp);
    } else {
        send_json_error(pipe, id, "unknown_cmd", "unsupported js pipe command");
    }
    free(cmd);
}

static void serve_client(HANDLE pipe) {
    size_t cap = 16384;
    size_t used = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return;

    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) { free(buf); return; }

    while (InterlockedCompareExchange(&g_pipe_running, 0, 0)) {
        if (used + 4096 >= cap) {
            if (cap >= 1024 * 1024) { used = 0; break; }
            char *nb = (char *)realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        OVERLAPPED ov = {0};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD got = 0;
        BOOL ok = ReadFile(pipe, buf + used, (DWORD)(cap - used - 1), &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE hs[2] = {ev, g_stop_event};
                DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                if (w != WAIT_OBJECT_0 || !GetOverlappedResult(pipe, &ov, &got, FALSE)) {
                    CancelIoEx(pipe, &ov);
                    break;
                }
            } else {
                break;
            }
        }
        if (got == 0) break;
        used += got;
        buf[used] = 0;

        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = 0;
            size_t len = (size_t)(nl - start);
            if (len > 0 && start[len - 1] == '\r') start[len - 1] = 0;
            if (*skip_ws(start)) handle_line(pipe, start);
            start = nl + 1;
        }
        if (start > buf) {
            size_t rem = used - (size_t)(start - buf);
            if (rem) memmove(buf, start, rem);
            used = rem;
        }
    }

    CloseHandle(ev);
    free(buf);
}

static HANDLE make_user_only_pipe(const char *pipe_name) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
    HANDLE tok = NULL;
    char sddl[256] = "D:(A;;GA;;;OW)";
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        uint8_t ubuf[256];
        DWORD need = 0;
        if (GetTokenInformation(tok, TokenUser, ubuf, sizeof ubuf, &need)) {
            LPSTR sid_str = NULL;
            if (ConvertSidToStringSidA(((TOKEN_USER *)ubuf)->User.Sid, &sid_str)) {
                snprintf(sddl, sizeof sddl, "D:(A;;GA;;;%s)", sid_str);
                LocalFree(sid_str);
            }
        }
        CloseHandle(tok);
    }
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL)) return INVALID_HANDLE_VALUE;
    sa.lpSecurityDescriptor = sd;

    HANDLE pipe = CreateNamedPipeA(pipe_name,
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   4, 65536, 65536, 0, &sa);
    if (sd) LocalFree(sd);
    return pipe;
}

static DWORD WINAPI pipe_thread_proc(LPVOID param) {
    char pipe_name[256];
    snprintf(pipe_name, sizeof pipe_name, "%s", (const char *)param);
    free(param);

    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) return 0;

    while (InterlockedCompareExchange(&g_pipe_running, 0, 0)) {
        HANDLE pipe = make_user_only_pipe(pipe_name);
        if (pipe == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(g_stop_event, 200);
            continue;
        }

        OVERLAPPED ov = {0};
        ov.hEvent = ev;
        ResetEvent(ev);
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE hs[2] = {ev, g_stop_event};
                DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) {
                    DWORD dummy = 0;
                    connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE);
                } else {
                    CancelIoEx(pipe, &ov);
                }
            }
        }

        if (connected && InterlockedCompareExchange(&g_pipe_running, 0, 0)) {
            serve_client(pipe);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    CloseHandle(ev);
    return 0;
}

void js_pipe_start(void) {
    if (InterlockedCompareExchange(&g_pipe_running, 0, 0)) return;
    char env[128] = "";
    DWORD elen = GetEnvironmentVariableA("HOS_JS_CONTROL", env, sizeof env);
    if (elen > 0 && !strcmp(env, "0")) return;

    const char *name = (elen > 0 && env[0]) ? env : "longshot-js";
    char *full = (char *)malloc(256);
    if (!full) return;
    if (!strncmp(name, "\\\\.\\pipe\\", 9)) snprintf(full, 256, "%s", name);
    else snprintf(full, 256, "\\\\.\\pipe\\%s", name);

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop_event) { free(full); return; }
    InterlockedExchange(&g_pipe_running, 1);
    g_pipe_thread = CreateThread(NULL, 0, pipe_thread_proc, full, 0, NULL);
    if (g_pipe_thread) {
        if (g_exe.hos_log) g_exe.hos_log("longshot-js: control pipe listening at %s", full);
    } else {
        InterlockedExchange(&g_pipe_running, 0);
        free(full);
    }
}

void js_pipe_stop(void) {
    if (!InterlockedExchange(&g_pipe_running, 0)) return;
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_pipe_thread) {
        WaitForSingleObject(g_pipe_thread, 2000);
        CloseHandle(g_pipe_thread);
        g_pipe_thread = NULL;
    }
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
    }
}
