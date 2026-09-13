/* ympd
   (c) 2013-2014 Andrew Karpow <andy@ndyk.de>
   This project's homepage is: http://www.ympd.org
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
/**
 已经改成新的ws模式，不在本函数里调用mg_ws_send,而是统一在主程序的消息MG_EV_WAKEUP里发送
 需要通过bool mg_wakeup(struct mg_mgr *mgr, unsigned long conn_id, const void *buf,size_t len)发送
  */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <libgen.h>
#include <mpd/client.h>
#include <mpd/message.h>

#include "mpd_client.h"
#include "config.h"
#include "json_encode.h"
#include "audio_devices.h"

/*
 * Coarse-grained recursive mutex around the single shared libmpdclient
 * connection (mpd.conn). Rationale is in mpd_client.h. A recursive mutex is
 * used because a few shutdown paths legitimately nest (mpd_clear_all ->
 * mpd_disconnect -> mpd_poll) and because the lock is error-prone to hold by
 * hand; it must never deadlock, since that would freeze the service.
 */
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION mpd_mutex;
static int mpd_mutex_ready = 0;

void mpd_lock_init(void)
{
    if (!mpd_mutex_ready) {
        InitializeCriticalSection(&mpd_mutex);
        mpd_mutex_ready = 1;
    }
}
void mpd_lock(void)   { mpd_lock_init(); EnterCriticalSection(&mpd_mutex); }
void mpd_unlock(void) { LeaveCriticalSection(&mpd_mutex); }
#else
#include <pthread.h>
static pthread_mutex_t mpd_mutex = PTHREAD_MUTEX_INITIALIZER;

void mpd_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mpd_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
void mpd_lock(void)   { pthread_mutex_lock(&mpd_mutex); }
void mpd_unlock(void) { pthread_mutex_unlock(&mpd_mutex); }
#endif

char dirble_api_token[28];
struct t_mpd mpd;

/* ------------------------------------------------------------------
 * 开机自启（autostart）
 *
 * Windows：以当前用户注册表 Run 项为准
 *   HKCU\Software\Microsoft\Windows\CurrentVersion\Run
*   值名   : "aurmpd"
 *   值内容 : "<aurmpd.exe 所在目录>\winaurmpd.exe"（带引号，路径可含空格）
 *   存在即“已启用”，不存在即“未启用”。
 * Linux：不写任何自启文件，命令仍存在但一律回 supported:false，
 *        前端据此隐藏设置项（优雅降级）。
 * ------------------------------------------------------------------ */
#ifdef _WIN32
#define AUTOSTART_SUPPORTED 1
#define AUTOSTART_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTOSTART_VALUE_NAME L"aurmpd"
/* Run 项虽不受 WOW64 重定向影响，显式指定 64 位视图可避免在
 * 32 位宿主（wine / WOW64）下读到另一视图而误判状态。 */
#define AUTOSTART_REG_ACCESS (KEY_READ | KEY_WRITE | KEY_WOW64_64KEY)

/* 取 "<aurmpd.exe 目录>\winaurmpd.exe"；成功返回 0 */
static int autostart_launcher_path(wchar_t *out, size_t cch)
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;
    size_t dirlen, need;
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);

    if (n == 0 || n >= MAX_PATH) {
        fprintf(stderr, "AUTOSTART: GetModuleFileNameW failed (%lu)\n",
                (unsigned long)GetLastError());
        return -1;
    }
    slash = wcsrchr(path, L'\\');
    if (slash == NULL)
        return -1;
    slash[1] = L'\0';              /* 保留结尾反斜杠 */
    dirlen = wcslen(path);
    need = dirlen + wcslen(L"winaurmpd.exe") + 1;
    if (need > cch)
        return -1;
    wcscpy(out, path);
    wcscat(out, L"winaurmpd.exe");
    return 0;
}

/* 当前实际状态：值存在即 1，否则 0（读注册表出错也按 0 处理并打日志） */
static int autostart_get_enabled(void)
{
    HKEY hkey;
    LONG rc;

    rc = RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0,
                       AUTOSTART_REG_ACCESS, &hkey);
    if (rc != ERROR_SUCCESS) {
        if (rc != ERROR_FILE_NOT_FOUND)
            fprintf(stderr, "AUTOSTART: RegOpenKeyExW failed (%ld), reporting disabled\n",
                    (long)rc);
        return 0;
    }
    rc = RegQueryValueExW(hkey, AUTOSTART_VALUE_NAME, NULL, NULL, NULL, NULL);
    RegCloseKey(hkey);
    if (rc == ERROR_SUCCESS)
        return 1;
    if (rc != ERROR_FILE_NOT_FOUND)
        fprintf(stderr, "AUTOSTART: RegQueryValueExW failed (%ld), reporting disabled\n",
                (long)rc);
    return 0;
}

/* 写入/删除自启值；成功返回 0，失败返回 -1（不中断服务，仅打日志） */
static int autostart_set_enabled(int enable)
{
    HKEY hkey;
    LONG rc;

    if (enable) {
        wchar_t path[MAX_PATH + 8];
        wchar_t quoted[MAX_PATH + 12];

        if (autostart_launcher_path(path, sizeof(path) / sizeof(path[0])) != 0) {
            fprintf(stderr, "AUTOSTART: cannot resolve winaurmpd.exe path\n");
            return -1;
        }
        quoted[0] = L'"';
        wcscpy(quoted + 1, path);
        wcscat(quoted, L"\"");

        rc = RegCreateKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0, NULL, 0,
                             AUTOSTART_REG_ACCESS, NULL, &hkey, NULL);
        if (rc != ERROR_SUCCESS) {
            fprintf(stderr, "AUTOSTART: RegCreateKeyExW failed (%ld)\n", (long)rc);
            return -1;
        }
        rc = RegSetValueExW(hkey, AUTOSTART_VALUE_NAME, 0, REG_SZ,
                            (const BYTE *)quoted,
                            (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
        RegCloseKey(hkey);
        if (rc != ERROR_SUCCESS) {
            fprintf(stderr, "AUTOSTART: RegSetValueExW failed (%ld)\n", (long)rc);
            return -1;
        }
        return 0;
    }

    rc = RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0,
                       AUTOSTART_REG_ACCESS, &hkey);
    if (rc != ERROR_SUCCESS) {
        /* 键不存在 => 本来就没启用，算成功 */
        if (rc == ERROR_FILE_NOT_FOUND)
            return 0;
        fprintf(stderr, "AUTOSTART: RegOpenKeyExW failed (%ld)\n", (long)rc);
        return -1;
    }
    rc = RegDeleteValueW(hkey, AUTOSTART_VALUE_NAME);
    RegCloseKey(hkey);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        fprintf(stderr, "AUTOSTART: RegDeleteValueW failed (%ld)\n", (long)rc);
        return -1;
    }
    return 0;
}
#else /* !_WIN32：Linux 优雅降级为“不支持”，不写任何自启文件 */
#define AUTOSTART_SUPPORTED 0
static int autostart_get_enabled(void) { return 0; }
static int autostart_set_enabled(int enable) { (void)enable; return 0; }
#endif

/* {"type":"autostart","data":{"supported":<bool>,"enabled":<bool>}} */
static int mpd_put_autostart(char *buffer, int enabled)
{
    return snprintf(buffer, MAX_SIZE,
                    "{\"type\":\"autostart\",\"data\":{\"supported\":%s,\"enabled\":%s}}",
                    AUTOSTART_SUPPORTED ? "true" : "false",
                    enabled ? "true" : "false");
}

/* ==================================================================
 * 音频输出设备（扬声器）
 *
 * Windows：
 *   - 设备表用 winmm（waveOutGetNumDevs/waveOutGetDevCapsW）枚举，序号与 mpd
 *     winmm 插件的 device 参数同一套语义（插件自己也是 waveOutGetNumDevs +
 *     strtoul/名字前缀匹配）。
 *   - mpd 的 audio_output 是配置项，改完必须重启 mpd 才生效；而 mpd 由启动器
 *     （winaurmpd.exe）持有，所以这里的流程是：
 *       1) 试开目标设备（waveOutOpen）确认可用，避免把 mpd 写死在一个坏设备上
 *       2) 保存当前队列到 .mpd/aurmpd-restart-queue（mpd 重启会丢队列）
 *       3) 改写 mpd.conf 的 audio_output.device
 *       4) 写请求文件 .mpd/restart-request，启动器的定时器（1s）看到后
 *          重启 mpd（新进程仍由启动器创建，因此仍在同一个 Job 对象里，
 *          启动器退出时不会残留），并写 .mpd/restart-result
 *       5) mpd 重连成功后恢复队列，并把新的设备状态广播给所有客户端
 *       6) 超时（AUDIO_RESTART_TIMEOUT 秒）仍未连上 -> 回可见错误，不静默
 * Linux：能列出设备（supported=true），但 canSet=false，前端禁用「应用」
 *        并提示手改 mpd.conf。
 * ================================================================== */
#define AUDIO_QUEUE_FILE     ".mpd/aurmpd-restart-queue"
#define AUDIO_REQUEST_FILE   ".mpd/restart-request"
#define AUDIO_RESULT_FILE    ".mpd/restart-result"
#define AUDIO_RESTART_TIMEOUT 25

static int s_audio_restart_pending = 0;
static time_t s_audio_restart_deadline = 0;

/* {"type":"error","data":"<msg>"}（转义交给 json_emit_quoted_str） */
static int audio_err(char *buffer, const char *msg)
{
    int n = json_emit_raw_str(buffer, MAX_SIZE, "{\"type\":\"error\",\"data\":");
    n += json_emit_quoted_str(buffer + n, MAX_SIZE - n, msg);
    n += json_emit_raw_str(buffer + n, MAX_SIZE - n, "}");
    return n;
}

static int mpd_api_get_audio_devices(char *buffer)
{
    struct audio_output_info info;
    struct audio_device devs[AUDIO_DEVICE_MAX];
    int ndev = audio_devices_enumerate(&info, devs, AUDIO_DEVICE_MAX);

    return audio_devices_json(buffer, MAX_SIZE, &info, devs, ndev);
}

#ifdef _WIN32

/* URI 按行存盘：转义 % 与控制字符（文件名里可能有空格，不能用空格分词） */
static void uri_encode(const char *uri, char *out, size_t outlen)
{
    size_t o = 0;

    for (; *uri != '\0' && o + 4 < outlen; uri++) {
        unsigned char c = (unsigned char)*uri;
        if (c == '%' || c == '\n' || c == '\r' || c < 0x20)
            o += (size_t)snprintf(out + o, outlen - o, "%%%02X", c);
        else
            out[o++] = (char)c;
    }
    out[o] = '\0';
}

static void uri_decode(const char *in, char *out, size_t outlen)
{
    size_t o = 0;

    while (*in != '\0' && o + 1 < outlen) {
        if (in[0] == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = { in[1], in[2], '\0' };
            out[o++] = (char)strtol(hex, NULL, 16);
            in += 3;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
}

/* 保存 mpd 当前队列与播放状态；返回 0 表示「可以恢复」（文件已写好），
 * 非 0 表示没保存（队列为空 / mpd 未连接 / 出错）——此时重启会清空队列，
 * 前端会看到提示。 */
static int audio_queue_save(void)
{
    struct mpd_status *st;
    struct mpd_song *song;
    FILE *f;
    unsigned n = 0;
    int rc = 0;

    if (mpd.conn == NULL) return -1;

    st = mpd_run_status(mpd.conn);
    if (st == NULL) {
        fprintf(stderr, "AUDIO: queue save: %s\n", mpd_connection_get_error_message(mpd.conn));
        mpd_connection_clear_error(mpd.conn);
        return -1;
    }
    if (mpd_status_get_queue_length(st) == 0) {
        mpd_status_free(st);
        return -1; /* 空队列，没什么可存 */
    }

    f = fopen(AUDIO_QUEUE_FILE, "wb");
    if (f == NULL) {
        mpd_status_free(st);
        fprintf(stderr, "AUDIO: cannot open %s\n", AUDIO_QUEUE_FILE);
        return -1;
    }
    fprintf(f, "aurmpd-restart-queue v1\n");
    fprintf(f, "state %d\nvolume %d\nsongpos %d\n",
            (int)mpd_status_get_state(st), mpd_status_get_volume(st),
            mpd_status_get_song_pos(st));
    fprintf(f, "repeat %d\nsingle %d\nrandom %d\nconsume %d\ncrossfade %d\n",
            mpd_status_get_repeat(st), mpd_status_get_single(st),
            mpd_status_get_random(st), mpd_status_get_consume(st),
            mpd_status_get_crossfade(st));
    mpd_status_free(st);

    if (mpd_send_list_queue_meta(mpd.conn)) {
        while ((song = mpd_recv_song(mpd.conn)) != NULL) {
            const char *uri = mpd_song_get_uri(song);
            if (uri != NULL) {
                char enc[AUDIO_NAME_MAX * 4];
                uri_encode(uri, enc, sizeof(enc));
                fprintf(f, "uri %s\n", enc);
                n++;
            }
            mpd_song_free(song);
        }
        if (!mpd_response_finish(mpd.conn) ||
            mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS) {
            fprintf(stderr, "AUDIO: queue save: %s\n", mpd_connection_get_error_message(mpd.conn));
            mpd_connection_clear_error(mpd.conn);
            rc = -1;
        }
    } else {
        fprintf(stderr, "AUDIO: queue save: %s\n", mpd_connection_get_error_message(mpd.conn));
        mpd_connection_clear_error(mpd.conn);
        rc = -1;
    }
    fclose(f);

    if (rc != 0 || n == 0) {
        remove(AUDIO_QUEUE_FILE);
        return -1;
    }
    fprintf(stderr, "AUDIO: saved %u queue entries for restart\n", n);
    return 0;
}

/* mpd 重启后恢复队列与播放状态；成功或失败都会删掉 save 文件（避免反复恢复旧队列） */
static void audio_queue_restore(void)
{
    FILE *f;
    char line[2048];
    char **uris = NULL;
    size_t nuris = 0, cap = 0, i;
    int state = -1, volume = -1, songpos = -1;
    int repeat = 0, single = 0, random = 0, consume = 0, crossfade = 0;
    int ok = 1;

    if (mpd.conn == NULL) return;
    f = fopen(AUDIO_QUEUE_FILE, "rb");
    if (f == NULL) return;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) *nl = '\0';
        if (strncmp(line, "state ", 6) == 0) state = atoi(line + 6);
        else if (strncmp(line, "volume ", 7) == 0) volume = atoi(line + 7);
        else if (strncmp(line, "songpos ", 8) == 0) songpos = atoi(line + 8);
        else if (strncmp(line, "repeat ", 7) == 0) repeat = atoi(line + 7);
        else if (strncmp(line, "single ", 7) == 0) single = atoi(line + 7);
        else if (strncmp(line, "random ", 7) == 0) random = atoi(line + 7);
        else if (strncmp(line, "consume ", 8) == 0) consume = atoi(line + 8);
        else if (strncmp(line, "crossfade ", 10) == 0) crossfade = atoi(line + 10);
        else if (strncmp(line, "uri ", 4) == 0) {
            char dec[AUDIO_NAME_MAX * 4];
            uri_decode(line + 4, dec, sizeof(dec));
            if (dec[0] == '\0') continue;
            if (nuris == cap) {
                char **tmp;
                size_t ncap = cap ? cap * 2 : 64;
                if (ncap > 5000) break;
                tmp = (char **)realloc(uris, ncap * sizeof(char *));
                if (tmp == NULL) { ok = 0; break; }
                uris = tmp;
                cap = ncap;
            }
            uris[nuris] = strdup(dec);
            if (uris[nuris] == NULL) { ok = 0; break; }
            nuris++;
        }
    }
    fclose(f);

    if (nuris == 0) goto out;

    if (!mpd_run_clear(mpd.conn)) {
        fprintf(stderr, "AUDIO: queue restore: clear failed: %s\n",
                mpd_connection_get_error_message(mpd.conn));
        mpd_connection_clear_error(mpd.conn);
        ok = 0;
        goto out;
    }
    for (i = 0; i < nuris; i++) {
        if (!mpd_run_add(mpd.conn, uris[i])) {
            fprintf(stderr, "AUDIO: queue restore: add \"%s\" failed: %s\n", uris[i],
                    mpd_connection_get_error_message(mpd.conn));
            mpd_connection_clear_error(mpd.conn);
            ok = 0;
            break;
        }
    }
    if (ok) {
        if (volume >= 0) mpd_run_set_volume(mpd.conn, (unsigned)volume);
        mpd_run_repeat(mpd.conn, repeat != 0);
        mpd_run_random(mpd.conn, random != 0);
        mpd_run_consume(mpd.conn, consume != 0);
        mpd_run_single(mpd.conn, single != 0);
        mpd_run_crossfade(mpd.conn, (unsigned)(crossfade > 0 ? crossfade : 0));
        if (songpos >= 0 && state == MPD_STATE_PLAY) {
            mpd_run_play_pos(mpd.conn, (unsigned)songpos);
        } else if (songpos >= 0 && state == MPD_STATE_PAUSE) {
            if (mpd_run_play_pos(mpd.conn, (unsigned)songpos))
                mpd_run_pause(mpd.conn, true);
        }
    }

out:
    for (i = 0; i < nuris; i++) free(uris[i]);
    free(uris);
    remove(AUDIO_QUEUE_FILE);
    fprintf(stderr, "AUDIO: queue restore %s (%lu entries)\n",
            ok && nuris ? "ok" : "skipped/failed", (unsigned long)nuris);
}

/* 写「请启动器重启 mpd」的请求文件（.mpd 不存在时兜底创建） */
static int audio_write_restart_request(const char *id)
{
    FILE *f = fopen(AUDIO_REQUEST_FILE, "wb");

    if (f == NULL) {
        CreateDirectoryA(".mpd", NULL);
        f = fopen(AUDIO_REQUEST_FILE, "wb");
        if (f == NULL) {
            fprintf(stderr, "AUDIO: cannot write %s\n", AUDIO_REQUEST_FILE);
            return -1;
        }
    }
    fprintf(f, "%s\n", id);
    fclose(f);
    return 0;
}

/* 读启动器写的结果文件（读到返回 0 并删除文件） */
static int audio_read_restart_result(char *out, size_t outlen)
{
    FILE *f;
    size_t l;

    if (outlen) out[0] = '\0';
    f = fopen(AUDIO_RESULT_FILE, "rb");
    if (f == NULL) return -1;
    if (fgets(out, (int)outlen, f) == NULL && outlen) out[0] = '\0';
    fclose(f);
    remove(AUDIO_RESULT_FILE);
    l = strlen(out);
    while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = '\0';
    return 0;
}

#else /* 非 Windows：不存在自动切换；只留公共路径会调用到的两个空实现 */
static void audio_queue_restore(void) {}
static int audio_read_restart_result(char *out, size_t outlen)
{
    if (outlen) out[0] = '\0';
    return -1;
}

#endif /* _WIN32 */

static int mpd_api_set_audio_device(const char *cmd, char *buffer)
{
#ifndef _WIN32
    (void)cmd;
    return audio_err(buffer, "audio output switching is not supported on this platform");
#else
    static const char prefix[] = "MPD_API_SET_AUDIO_DEVICE,";
    struct audio_output_info info;
    struct audio_device devs[AUDIO_DEVICE_MAX];
    char err[256];
    const char *id;
    int ndev, i, found = -1, qsaved;

    ndev = audio_devices_enumerate(&info, devs, AUDIO_DEVICE_MAX);

    if (strncmp(cmd, prefix, sizeof(prefix) - 1) != 0 || cmd[sizeof(prefix) - 1] == '\0')
        return audio_err(buffer, "malformed audio device command");
    id = cmd + sizeof(prefix) - 1;

    for (i = 0; i < ndev; i++) {
        if (strcmp(devs[i].id, id) == 0) { found = i; break; }
    }
    if (found < 0)
        return audio_err(buffer, "unknown audio device");
    if (!info.can_set)
        return audio_err(buffer, "the configured mpd audio_output is not managed by this build");

    /* 先把设备试开一次：失败就别写配置，否则 mpd 重启后会起不来 */
    if (audio_device_check(&devs[found], err, sizeof(err)) != 0)
        return audio_err(buffer, err);

    /* 队列在 mpd 侧，重启 mpd 会丢 —— 先存一份，重连后自动恢复 */
    qsaved = (mpd.conn_state == MPD_CONNECTED) ? audio_queue_save() : -1;

    if (audio_conf_set_device("mpd.conf", devs[found].arg, err, sizeof(err)) != 0)
        return audio_err(buffer, err);

    if (audio_write_restart_request(devs[found].id) != 0)
        return audio_err(buffer, "cannot write .mpd/restart-request (is winaurmpd.exe running here?)");

    s_audio_restart_pending = 1;
    s_audio_restart_deadline = time(NULL) + AUDIO_RESTART_TIMEOUT;
    fprintf(stderr, "AUDIO: restart requested (device %s, queue %s)\n",
            devs[found].id, qsaved == 0 ? "saved" : "NOT saved");

    /* 回复应用后的实际状态（读回 mpd.conf） */
    return mpd_api_get_audio_devices(buffer);
#endif
}

/* 内部队列与 mpd 的同步状态：首次连接后（s_queue_synced==0）强制同步一次，
 * 之后只在 mpd queue_version 变化时同步（外部 mpc / 其它客户端的改动）。 */
static int s_queue_synced = 0;
static unsigned s_queue_version_synced = 0;
/* forward declaration */
static void mpd_notify_callback(struct thread_data *p);

const char * mpd_cmd_strs[] = {
    MPD_CMDS(GEN_STR)
};

char * get_arg1 (char *p) {
	return strchr(p, ',') + 1;
}

char * get_arg2 (char *p) {
	return get_arg1(get_arg1(p));
}

static inline enum mpd_cmd_ids get_cmd_id(char *cmd)
{
    for(int i = 0; i < sizeof(mpd_cmd_strs)/sizeof(mpd_cmd_strs[0]); i++)
        if(!strncmp(cmd, mpd_cmd_strs[i], strlen(mpd_cmd_strs[i])))
            return i;

    return -1;
}

/*add by luping */
void mg_ws_send_error(struct thread_data *p,const char *buf)
{
    size_t n;

    if(buf){
        n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"error\",\"data\":\"%s\"}", 
            buf);
        mg_wakeup(p->mgr, p->conn_id, mpd.buf, n);
    }
}
/*end of add by luping*/
void callback_mpd(struct mg_connection *c,struct mg_ws_message *wm)
{
    enum mpd_cmd_ids cmd_id = get_cmd_id(wm->data.buf);
    size_t n = 0;
    unsigned int uint_buf, uint_buf_2;
    int int_buf;
    char *p_charbuf = NULL, *token;
    struct thread_data *p;

    if(cmd_id == -1)
        return;

    /* Serialise against the 1 Hz poll thread (and HTTP handlers) which use the
     * same mpd.conn: libmpdclient connections are not thread safe. */
    mpd_lock();

    if(mpd.conn_state != MPD_CONNECTED && cmd_id != MPD_API_SET_MPDHOST &&
        cmd_id != MPD_API_GET_MPDHOST && cmd_id != MPD_API_SET_MPDPASS &&
        cmd_id != MPD_API_GET_DIRBLEAPITOKEN &&
        cmd_id != MPD_API_GET_AUTOSTART && cmd_id != MPD_API_SET_AUTOSTART &&
        cmd_id != MPD_API_GET_AUDIO_DEVICES && cmd_id != MPD_API_SET_AUDIO_DEVICE) {
        mpd_unlock();
        return;
    }

    switch(cmd_id)
    {
        case MPD_API_UPDATE_DB:
            mpd_run_update(mpd.conn, NULL);
            break;
        case MPD_API_SET_PAUSE:
            mpd_run_toggle_pause(mpd.conn);
            break;
        case MPD_API_SET_PREV:
            mpd_run_previous(mpd.conn);
            break;
        case MPD_API_SET_NEXT:
            mpd_run_next(mpd.conn);
            break;
        case MPD_API_SET_PLAY:
            mpd_run_play(mpd.conn);
            break;
        case MPD_API_SET_STOP:
            mpd_run_stop(mpd.conn);
            break;
        case MPD_API_RM_ALL:
            mpd_run_clear(mpd.conn);
            break;
        case MPD_API_RM_TRACK:
            if(sscanf(wm->data.buf, "MPD_API_RM_TRACK,%u", &uint_buf))
                mpd_run_delete_id(mpd.conn, uint_buf);
            break;
        case MPD_API_RM_RANGE:
            if(sscanf(wm->data.buf, "MPD_API_RM_RANGE,%u,%u", &uint_buf, &uint_buf_2))
                mpd_run_delete_range(mpd.conn, uint_buf, uint_buf_2);
            break;
        case MPD_API_MOVE_TRACK:
            if (sscanf(wm->data.buf, "MPD_API_MOVE_TRACK,%u,%u", &uint_buf, &uint_buf_2) == 2)
            {
                uint_buf -= 1;
                uint_buf_2 -= 1;
                mpd_run_move(mpd.conn, uint_buf, uint_buf_2);
            }
            break;
        case MPD_API_PLAY_TRACK:
            if(sscanf(wm->data.buf, "MPD_API_PLAY_TRACK,%u", &uint_buf))
                mpd_run_play_id(mpd.conn, uint_buf);
                //mpd_run_play_pos(mpd.conn, uint_buf);
            break;
        case MPD_API_TOGGLE_RANDOM:
            if(sscanf(wm->data.buf, "MPD_API_TOGGLE_RANDOM,%u", &uint_buf))
                mpd_run_random(mpd.conn, uint_buf);
            break;
        case MPD_API_TOGGLE_REPEAT:
            if(sscanf(wm->data.buf, "MPD_API_TOGGLE_REPEAT,%u", &uint_buf))
                mpd_run_repeat(mpd.conn, uint_buf);
            break;
        case MPD_API_TOGGLE_CONSUME:
            if(sscanf(wm->data.buf, "MPD_API_TOGGLE_CONSUME,%u", &uint_buf))
                mpd_run_consume(mpd.conn, uint_buf);
            break;
        case MPD_API_TOGGLE_SINGLE:
            if(sscanf(wm->data.buf, "MPD_API_TOGGLE_SINGLE,%u", &uint_buf))
                mpd_run_single(mpd.conn, uint_buf);
            break;
        case MPD_API_TOGGLE_CROSSFADE:
            if(sscanf(wm->data.buf, "MPD_API_TOGGLE_CROSSFADE,%u", &uint_buf))
                mpd_run_crossfade(mpd.conn, uint_buf);
            break;
        case MPD_API_GET_OUTPUTS:
            mpd.buf_size = mpd_put_outputs(mpd.buf, 1);
            mpd_notify_callback(c->fn_data);
            break;
        case MPD_API_TOGGLE_OUTPUT:
            if (sscanf(wm->data.buf, "MPD_API_TOGGLE_OUTPUT,%u,%u", &uint_buf, &uint_buf_2)) {
                if (uint_buf_2)
                    mpd_run_enable_output(mpd.conn, uint_buf);
                else
                    mpd_run_disable_output(mpd.conn, uint_buf);
            }
            break;
        case MPD_API_SET_VOLUME:
            if(sscanf(wm->data.buf, "MPD_API_SET_VOLUME,%ud", &uint_buf) && uint_buf <= 100)
                mpd_run_set_volume(mpd.conn, uint_buf);
            break;
        case MPD_API_SET_SEEK:
            if(sscanf(wm->data.buf, "MPD_API_SET_SEEK,%u,%u", &uint_buf, &uint_buf_2))
                mpd_run_seek_id(mpd.conn, uint_buf, uint_buf_2);
            break;
        case MPD_API_GET_QUEUE:
            if(sscanf(wm->data.buf, "MPD_API_GET_QUEUE,%u", &uint_buf))
                n = mpd_put_queue(mpd.buf, uint_buf);
            break;
        case MPD_API_GET_BROWSE:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_GET_BROWSE"))
                goto out_browse;

            uint_buf = strtoul(strtok(NULL, ","), NULL, 10);
            if((token = strtok(NULL, ",")) == NULL)
                goto out_browse;

			free(p_charbuf);
            p_charbuf = strdup(wm->data.buf);
            n = mpd_put_browse(mpd.buf, get_arg2(p_charbuf), uint_buf);
out_browse:
			free(p_charbuf);
            break;
        case MPD_API_ADD_TRACK:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_ADD_TRACK"))
                goto out_add_track;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_add_track;

			free(p_charbuf);
            p_charbuf = strdup(wm->data.buf);
            mpd_run_add(mpd.conn, get_arg1(p_charbuf));
out_add_track:
            free(p_charbuf);
            break;           
        case MPD_API_ADD_PLAY_TRACK:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_ADD_PLAY_TRACK"))
                goto out_play_track;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_play_track;

			free(p_charbuf);
            p_charbuf = strdup(wm->data.buf);
            int_buf = mpd_run_add_id(mpd.conn, get_arg1(p_charbuf));
            if(int_buf != -1)
                mpd_run_play_id(mpd.conn, int_buf);
out_play_track:
            free(p_charbuf);
            break;
        /* Load a stored playlist into the queue (mpd: replace queue) */
        case MPD_API_ADD_PLAYLIST:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_ADD_PLAYLIST"))
                goto out_playlist;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_playlist;

			free(p_charbuf);
            p_charbuf = strdup(wm->data.buf);
            mpd_run_load(mpd.conn, get_arg1(p_charbuf));
out_playlist:
            free(p_charbuf);
            p_charbuf = NULL;
            break;
        /* Save the current queue as a stored playlist; answer with the fresh list */
        case MPD_API_SAVE_QUEUE:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_SAVE_QUEUE"))
                goto out_save_queue;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_save_queue;

			free(p_charbuf);
            p_charbuf = strdup(wm->data.buf);
            if(mpd_run_save(mpd.conn, get_arg1(p_charbuf)))
                n = mpd_put_playlists(mpd.buf);
            else {
                n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"error\", \"data\": \"%s\"}",
                    mpd_connection_get_error_message(mpd.conn));
                mpd_connection_clear_error(mpd.conn);
            }
out_save_queue:
            free(p_charbuf);
            p_charbuf = NULL;
            break;
        /* List stored playlists (mpd playlist directory) */
        case MPD_API_GET_PLAYLISTS:
            n = mpd_put_playlists(mpd.buf);
            break;
        /* Read the songs of one stored playlist */
        case MPD_API_GET_PLAYLIST_SONGS:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_GET_PLAYLIST_SONGS"))
                goto out_playlist_songs;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_playlist_songs;

			free(p_charbuf);
            p_charbuf = strdup(get_arg1(wm->data.buf));
            n = mpd_put_playlist_songs(mpd.buf, p_charbuf);
out_playlist_songs:
            free(p_charbuf);
            p_charbuf = NULL;
            break;
        /* Delete a stored playlist, then answer with the fresh list */
        case MPD_API_RM_PLAYLIST:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_RM_PLAYLIST"))
                goto out_rm_playlist;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_rm_playlist;

			free(p_charbuf);
            p_charbuf = strdup(wm->data.buf);
            if(mpd_run_rm(mpd.conn, get_arg1(p_charbuf)))
                n = mpd_put_playlists(mpd.buf);
            else {
                n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"error\", \"data\": \"%s\"}",
                    mpd_connection_get_error_message(mpd.conn));
                mpd_connection_clear_error(mpd.conn);
            }
out_rm_playlist:
            free(p_charbuf);
            p_charbuf = NULL;
            break;
        case MPD_API_SEARCH:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_SEARCH"))
				goto out_search;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_search;

			free(p_charbuf);
            p_charbuf = strdup(wm->data.buf);
            n = mpd_search(mpd.buf, get_arg1(p_charbuf));
out_search:
            free(p_charbuf);
            break;
        case MPD_API_SEND_MESSAGE:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_SEND_MESSAGE"))
				goto out_send_message;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_send_message;

			free(p_charbuf);
            p_charbuf = strdup(get_arg1(wm->data.buf));

            if ( strtok(p_charbuf, ",") == NULL )
                goto out_send_message;

            if ( (token = strtok(NULL, ",")) == NULL )
                goto out_send_message;

			mpd_run_send_message(mpd.conn, p_charbuf, token);
out_send_message:
            free(p_charbuf);
            break;
#ifdef WITH_MPD_HOST_CHANGE
        /* Commands allowed when disconnected from MPD server */
        case MPD_API_SET_MPDHOST:
            int_buf = 0;
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_SET_MPDHOST"))
                goto out_host_change;

            if((int_buf = strtol(strtok(NULL, ","), NULL, 10)) <= 0)
                goto out_host_change;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_host_change;

            strncpy(mpd.host, token, sizeof(mpd.host));
            mpd.port = int_buf;
            mpd.conn_state = MPD_RECONNECT;
            free(p_charbuf);
out_host_change:
            free(p_charbuf);
            break;
        case MPD_API_GET_MPDHOST:
            n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"mpdhost\", \"data\": "
                "{\"host\" : \"%s\", \"port\": \"%d\", \"passwort_set\": %s}"
                "}", mpd.host, mpd.port, mpd.password ? "true" : "false");
            break;
        case MPD_API_GET_DIRBLEAPITOKEN:
            n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"dirbleapitoken\", \""
                "data\": \"%s\"}", dirble_api_token);
            break;
        case MPD_API_SET_MPDPASS:
            p_charbuf = strdup(wm->data.buf);
            if(strcmp(strtok(p_charbuf, ","), "MPD_API_SET_MPDPASS"))
                goto out_set_pass;

            if((token = strtok(NULL, ",")) == NULL)
                goto out_set_pass;

            if(mpd.password)
                free(mpd.password);

            mpd.password = strdup(token);
            mpd.conn_state = MPD_RECONNECT;
            free(p_charbuf);
out_set_pass:
            free(p_charbuf);
            break;
#endif
        /* 开机自启：不依赖 mpd 连接，任何时候都要能读写（前端设置项） */
        case MPD_API_GET_AUTOSTART:
            n = mpd_put_autostart(mpd.buf, autostart_get_enabled());
            break;
        case MPD_API_SET_AUTOSTART:
        {
            /* 形如 "MPD_API_SET_AUTOSTART,1" / "MPD_API_SET_AUTOSTART,0" */
            static const char prefix[] = "MPD_API_SET_AUTOSTART,";
            int enable = 0;

            if (strncmp(wm->data.buf, prefix, sizeof(prefix) - 1) != 0 ||
                wm->data.buf[sizeof(prefix) - 1] == '\0') {
                fprintf(stderr, "AUTOSTART: malformed command, ignoring\n");
            } else {
                enable = (wm->data.buf[sizeof(prefix) - 1] == '1');
                if (autostart_set_enabled(enable) != 0)
                    fprintf(stderr, "AUTOSTART: failed to %s autostart\n",
                            enable ? "enable" : "disable");
            }
            /* 无论成功与否，都回复写入后的实际状态，让 UI 能立即确认结果 */
            n = mpd_put_autostart(mpd.buf, autostart_get_enabled());
            break;
        }
        /* 音频输出设备：与 mpd 连接无关，设置页任何时候都要能查（见文件上部说明） */
        case MPD_API_GET_AUDIO_DEVICES:
            n = mpd_api_get_audio_devices(mpd.buf);
            break;
        case MPD_API_SET_AUDIO_DEVICE:
            n = mpd_api_set_audio_device(wm->data.buf, mpd.buf);
            break;
    }

    if(mpd.conn_state == MPD_CONNECTED && mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS)
    {
        n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"error\", \"data\": \"%s\"}", 
            mpd_connection_get_error_message(mpd.conn));

        /* Try to recover error */
        if (!mpd_connection_clear_error(mpd.conn))
            mpd.conn_state = MPD_FAILURE;
    }

    mpd_unlock();

    if(n > 0){
        //mg_ws_send(c, mpd.buf, n, WEBSOCKET_OP_TEXT);
        p = (struct thread_data *)c->fn_data;
        mg_wakeup(p->mgr, p->conn_id, mpd.buf, n);
    }
    return;
}

int mpd_close_handler(struct mg_connection *c)
{
    /* Cleanup session data */
    if(c->fn_data)
        free(c->fn_data);
    return 0;
}

static void mpd_notify_callback(struct thread_data *p) {
    size_t n;
    
    if(mpd.conn_state != MPD_CONNECTED) {
        n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"disconnected\"}");
        mg_wakeup(p->mgr, p->conn_id, mpd.buf, n);  // Send to parent
    }
    else{
        mg_wakeup(p->mgr, p->conn_id, mpd.buf, mpd.buf_size);  //之前mpd命令的response
        if(p->mpd_cs.song_id != mpd.song_id) //song_change
        {
            n = mpd_put_current_song(mpd.buf);
            mg_wakeup(p->mgr, p->conn_id, mpd.buf, n);
            p->mpd_cs.song_id = mpd.song_id;
        }
        if(p->mpd_cs.queue_version != mpd.queue_version) //update_queue
        {
            n = snprintf(mpd.buf, MAX_SIZE, "{\"type\":\"update_queue\"}");
            mg_wakeup(p->mgr, p->conn_id, mpd.buf, n);
            p->mpd_cs.queue_version = mpd.queue_version;
        }        
    }
}

void mpd_poll(struct thread_data *p)
{
    const char * buf;

    /* 音频输出切换：请求启动器重启 mpd 后，如果超过超时时间仍未连上，
     * 回一条可见错误（不静默）。这段不碰 mpd.conn，放在取锁之前。 */
    if (s_audio_restart_pending && time(NULL) >= s_audio_restart_deadline) {
        s_audio_restart_pending = 0;
        if (mpd.conn_state != MPD_CONNECTED) {
            char detail[AUDIO_ID_MAX];
            char msg[256];
            size_t an;

            if (audio_read_restart_result(detail, sizeof(detail)) != 0)
                snprintf(detail, sizeof(detail), "launcher did not respond");
            snprintf(msg, sizeof(msg), "audio output switch did not take effect: %s", detail);
            fprintf(stderr, "AUDIO: %s\n", msg);
            mpd_lock();
            an = audio_err(mpd.buf, msg);
            mpd_unlock();
            if (p != NULL)
                mg_wakeup(p->mgr, p->conn_id, mpd.buf, an);
        }
    }

    mpd_lock();
    switch (mpd.conn_state) {
        case MPD_DISCONNECTED:
            /* Try to connect */
            fprintf(stdout, "MPD Connecting to %s:%d\n", mpd.host, mpd.port);
            mpd.conn = mpd_connection_new(mpd.host, mpd.port, 3000);
            if (mpd.conn == NULL) {
                fprintf(stderr, "Out of memory.");
                mpd.conn_state = MPD_FAILURE;
                /* 不能直接 return：这里持有 mpd_lock，直接返回会让锁永远不释放，
                   后续 mpd_clear_all 会永久阻塞，进程退不掉（残留进程占着目录）。 */
                goto out;
            }

            if (mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS) {
                fprintf(stderr, "MPD connection: %s\n", mpd_connection_get_error_message(mpd.conn));
                buf = mpd_connection_get_error_message(mpd.conn);
                mg_ws_send_error(p,buf);
                mpd.conn_state = MPD_FAILURE;
                goto out;
            }

            if(mpd.password && !mpd_run_password(mpd.conn, mpd.password))
            {
                fprintf(stderr, "MPD connection: %s\n", mpd_connection_get_error_message(mpd.conn));
                buf = mpd_connection_get_error_message(mpd.conn);
                mg_ws_send_error(p,buf);
                mpd.conn_state = MPD_FAILURE;
                goto out;
            }

            fprintf(stderr, "MPD connected.\n");
            mpd_connection_set_timeout(mpd.conn, 10000);
            mpd.conn_state = MPD_CONNECTED;

            /* 若刚才是为了切换音频输出而重启 mpd：先把队列恢复回来（mpd 重启
             * 会丢队列），再把新的输出状态广播出去，让所有客户端校准。 */
            audio_queue_restore();
            if (s_audio_restart_pending) {
                char result[AUDIO_ID_MAX];
                size_t an;

                s_audio_restart_pending = 0;
                audio_read_restart_result(result, sizeof(result));
                fprintf(stderr, "AUDIO: mpd restarted, new output state broadcast (result=%s)\n",
                        result[0] ? result : "?");
                if (p != NULL) {
                    an = mpd_api_get_audio_devices(mpd.buf);
                    mg_wakeup(p->mgr, p->conn_id, mpd.buf, an);
                }
            }

            /* 连接/重连成功后立刻用 mpd 的真实队列重建内部队列。否则 aurmpd
             * 重启后内部队列为空，GET /api/queue 返回空，前端看不到队列、也
             * 无法控制正在播放的曲目（用户反馈的「完全脱离掌控」）。 */
            queue_sync_from_mpd();
            s_queue_synced = 1;

            /* 曲库为空时自动触发一次全量扫描。mpd 只在 db_file 不存在时才会
             * 自动扫描；用户换掉 mpd.conf / music_directory 后旧 db_file 仍在，
             * 不显式 update 就永远读到旧（甚至空）曲库。这里只在「库为空」时
             * 扫，避免每次启动都为超大曲库做一次很慢的全量扫描。 */
            {
                struct mpd_stats *stats = mpd_run_stats(mpd.conn);
                if (stats == NULL) {
                    fprintf(stderr, "MPD stats: %s\n", mpd_connection_get_error_message(mpd.conn));
                    mpd_connection_clear_error(mpd.conn);
                } else {
                    unsigned num_songs = mpd_stats_get_number_of_songs(stats);
                    mpd_stats_free(stats);
                    if (num_songs == 0) {
                        fprintf(stderr, "MPD library is empty (0 songs), triggering full database update.\n");
                        if (!mpd_run_update(mpd.conn, NULL))
                            fprintf(stderr, "MPD update: %s\n", mpd_connection_get_error_message(mpd.conn));
                    }
                }
            }

            /* write outputs */
            mpd.buf_size = mpd_put_outputs(mpd.buf, 1);
            mpd_notify_callback(p);
            break;

        case MPD_FAILURE:
            fprintf(stderr, "MPD connection failed.\n");

        case MPD_DISCONNECT:
        case MPD_RECONNECT:
            if(mpd.conn != NULL)
                mpd_connection_free(mpd.conn);
            mpd.conn = NULL;
            mpd.conn_state = MPD_DISCONNECTED;
            break;

        case MPD_CONNECTED:
            mpd.buf_size = mpd_put_state(mpd.buf, &mpd.song_id, &mpd.queue_version);
            /* 队列版本变化（包括外部用 mpc / 其它客户端改动 mpd 队列）时，
             * 把 mpd 当前队列同步回内部队列，再广播 update_queue。 */
            if (mpd.conn_state == MPD_CONNECTED &&
                (!s_queue_synced || mpd.queue_version != s_queue_version_synced)) {
                queue_sync_from_mpd();
                s_queue_version_synced = mpd.queue_version;
                s_queue_synced = 1;
            }
            mpd_notify_callback(p);
            mpd.buf_size = mpd_put_outputs(mpd.buf, 0);
            mpd_notify_callback(p);
            break;
    }
    /* 唯一的解锁出口：所有失败路径都 goto 到这里，避免持锁返回造成死锁 */
out:
    mpd_unlock();
}

char* mpd_get_title(struct mpd_song const *song)
{
    char *str;

    str = (char *)mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
    if(str == NULL){
        str = basename((char *)mpd_song_get_uri(song));
    }

    return str;
}

char* mpd_get_artist(struct mpd_song const *song)
{
    char *str;

    str = (char *)mpd_song_get_tag(song, MPD_TAG_ARTIST, 0);
    if (str == NULL) {
	return "";
    } else {
	return str;
    }
}

char* mpd_get_album(struct mpd_song const *song)
{
    char *str;

    str = (char *)mpd_song_get_tag(song, MPD_TAG_ALBUM, 0);
    if (str == NULL) {
	return "";
    } else {
	return str;
    }
}

int mpd_put_state(char *buffer, int *current_song_id, unsigned *queue_version)
{
    struct mpd_status *status;
    int len;

    status = mpd_run_status(mpd.conn);
    if (!status) {
        fprintf(stderr, "MPD mpd_run_status: %s\n", mpd_connection_get_error_message(mpd.conn));
        mpd.conn_state = MPD_FAILURE;
        return 0;
    }

    len = snprintf(buffer, MAX_SIZE,
        "{\"type\":\"state\", \"data\":{"
        " \"state\":%d, \"volume\":%d, \"repeat\":%d,"
        " \"single\":%d, \"crossfade\":%d, \"consume\":%d, \"random\":%d, "
        " \"songpos\": %d, \"elapsedTime\": %d, \"totalTime\":%d, "
        " \"currentsongid\": %d,\"queueLength\": %d,\"queueVersion\": %d"
        "}}", 
        mpd_status_get_state(status),
        mpd_status_get_volume(status), 
        mpd_status_get_repeat(status),
        mpd_status_get_single(status),
        mpd_status_get_crossfade(status),
        mpd_status_get_consume(status),
        mpd_status_get_random(status),
        mpd_status_get_song_pos(status),
        mpd_status_get_elapsed_time(status),
        mpd_status_get_total_time(status),
        mpd_status_get_song_id(status),
        mpd_status_get_queue_length(status),
        mpd_status_get_queue_version(status));

    *current_song_id = mpd_status_get_song_id(status);
    *queue_version = mpd_status_get_queue_version(status);
    mpd_status_free(status);
    return len;
}

int mpd_put_outputs(char *buffer, int names)
{
    struct mpd_output *out;
    int nout;
    char *str, *strend;

    str = buffer;
    strend = buffer+MAX_SIZE;
    str += snprintf(str, strend-str, "{\"type\":\"%s\", \"data\":{",
            names ? "outputnames" : "outputs");

    mpd_send_outputs(mpd.conn);
    nout = 0;
    while ((out = mpd_recv_output(mpd.conn)) != NULL) {
        if (nout++)
            *str++ = ',';
        if (names)
            str += snprintf(str, strend - str, " \"%d\":\"%s\"",
                    mpd_output_get_id(out), mpd_output_get_name(out));
        else
            str += snprintf(str, strend-str, " \"%d\":%d",
                    mpd_output_get_id(out), mpd_output_get_enabled(out));
        mpd_output_free(out);
    }
    if (!mpd_response_finish(mpd.conn)) {
        fprintf(stderr, "MPD outputs: %s\n", mpd_connection_get_error_message(mpd.conn));
        mpd_connection_clear_error(mpd.conn);
        return 0;
    }
    str += snprintf(str, strend-str, " }}");
    return str-buffer;
}

int mpd_put_current_song(char *buffer)
{
    char *cur = buffer;
    const char *end = buffer + MAX_SIZE;
    struct mpd_song *song;

    song = mpd_run_current_song(mpd.conn);
    if(song == NULL)
        return 0;

    cur += json_emit_raw_str(cur, end - cur, "{\"type\": \"song_change\", \"data\":{\"pos\":");
    cur += json_emit_int(cur, end - cur, mpd_song_get_pos(song));
    cur += json_emit_raw_str(cur, end - cur, ",\"title\":");
    cur += json_emit_quoted_str(cur, end - cur, mpd_get_title(song));
    cur += json_emit_raw_str(cur, end - cur, ",\"artist\":");
    cur += json_emit_quoted_str(cur, end - cur, mpd_get_artist(song));
    cur += json_emit_raw_str(cur, end - cur, ",\"album\":");
    cur += json_emit_quoted_str(cur, end - cur, mpd_get_album(song));

    cur += json_emit_raw_str(cur, end - cur, "}}");
    mpd_song_free(song);
    mpd_response_finish(mpd.conn);

    return cur - buffer;
}

int mpd_put_queue(char *buffer, unsigned int offset)
{
    char *cur = buffer;
    const char *end = buffer + MAX_SIZE;
    struct mpd_entity *entity;

    if (!mpd_send_list_queue_range_meta(mpd.conn, offset, offset+MAX_ELEMENTS_PER_PAGE))
        RETURN_ERROR_AND_RECOVER("mpd_send_list_queue_meta");

    cur += json_emit_raw_str(cur, end  - cur, "{\"type\":\"queue\",\"data\":[ ");

    while((entity = mpd_recv_entity(mpd.conn)) != NULL) {
        const struct mpd_song *song;

        if(mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG) {
            song = mpd_entity_get_song(entity);

            cur += json_emit_raw_str(cur, end - cur, "{\"id\":");
            cur += json_emit_int(cur, end - cur, mpd_song_get_id(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"pos\":");
            cur += json_emit_int(cur, end - cur, mpd_song_get_pos(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"duration\":");
            cur += json_emit_int(cur, end - cur, mpd_song_get_duration(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"title\":");
            cur += json_emit_quoted_str(cur, end - cur, mpd_get_title(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"artist\":");
            cur += json_emit_quoted_str(cur, end - cur, mpd_get_artist(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"album\":");
            cur += json_emit_quoted_str(cur, end - cur, mpd_get_album(song));
            cur += json_emit_raw_str(cur, end - cur, "},");
        }
        mpd_entity_free(entity);
    }

    /* remove last ',' */
    cur--;

    cur += json_emit_raw_str(cur, end - cur, "]}");
    return cur - buffer;
}

int mpd_put_browse(char *buffer, char *path, unsigned int offset)
{
    char *cur = buffer;
    const char *end = buffer + MAX_SIZE;
    struct mpd_entity *entity;
    unsigned int entity_count = 0;

    if (!mpd_send_list_meta(mpd.conn, path))
        RETURN_ERROR_AND_RECOVER("mpd_send_list_meta");

    cur += json_emit_raw_str(cur, end  - cur, "{\"type\":\"browse\",\"data\":[ ");

    while((entity = mpd_recv_entity(mpd.conn)) != NULL) {
        const struct mpd_song *song;
        const struct mpd_directory *dir;
        const struct mpd_playlist *pl;

        if(offset > entity_count)
        {
            mpd_entity_free(entity);
            entity_count++;
            continue;
        }
        else if(offset + MAX_ELEMENTS_PER_PAGE - 1 < entity_count)
        {
            mpd_entity_free(entity);
            cur += json_emit_raw_str(cur, end  - cur, "{\"type\":\"wrap\",\"count\":");
            cur += json_emit_int(cur, end - cur, entity_count);
            cur += json_emit_raw_str(cur, end  - cur, "} ");
            break;
        }

        switch (mpd_entity_get_type(entity)) {
            case MPD_ENTITY_TYPE_UNKNOWN:
                break;

            case MPD_ENTITY_TYPE_SONG:
                song = mpd_entity_get_song(entity);
                cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"song\",\"uri\":");
                cur += json_emit_quoted_str(cur, end - cur, mpd_song_get_uri(song));
                cur += json_emit_raw_str(cur, end - cur, ",\"duration\":");
                cur += json_emit_int(cur, end - cur, mpd_song_get_duration(song));
                cur += json_emit_raw_str(cur, end - cur, ",\"title\":");
                cur += json_emit_quoted_str(cur, end - cur, mpd_get_title(song));
                cur += json_emit_raw_str(cur, end - cur, "},");
                break;

            case MPD_ENTITY_TYPE_DIRECTORY:
                dir = mpd_entity_get_directory(entity);

                cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"directory\",\"dir\":");
                cur += json_emit_quoted_str(cur, end - cur, mpd_directory_get_path(dir));
                cur += json_emit_raw_str(cur, end - cur, "},");
                break;

            case MPD_ENTITY_TYPE_PLAYLIST:
                pl = mpd_entity_get_playlist(entity);
                cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"playlist\",\"plist\":");
                cur += json_emit_quoted_str(cur, end - cur, mpd_playlist_get_path(pl));
                cur += json_emit_raw_str(cur, end - cur, "},");
                break;
        }
        mpd_entity_free(entity);
        entity_count++;
    }

    if (mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS || !mpd_response_finish(mpd.conn)) {
        fprintf(stderr, "MPD mpd_send_list_meta: %s\n", mpd_connection_get_error_message(mpd.conn));
        mpd.conn_state = MPD_FAILURE;
        return 0;
    }

    /* remove last ',' */
    cur--;

    cur += json_emit_raw_str(cur, end - cur, "]}");
    return cur - buffer;
}

/* List the playlists stored in mpd's playlist directory.
 * Output: {"type":"playlists","data":[{"name":"x","lastmodified":<epoch seconds>},...]} */
int mpd_put_playlists(char *buffer)
{
    char *cur = buffer;
    const char *end = buffer + MAX_SIZE;
    struct mpd_playlist *pl;
    time_t last_modified;

    if (!mpd_send_list_playlists(mpd.conn))
        RETURN_ERROR_AND_RECOVER("mpd_send_list_playlists");

    cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"playlists\",\"data\":[ ");

    while ((pl = mpd_recv_playlist(mpd.conn)) != NULL) {
        last_modified = mpd_playlist_get_last_modified(pl);

        cur += json_emit_raw_str(cur, end - cur, "{\"name\":");
        cur += json_emit_quoted_str(cur, end - cur, mpd_playlist_get_path(pl));
        cur += json_emit_raw_str(cur, end - cur, ",\"lastmodified\":");
        cur += json_emit_int(cur, end - cur, (long int)last_modified);
        cur += json_emit_raw_str(cur, end - cur, "},");
        mpd_playlist_free(pl);
    }

    if (mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS || !mpd_response_finish(mpd.conn)) {
        fprintf(stderr, "MPD mpd_send_list_playlists: %s\n", mpd_connection_get_error_message(mpd.conn));
        mpd.conn_state = MPD_FAILURE;
        return 0;
    }

    /* remove last ',' (or the separating space when the list is empty) */
    cur--;

    cur += json_emit_raw_str(cur, end - cur, "]}");
    return cur - buffer;
}

/* Read the songs of one stored playlist.
 * Output: {"type":"playlist","data":{"name":"x","song":[{uri,pos,duration,title,artist,album},...]}} */
int mpd_put_playlist_songs(char *buffer, const char *name)
{
    char *cur = buffer;
    const char *end = buffer + MAX_SIZE;
    struct mpd_song *song;

    if (!mpd_send_list_playlist_meta(mpd.conn, name))
        RETURN_ERROR_AND_RECOVER("mpd_send_list_playlist_meta");

    cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"playlist\",\"data\":{\"name\":");
    cur += json_emit_quoted_str(cur, end - cur, name);
    cur += json_emit_raw_str(cur, end - cur, ",\"song\":[ ");

    while ((song = mpd_recv_song(mpd.conn)) != NULL) {
        cur += json_emit_raw_str(cur, end - cur, "{\"uri\":");
        cur += json_emit_quoted_str(cur, end - cur, mpd_song_get_uri(song));
        cur += json_emit_raw_str(cur, end - cur, ",\"pos\":");
        cur += json_emit_int(cur, end - cur, mpd_song_get_pos(song));
        cur += json_emit_raw_str(cur, end - cur, ",\"duration\":");
        cur += json_emit_int(cur, end - cur, mpd_song_get_duration(song));
        cur += json_emit_raw_str(cur, end - cur, ",\"title\":");
        cur += json_emit_quoted_str(cur, end - cur, mpd_get_title(song));
        cur += json_emit_raw_str(cur, end - cur, ",\"artist\":");
        cur += json_emit_quoted_str(cur, end - cur, mpd_get_artist(song));
        cur += json_emit_raw_str(cur, end - cur, ",\"album\":");
        cur += json_emit_quoted_str(cur, end - cur, mpd_get_album(song));
        cur += json_emit_raw_str(cur, end - cur, "},");
        mpd_song_free(song);
    }

    if (mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS || !mpd_response_finish(mpd.conn)) {
        fprintf(stderr, "MPD mpd_send_list_playlist_meta: %s\n", mpd_connection_get_error_message(mpd.conn));
        mpd.conn_state = MPD_FAILURE;
        return 0;
    }

    /* remove last ',' (or the separating space when the playlist is empty) */
    cur--;

    cur += json_emit_raw_str(cur, end - cur, "]}}");
    return cur - buffer;
}

int mpd_search(char *buffer, char *searchstr)
{
    int i = 0;
    char *cur = buffer;
    const char *end = buffer + MAX_SIZE;
    struct mpd_song *song;

    if(mpd_search_db_songs(mpd.conn, false) == false)
        RETURN_ERROR_AND_RECOVER("mpd_search_db_songs");
    else if(mpd_search_add_any_tag_constraint(mpd.conn, MPD_OPERATOR_DEFAULT, searchstr) == false)
        RETURN_ERROR_AND_RECOVER("mpd_search_add_any_tag_constraint");
    else if(mpd_search_commit(mpd.conn) == false)
        RETURN_ERROR_AND_RECOVER("mpd_search_commit");
    else {
        cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"search\",\"data\":[ ");

        while((song = mpd_recv_song(mpd.conn)) != NULL) {
            cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"song\",\"uri\":");
            cur += json_emit_quoted_str(cur, end - cur, mpd_song_get_uri(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"duration\":");
            cur += json_emit_int(cur, end - cur, mpd_song_get_duration(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"title\":");
            cur += json_emit_quoted_str(cur, end - cur, mpd_get_title(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"artist\":");
            cur += json_emit_quoted_str(cur, end - cur, mpd_get_artist(song));
            cur += json_emit_raw_str(cur, end - cur, ",\"album\":");
            cur += json_emit_quoted_str(cur, end - cur, mpd_get_album(song));
            cur += json_emit_raw_str(cur, end - cur, "},");
            mpd_song_free(song);

            /* Maximum results */
            if(i++ >= 300)
            {
                cur += json_emit_raw_str(cur, end - cur, "{\"type\":\"wrap\"},");
                break;
            }
        }

        /* remove last ',' */
        cur--;

        cur += json_emit_raw_str(cur, end - cur, "]}");
    }
    return cur - buffer;
}


void mpd_disconnect()
{
    mpd.conn_state = MPD_DISCONNECT;
    mpd_poll(NULL);
}

void mpd_clear_all()
{
    mpd_lock();
    if (mpd.conn != NULL) {
        // 标记响应接收结束
        //mpd_response_finish(mpd.conn);
        // 停止播放
        if (!mpd_run_stop(mpd.conn)) {
            fprintf(stderr, "Failed to stop playback: %s\n", mpd_connection_get_error_message(mpd.conn));
        }
        // 清空播放队列
        if (!mpd_run_clear(mpd.conn)) {
            fprintf(stderr, "Failed to clear playlist: %s\n", mpd_connection_get_error_message(mpd.conn));
        }
        // 刷新数据库
        if (!mpd_run_update(mpd.conn, NULL)) {
            fprintf(stderr, "Failed to update database: %s\n", mpd_connection_get_error_message(mpd.conn));
        }
        // 清除错误信息
        if (!mpd_run_clearerror(mpd.conn)) {
            fprintf(stderr, "Failed to clear error: %s\n", mpd_connection_get_error_message(mpd.conn));
        }
        mpd_disconnect();
    }else
        fprintf(stderr, "mpd.conn is NULL\n");
    mpd_unlock();
}
