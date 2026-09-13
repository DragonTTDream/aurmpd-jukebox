/*
 * aurmpd —— 音频输出设备检测 / mpd.conf 读写（实现）
 * 见 audio_devices.h 顶部的设计说明。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <unistd.h>
#endif

#include "audio_devices.h"
#include "json_encode.h"

/* JSON 累加辅助：buf 满时安全停住（返回已用长度，不再写） */
static int jr(char *buf, int bufsize, int n, const char *raw)
{
    if (n < 0) n = 0;
    if (n >= bufsize) return n;
    return n + json_emit_raw_str(buf + n, bufsize - n, raw);
}

static int jq(char *buf, int bufsize, int n, const char *str)
{
    if (n < 0) n = 0;
    if (n >= bufsize) return n;
    return n + json_emit_quoted_str(buf + n, bufsize - n, str);
}

int audio_devices_json(char *buf, int bufsize,
                       const struct audio_output_info *info,
                       const struct audio_device *devs, int ndevs)
{
    int n = 0, i;

    if (bufsize <= 0) return 0;

    n = jr(buf, bufsize, n, info->supported
        ? "{\"type\":\"audio_devices\",\"data\":{\"supported\":true,\"current\":"
        : "{\"type\":\"audio_devices\",\"data\":{\"supported\":false,\"current\":");
    n = jq(buf, bufsize, n, info->current);
    n = jr(buf, bufsize, n, info->can_set ? ",\"canSet\":true,\"devices\":["
                                          : ",\"canSet\":false,\"devices\":[");
    for (i = 0; i < ndevs; i++) {
        n = jr(buf, bufsize, n, "{\"id\":");
        n = jq(buf, bufsize, n, devs[i].id);
        n = jr(buf, bufsize, n, ",\"name\":");
        n = jq(buf, bufsize, n, devs[i].name);
        n = jr(buf, bufsize, n, ",\"kind\":");
        n = jq(buf, bufsize, n, devs[i].kind);
        n = jr(buf, bufsize, n, "}");
        if (i + 1 < ndevs) n = jr(buf, bufsize, n, ",");
    }
    n = jr(buf, bufsize, n, "]}}");
    return n;
}

/* ==================================================================
 * 平台无关的小工具
 * ================================================================== */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
}

static int key_at(const char *line, const char *key, const char **rest)
{
    size_t klen = strlen(key);
    const char *p = skip_ws(line);
    if (strncmp(p, key, klen) != 0) return 0;
    if (p[klen] != ' ' && p[klen] != '\t' && p[klen] != '\0') return 0;
    if (rest) *rest = p + klen;
    return 1;
}

/* 去掉行内注释与引号内容后的「裸行」，用于识别 key / 统计大括号深度；
 * 引号内的字符整体丢弃（其中包括可能出现的 '{' '}' '#'）。
 * *delta 是这一行的净深度变化：'{' 加一，'}' 减一。 */
static void bare_of(const char *line, size_t len, char *out, size_t outlen, int *delta)
{
    size_t i, o = 0;
    int q = 0;

    if (delta) *delta = 0;
    for (i = 0; i < len; i++) {
        char c = line[i];
        if (c == '"') { q = !q; continue; }
        if (q) continue;
        if (c == '#') break;
        if (c == '{') { if (delta) (*delta)++; }
        else if (c == '}') { if (delta) (*delta)--; }
        if (outlen && o + 1 < outlen) out[o++] = c;
    }
    if (outlen) out[o] = '\0';
}

/* 从原始行里取 key 的值（支持 "值" 与裸值两种写法），值可能为空串 */
static void extract_value(const char *line, const char *key, char *out, size_t outlen)
{
    const char *rest = NULL;
    size_t o = 0;

    out[0] = '\0';
    if (!key_at(line, key, &rest)) return;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (*rest == '"') {
        rest++;
        while (*rest != '\0' && *rest != '"' && *rest != '\n' && *rest != '\r') {
            if (o + 1 < outlen) out[o++] = *rest;
            rest++;
        }
    } else {
        while (*rest != '\0' && *rest != '\n' && *rest != '\r' &&
               *rest != '#' && *rest != ' ' && *rest != '\t') {
            if (o + 1 < outlen) out[o++] = *rest;
            rest++;
        }
    }
    out[o] = '\0';
}

/* 读整个文件；成功返回 0，*out 是 malloc 出来的、以 NUL 结尾的内容 */
static int read_file(const char *path, char **out, long *outlen)
{
    FILE *f;
    long sz;
    size_t rd;
    char *data;

    *out = NULL;
    if (outlen) *outlen = 0;
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) { fclose(f); return -1; }
    rewind(f);
    data = (char *)malloc((size_t)sz + 1);
    if (data == NULL) { fclose(f); return -1; }
    rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[rd] = '\0';
    *out = data;
    if (outlen) *outlen = (long)rd;
    return 0;
}

/* 原子替换写文件：先写 <path>.tmp，再替换目标。
 * POSIX rename() 覆盖已存在文件是原子的；Windows 上用 MoveFileEx(REPLACE_EXISTING)。 */
static int replace_file(const char *path, const char *data, size_t len, char *err, size_t errlen)
{
    char tmp[1024];
    FILE *f;
    int rc = 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL) {
        snprintf(err, errlen, "cannot open %s for writing", tmp);
        return -1;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        snprintf(err, errlen, "cannot write %s", tmp);
        rc = -1;
    }
    if (fclose(f) != 0 && rc == 0) {
        snprintf(err, errlen, "cannot flush %s", tmp);
        rc = -1;
    }
    if (rc != 0) {
        remove(tmp);
        return -1;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(err, errlen, "cannot replace %s", path);
        remove(tmp);
        return -1;
    }
#else
    if (rename(tmp, path) != 0) {
        snprintf(err, errlen, "cannot replace %s", path);
        remove(tmp);
        return -1;
    }
#endif
    return 0;
}

/* 第一个 audio_output 块的扫描结果 */
struct conf_block {
    int have_block;       /* 找到 audio_output 块 */
    int have_device;      /* 块内写了 device */
    int have_type;
    char type[16];
    char device[AUDIO_ID_MAX];
    char indent[16];      /* device 行（或 type 行）的缩进，写回时保持风格 */
    size_t device_start;  /* device 行起点（have_device 时有效） */
    size_t device_end;    /* device 行末尾（含换行） */
    size_t close_start;   /* 块的 '}' 所在行起点（插入 device 的位置） */
};

/* 扫描第一个 audio_output 块。data 必须以 NUL 结尾。 */
static void scan_conf(const char *data, struct conf_block *b)
{
    const char *p = data;
    int in_block = 0, depth = 0;

    memset(b, 0, sizeof(*b));
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        size_t line_end = llen + (nl ? 1 : 0);
        char bare[1024];
        int delta = 0;

        bare_of(p, llen, bare, sizeof(bare), &delta);

        if (!in_block) {
            const char *t = skip_ws(bare);
            if (strncmp(t, "audio_output", 12) == 0 &&
                (t[12] == '\0' || t[12] == ' ' || t[12] == '\t' || t[12] == '{')) {
                in_block = 1;
                b->have_block = 1;
                depth += delta;
                if (depth <= 0) {
                    /* 空块（audio_output {}）：'}' 就在这一行 */
                    b->close_start = (size_t)(p - data);
                    return;
                }
            }
        } else {
            const char *t = skip_ws(bare);
            const char *ind = p;
            size_t indlen = (size_t)(t - bare); /* 裸行的缩进 == 原始行缩进（无引号时一致） */

            depth += delta;
            if (key_at(t, "type", NULL) && !b->have_type) {
                extract_value(p, "type", b->type, sizeof(b->type));
                b->have_type = 1;
                if (indlen < sizeof(b->indent)) {
                    memcpy(b->indent, ind, indlen);
                    b->indent[indlen] = '\0';
                }
            }
            if (key_at(t, "device", NULL) && !b->have_device) {
                extract_value(p, "device", b->device, sizeof(b->device));
                b->have_device = 1;
                b->device_start = (size_t)(p - data);
                b->device_end = b->device_start + line_end;
                if (indlen < sizeof(b->indent)) {
                    memcpy(b->indent, ind, indlen);
                    b->indent[indlen] = '\0';
                }
            }
            if (depth <= 0) {
                b->close_start = (size_t)(p - data);
                return; /* 只看第一个块 */
            }
        }

        if (nl == NULL) break;
        p = nl + 1;
    }
}

int audio_conf_type(const char *conf_path, char *out, size_t outlen)
{
    char *data = NULL;
    struct conf_block b;

    if (outlen) out[0] = '\0';
    if (read_file(conf_path, &data, NULL) != 0) return -1;
    scan_conf(data, &b);
    free(data);
    if (!b.have_block) return -1;
    if (outlen) snprintf(out, outlen, "%s", b.have_type ? b.type : "");
    return 0;
}

int audio_conf_current(const char *conf_path, char *out, size_t outlen)
{
    char *data = NULL;
    struct conf_block b;

    if (outlen) out[0] = '\0';
    if (read_file(conf_path, &data, NULL) != 0) return -1;
    scan_conf(data, &b);
    free(data);
    if (!b.have_block) return -1;
    if (outlen) snprintf(out, outlen, "%s", b.have_device ? b.device : "");
    return 0;
}

int audio_conf_set_device(const char *conf_path, const char *arg,
                          char *err, size_t errlen)
{
    char *data = NULL;
    long len = 0;
    struct conf_block b;
    char *out = NULL;
    size_t cap, olen = 0;
    const char *indent;
    int rc = -1;

    if (errlen) err[0] = '\0';
    if (read_file(conf_path, &data, &len) != 0) {
        snprintf(err, errlen, "cannot read %s", conf_path);
        return -1;
    }
    scan_conf(data, &b);
    if (!b.have_block) {
        snprintf(err, errlen, "no audio_output block in %s", conf_path);
        free(data);
        return -1;
    }

    indent = b.indent[0] ? b.indent : "    ";
    cap = (size_t)len + 512;
    out = (char *)malloc(cap);
    if (out == NULL) {
        snprintf(err, errlen, "out of memory");
        free(data);
        return -1;
    }

    if (b.have_device) {
        /* 保留 device 行之前的内容 */
        memcpy(out, data, b.device_start);
        olen = b.device_start;
        if (arg[0] != '\0')
            olen += (size_t)snprintf(out + olen, cap - olen,
                                     "%sdevice \"%s\"\n", indent, arg);
        /* device 行之后（含其换行）原样接上 */
        olen += (size_t)snprintf(out + olen, cap - olen, "%s", data + b.device_end);
    } else {
        /* 没有 device 行：需要时插到块的 '}' 那一行之前 */
        memcpy(out, data, b.close_start);
        olen = b.close_start;
        if (arg[0] != '\0')
            olen += (size_t)snprintf(out + olen, cap - olen,
                                     "%sdevice \"%s\"\n", indent, arg);
        olen += (size_t)snprintf(out + olen, cap - olen, "%s", data + b.close_start);
    }

    if (replace_file(conf_path, out, olen, err, errlen) == 0) {
        fprintf(stderr, "AUDIO: %s: device -> \"%s\"\n", conf_path, arg);
        rc = 0;
    }
    free(out);
    free(data);
    return rc;
}

/* ==================================================================
 * Windows：用 winmm 枚举（与 mpd winmm 插件的 device 语义同一套序号）
 * ================================================================== */
#ifdef _WIN32

static void utf8_from_wide(const wchar_t *w, char *out, size_t outlen)
{
    int n;

    if (outlen == 0) return;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outlen, NULL, NULL);
    if (n <= 0) out[0] = '\0';
    else out[outlen - 1] = '\0';
}

/* 枚举本机 waveOut 设备；返回条目数。
 * 第 0 项固定是「系统默认设备」（对应 mpd.conf 不写 device = WAVE_MAPPER），
 * 但只有在确实存在物理设备时才提供（否则 supported=false）。 */
static int enum_winmm(struct audio_device *devs, int max_devs)
{
    UINT numdevs, i;
    int cnt = 0;

    numdevs = waveOutGetNumDevs();
    if (numdevs == 0 || max_devs <= 0)
        return 0;

    snprintf(devs[cnt].id, sizeof(devs[cnt].id), "winmm:default");
    snprintf(devs[cnt].name, sizeof(devs[cnt].name), "System default");
    snprintf(devs[cnt].kind, sizeof(devs[cnt].kind), "winmm");
    devs[cnt].arg[0] = '\0';
    cnt++;

    for (i = 0; i < numdevs && cnt < max_devs; i++) {
        WAVEOUTCAPSW caps;
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
            continue;
        snprintf(devs[cnt].id, sizeof(devs[cnt].id), "winmm:%u", (unsigned)i);
        utf8_from_wide(caps.szPname, devs[cnt].name, sizeof(devs[cnt].name));
        snprintf(devs[cnt].kind, sizeof(devs[cnt].kind), "winmm");
        snprintf(devs[cnt].arg, sizeof(devs[cnt].arg), "%u", (unsigned)i);
        cnt++;
    }
    return cnt;
}

int audio_device_check(const struct audio_device *dev, char *err, size_t errlen)
{
    WAVEFORMATEX fmt;
    HWAVEOUT h = NULL;
    MMRESULT result;
    UINT id;

    if (errlen) err[0] = '\0';
    if (strcmp(dev->kind, "winmm") != 0) {
        snprintf(err, errlen, "device kind %s is not managed by this build", dev->kind);
        return -1;
    }
    if (dev->arg[0] == '\0')
        id = WAVE_MAPPER;
    else
        id = (UINT)strtoul(dev->arg, NULL, 10);

    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 44100;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize = 0;

    result = waveOutOpen(&h, id, &fmt, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        char text[256];
        if (waveOutGetErrorTextA(result, text, sizeof(text)) == MMSYSERR_NOERROR)
            snprintf(err, errlen, "cannot open audio device: %s", text);
        else
            snprintf(err, errlen, "cannot open audio device (waveOutOpen error %u)",
                     (unsigned)result);
        return -1;
    }
    waveOutClose(h);
    return 0;
}

#else /* ---------------------- 非 Windows ---------------------- */

/* Linux：只做「能列出设备」，不改任何系统 mpd 配置（前端据此禁用「应用」）。
 * 设备来源依次为 /proc/asound/pcm（每个 playback PCM）→ /proc/asound/cards
 * （每张声卡）→ PulseAudio socket。全都拿不到就 supported=false。 */

static void trim_copy(const char *src, size_t len, char *out, size_t outlen)
{
    size_t s = 0, e = len, o = 0;

    while (s < e && isspace((unsigned char)src[s])) s++;
    while (e > s && isspace((unsigned char)src[e - 1])) e--;
    while (s < e && o + 1 < outlen) out[o++] = src[s++];
    if (outlen) out[o] = '\0';
}

/* 00-03: HDMI 0 : HDMI 0 : playback 1  ->  alsa:hw:0,3 / "HDMI 0 (hw:0,3)" */
static int enum_alsa_pcm(struct audio_device *devs, int max_devs)
{
    FILE *f;
    char line[512];
    int cnt = 0;

    f = fopen("/proc/asound/pcm", "r");
    if (f == NULL) return 0;

    while (fgets(line, sizeof(line), f) != NULL && cnt < max_devs) {
        int card = -1, dev = -1;
        const char *colon, *name_start, *name_end;
        char name[128];

        if (strstr(line, "playback 1") == NULL) continue;
        if (sscanf(line, "%d-%d:", &card, &dev) != 2) continue;
        if (card < 0 || dev < 0) continue;

        colon = strchr(line, ':');
        if (colon == NULL) continue;
        name_start = colon + 1;
        name_end = strchr(name_start, ':');
        if (name_end == NULL) name_end = name_start + strlen(name_start);
        trim_copy(name_start, (size_t)(name_end - name_start), name, sizeof(name));

        snprintf(devs[cnt].id, sizeof(devs[cnt].id), "alsa:hw:%d,%d", card, dev);
        if (name[0] != '\0')
            snprintf(devs[cnt].name, sizeof(devs[cnt].name), "%s (hw:%d,%d)", name, card, dev);
        else
            snprintf(devs[cnt].name, sizeof(devs[cnt].name), "hw:%d,%d", card, dev);
        snprintf(devs[cnt].kind, sizeof(devs[cnt].kind), "alsa");
        snprintf(devs[cnt].arg, sizeof(devs[cnt].arg), "hw:%d,%d", card, dev);
        cnt++;
    }
    fclose(f);
    return cnt;
}

/*  0 [HDMI           ]: HDA-Intel - HDA ATI HDMI  ->  alsa:card:0 / "HDMI: HDA ATI HDMI" */
static int enum_alsa_cards(struct audio_device *devs, int max_devs)
{
    FILE *f;
    char line[512];
    int cnt = 0;

    f = fopen("/proc/asound/cards", "r");
    if (f == NULL) return 0;

    while (fgets(line, sizeof(line), f) != NULL && cnt < max_devs) {
        int card = -1;
        const char *lb, *rb;
        char cardname[128], desc[256];

        if (sscanf(line, " %d [", &card) != 1 || card < 0) continue;
        lb = strchr(line, '[');
        rb = lb ? strchr(lb, ']') : NULL;
        trim_copy(lb ? lb + 1 : "", lb && rb ? (size_t)(rb - lb - 1) : 0,
                  cardname, sizeof(cardname));
        trim_copy(rb ? rb + 2 : "", rb ? strlen(rb + 2) : 0, desc, sizeof(desc));

        snprintf(devs[cnt].id, sizeof(devs[cnt].id), "alsa:card:%d", card);
        if (cardname[0] != '\0' && desc[0] != '\0')
            snprintf(devs[cnt].name, sizeof(devs[cnt].name), "%.120s: %.120s", cardname, desc);
        else if (desc[0] != '\0')
            snprintf(devs[cnt].name, sizeof(devs[cnt].name), "%.240s", desc);
        else
            snprintf(devs[cnt].name, sizeof(devs[cnt].name), "card %d", card);
        snprintf(devs[cnt].kind, sizeof(devs[cnt].kind), "alsa");
        snprintf(devs[cnt].arg, sizeof(devs[cnt].arg), "hw:%d,0", card);
        cnt++;
    }
    fclose(f);
    return cnt;
}

static int enum_pulse(struct audio_device *devs, int max_devs)
{
    const char *env;
    char path[512];
    FILE *probe;

    if (max_devs <= 0) return 0;
    env = getenv("XDG_RUNTIME_DIR");
    if (env && env[0] != '\0')
        snprintf(path, sizeof(path), "%s/pulse/native", env);
    else
        snprintf(path, sizeof(path), "/run/user/%u/pulse/native", (unsigned)getuid());
    probe = fopen(path, "rb");
    if (probe == NULL) {
        snprintf(path, sizeof(path), "/var/run/pulse/native");
        probe = fopen(path, "rb");
        if (probe == NULL) return 0;
    }
    fclose(probe);

    snprintf(devs[0].id, sizeof(devs[0].id), "pulse:default");
    snprintf(devs[0].name, sizeof(devs[0].name), "PulseAudio (default sink)");
    snprintf(devs[0].kind, sizeof(devs[0].kind), "pulse");
    devs[0].arg[0] = '\0';
    return 1;
}

static int enum_linux(struct audio_device *devs, int max_devs)
{
    int cnt = enum_alsa_pcm(devs, max_devs);
    if (cnt == 0) cnt = enum_alsa_cards(devs, max_devs);
    if (cnt == 0) cnt = enum_pulse(devs, max_devs);
    return cnt;
}

int audio_device_check(const struct audio_device *dev, char *err, size_t errlen)
{
    (void)dev;
    if (errlen) err[0] = '\0';
    return 0;
}

#endif /* _WIN32 */

/* ==================================================================
 * 对外：枚举 + 当前值
 * ================================================================== */
int audio_devices_enumerate(struct audio_output_info *info,
                            struct audio_device *devs, int max_devs)
{
    int cnt;
    char arg[AUDIO_ID_MAX];

    memset(info, 0, sizeof(*info));
    if (max_devs > AUDIO_DEVICE_MAX) max_devs = AUDIO_DEVICE_MAX;

#ifdef _WIN32
    audio_conf_type("mpd.conf", info->type, sizeof(info->type));
    cnt = enum_winmm(devs, max_devs);
    info->supported = cnt > 0;

    /* 只有「第一个 audio_output 就是 winmm」时才由本程序代改 mpd.conf：
     * 其它类型（null / wasapi / alsa）有各自的 device 语义，贸然改写会让 mpd 起不来。 */
    info->can_set = info->supported &&
                    (info->type[0] == '\0' || _stricmp(info->type, "winmm") == 0);

    if (info->supported && audio_conf_current("mpd.conf", arg, sizeof(arg)) == 0) {
        int i;
        if (arg[0] == '\0') {
            /* 没写 device = WAVE_MAPPER */
            const char *t = info->type[0] ? info->type : "winmm";
            snprintf(info->current, sizeof(info->current), "%s:default", t);
        } else {
            for (i = 0; i < cnt; i++) {
                if (devs[i].arg[0] != '\0' && strcmp(devs[i].arg, arg) == 0) {
                    snprintf(info->current, sizeof(info->current), "%s", devs[i].id);
                    break;
                }
            }
            /* 认不出的写法（例如按设备名配置）保持空串，前端会退回第一项 */
        }
    }
#else
    cnt = enum_linux(devs, max_devs);
    info->supported = cnt > 0;
    /* Linux：能列出设备，但绝不代用户改系统 mpd 配置 */
    info->can_set = 0;
    info->current[0] = '\0';
    (void)arg;
#endif

    return cnt;
}
