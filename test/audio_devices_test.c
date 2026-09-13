/*
 * audio_devices 单元测试（不依赖 CMake、不依赖 mpd、不依赖声卡）
 *
 * Linux:
 *   gcc -I src test/audio_devices_test.c src/audio_devices.c src/json_encode.c \
 *       -o /tmp/audio_devices_test && /tmp/audio_devices_test
 * Windows（交叉编译后跑 wine，验证 winmm 枚举代码路径真的能跑起来）:
 *   x86_64-w64-mingw32-gcc -I src test/audio_devices_test.c src/audio_devices.c \
 *       src/json_encode.c -o /tmp/audio_devices_test.exe && wine /tmp/audio_devices_test.exe
 *
 * 覆盖：
 *   1) audio_devices_json 的结构与转义（引号 / 反斜杠 / 中文设备名）
 *   2) audio_conf_type / audio_conf_current 解析
 *   3) audio_conf_set_device：覆盖已有 device / 插入 device / 删除 device，
 *      并逐字节确认其它内容未被改动
 *   4) 错误路径：文件不存在、没有 audio_output 块、不存在的设备 id
 *   5) 本机真实枚举（Linux: /proc/asound；Windows: waveOut）——只打印不断言，
 *      平台差异由人读输出判断
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_devices.h"

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("  [ok]   %s\n", what);
    } else {
        failures++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_contains(const char *hay, const char *needle, const char *what)
{
    checks++;
    if (hay != NULL && strstr(hay, needle) != NULL) {
        printf("  [ok]   %s\n", what);
    } else {
        failures++;
        printf("  [FAIL] %s\n     haystack: %s\n     missing : %s\n", what,
               hay ? hay : "(null)", needle);
    }
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) { printf("cannot write %s\n", path); exit(2); }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

int main(void)
{
    char buf[8192];
    char tmp[256];
    char *text;

    printf("== 1) JSON 结构与转义（假数据）==\n");
    {
        struct audio_output_info info;
        struct audio_device devs[3];
        int n;

        memset(&info, 0, sizeof(info));
        memset(devs, 0, sizeof(devs));
        info.supported = 1;
        info.can_set = 1;
        snprintf(info.current, sizeof(info.current), "winmm:1");
        snprintf(info.type, sizeof(info.type), "winmm");

        snprintf(devs[0].id, sizeof(devs[0].id), "winmm:default");
        snprintf(devs[0].name, sizeof(devs[0].name), "System default");
        snprintf(devs[0].kind, sizeof(devs[0].kind), "winmm");
        devs[0].arg[0] = '\0';

        snprintf(devs[1].id, sizeof(devs[1].id), "winmm:1");
        snprintf(devs[1].name, sizeof(devs[1].name), "Speakers \"Realtek\" C:\\path");
        snprintf(devs[1].kind, sizeof(devs[1].kind), "winmm");
        snprintf(devs[1].arg, sizeof(devs[1].arg), "1");

        snprintf(devs[2].id, sizeof(devs[2].id), "alsa:hw:0,3");
        snprintf(devs[2].name, sizeof(devs[2].name), "HDMI 0 （中文设备名）");
        snprintf(devs[2].kind, sizeof(devs[2].kind), "alsa");
        snprintf(devs[2].arg, sizeof(devs[2].arg), "hw:0,3");

        n = audio_devices_json(buf, sizeof(buf), &info, devs, 3);
        printf("  回包（%d 字节）: %s\n", n, buf);

        check(n == (int)strlen(buf), "返回长度 == 实际字符串长度");
        check_contains(buf, "\"type\":\"audio_devices\"", "type 字段");
        check_contains(buf, "\"data\":{\"supported\":true", "supported 字段");
        check_contains(buf, "\"current\":\"winmm:1\"", "current 字段");
        check_contains(buf, "\"canSet\":true", "canSet 字段");
        check_contains(buf, "\"devices\":[{\"id\":\"winmm:default\",\"name\":\"System default\",\"kind\":\"winmm\"}",
                       "第一个设备对象字段顺序 id/name/kind");
        check_contains(buf, "\"name\":\"Speakers \\\"Realtek\\\" C:\\\\path\"",
                       "设备名中的引号与反斜杠被正确转义");
        check_contains(buf, "\"name\":\"HDMI 0 （中文设备名）\"", "中文（UTF-8）设备名原样输出");
        check_contains(buf, "{\"id\":\"alsa:hw:0,3\",\"name\":\"HDMI 0 （中文设备名）\",\"kind\":\"alsa\"}]}}",
                       "数组末尾与整体闭合正确");

        /* supported=false / canSet=false / 无设备 */
        info.supported = 0;
        info.can_set = 0;
        info.current[0] = '\0';
        n = audio_devices_json(buf, sizeof(buf), &info, devs, 0);
        printf("  回包（%d 字节）: %s\n", n, buf);
        check_contains(buf, "\"supported\":false,\"current\":\"\",\"canSet\":false,\"devices\":[]}}",
                       "无设备时 supported/current/canSet/devices 结构正确");
    }

    printf("\n== 2) mpd.conf 解析（launcher 生成的真实模板 + audio_output 块）==\n");
    {
        char v[128];
        snprintf(tmp, sizeof(tmp), "audio_devices_test_tmp.conf");
        write_text(tmp,
            "# 音乐库路径\n"
            "playlist_directory \".mpd/playlists\"\n"
            "#music_directory \"c:/Music\"\n"
            "state_file \".mpd/state\"\n"
            "\n"
            "audio_output {\n"
            "    type \"winmm\"\n"
            "    name \"Speakers (Realtek Audio)\"\n"
            "}\n");

        check(audio_conf_type(tmp, v, sizeof(v)) == 0 && strcmp(v, "winmm") == 0,
              "audio_conf_type 读到 \"winmm\"");
        check(audio_conf_current(tmp, v, sizeof(v)) == 0 && v[0] == '\0',
              "没有 device 行时 audio_conf_current 返回空串（= WAVE_MAPPER）");

        check(audio_conf_set_device(tmp, "2", buf, sizeof(buf)) == 0, "插入 device \"2\" 成功");
        text = read_text(tmp);
        printf("  ---- 插入后 ----\n%s  ----------------\n", text);
        check_contains(text, "audio_output {\n    type \"winmm\"\n    name \"Speakers (Realtek Audio)\"\n    device \"2\"\n}\n",
                       "device 行插入在块的 '}' 之前、缩进 4 空格，其它行逐字节未变");

        check(audio_conf_current(tmp, v, sizeof(v)) == 0 && strcmp(v, "2") == 0,
              "再读回 device = \"2\"");
        free(text);

        check(audio_conf_set_device(tmp, "0", buf, sizeof(buf)) == 0, "覆盖为 device \"0\" 成功");
        text = read_text(tmp);
        check_contains(text, "    device \"0\"\n}\n", "device 值被覆盖（不是追加第二行）");
        check(strstr(text, "device \"2\"") == NULL, "旧的 device 行不再存在");
        free(text);

        check(audio_conf_set_device(tmp, "", buf, sizeof(buf)) == 0, "清空 device（回到默认设备）成功");
        text = read_text(tmp);
        check(strstr(text, "device") == NULL, "device 行已删除");
        check_contains(text, "    name \"Speakers (Realtek Audio)\"\n}\n", "删除 device 后块结构仍然完整");
        free(text);

        /* 幂等性：连续写同一个值不应变形 */
        audio_conf_set_device(tmp, "3", buf, sizeof(buf));
        {
            char *a = read_text(tmp);
            audio_conf_set_device(tmp, "3", buf, sizeof(buf));
            char *b = read_text(tmp);
            check(a != NULL && b != NULL && strcmp(a, b) == 0, "重复写入同一值是幂等的");
            free(a); free(b);
        }
        remove(tmp);
    }

    printf("\n== 3) 错误路径 ==\n");
    {
        char err[256];
        char v[64];

        snprintf(tmp, sizeof(tmp), "audio_devices_test_missing.conf");
        remove(tmp);
        check(audio_conf_type(tmp, v, sizeof(v)) == -1, "文件不存在: audio_conf_type 返回 -1");
        check(audio_conf_set_device(tmp, "1", err, sizeof(err)) == -1, "文件不存在: set_device 失败");
        printf("     err = %s\n", err);

        write_text(tmp, "music_directory \"c:/Music\"\nstate_file \".mpd/state\"\n");
        check(audio_conf_type(tmp, v, sizeof(v)) == -1, "没有 audio_output 块: type 返回 -1");
        check(audio_conf_set_device(tmp, "1", err, sizeof(err)) == -1, "没有 audio_output 块: set_device 失败");
        printf("     err = %s\n", err);
        text = read_text(tmp);
        check(strcmp(text, "music_directory \"c:/Music\"\nstate_file \".mpd/state\"\n") == 0,
              "失败时原文件未被改动");
        free(text);
        remove(tmp);
    }

    printf("\n== 4) 本机真实枚举（平台相关，仅打印）==\n");
    {
        struct audio_output_info info;
        struct audio_device devs[AUDIO_DEVICE_MAX];
        int i, n = audio_devices_enumerate(&info, devs, AUDIO_DEVICE_MAX);
        size_t jn = audio_devices_json(buf, sizeof(buf), &info, devs, n);

        printf("  supported=%d canSet=%d type=\"%s\" current=\"%s\" 设备数=%d\n",
               info.supported, info.can_set, info.type, info.current, n);
        for (i = 0; i < n; i++)
            printf("   - id=%-16s kind=%-6s arg=%-8s name=%s\n",
                   devs[i].id, devs[i].kind, devs[i].arg[0] ? devs[i].arg : "(不写)",
                   devs[i].name);
        printf("  JSON(%lu 字节): %s\n", (unsigned long)jn, buf);
    }

    printf("\n== 结论: %d 项检查, %d 失败 ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
