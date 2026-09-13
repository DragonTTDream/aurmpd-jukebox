/* aurmpd —— 音频输出设备（扬声器）检测与 mpd.conf 读写
 *
 * 设计要点（详见 docs/FEATURES.md §7.5）：
 *  - 平台差异全部关在 audio_devices.c 内部，Linux 构建不依赖任何 Windows 头文件。
 *  - 设备枚举结果是一个「列表 + 当前项」的纯数据结构，JSON 序列化不依赖平台，
 *    因此可以在任意平台上用假数据验证结构与转义（见 test/）。
 *  - mpd.conf 的读取/改写是纯文件操作（平台无关），同样可离线验证。
 *
 * mpd 侧的 device 语义（本模块的 arg 就是写进 mpd.conf 的那个值）：
 *  - winmm  插件：device 是 waveOut 设备序号（整数）或设备名前缀；
 *               不写 device = WAVE_MAPPER（系统默认设备）。
 *  - wasapi 插件：device 是 IMMDevice 序号或设备友好名的完整匹配；不写 = 默认端点。
 *  - alsa   插件：device 形如 "hw:0,3"；不写 = plughw/default。
 * 本模块只管理 winmm 输出（与启动器生成 mpd.conf 时的类型一致）。
 */
#ifndef __AUDIO_DEVICES_H__
#define __AUDIO_DEVICES_H__

#include <stddef.h>

#define AUDIO_DEVICE_MAX 32
#define AUDIO_ID_MAX     64
#define AUDIO_NAME_MAX   256

/* 一个可选的音频输出设备 */
struct audio_device {
    char id[AUDIO_ID_MAX];     /* 回给前端的稳定标识，如 winmm:0 / alsa:hw:0,3 */
    char name[AUDIO_NAME_MAX]; /* 展示名（来自操作系统，UTF-8） */
    char kind[16];             /* winmm | wasapi | alsa | pulse */
    char arg[AUDIO_ID_MAX];    /* 写入 mpd.conf 的 device 值；空串 = 不写该项（用插件默认设备） */
};

/* 一次枚举的结果 */
struct audio_output_info {
    int  supported;             /* 是否检测到可用输出设备（0 = 未检测到） */
    int  can_set;               /* 是否允许由本程序自动改 mpd.conf 并重启 mpd */
    char current[AUDIO_ID_MAX]; /* 当前生效设备标识；空 = 未知/不适用 */
    char type[16];              /* mpd.conf 里第一个 audio_output 的 type；空 = 未读到 */
};

/* 枚举设备；返回设备数（0 = 没有可用设备，此时 info->supported 也是 0） */
int audio_devices_enumerate(struct audio_output_info *info,
                            struct audio_device *devs, int max_devs);

/* 纯 JSON 序列化（无平台依赖，可单测）：
 * {"type":"audio_devices","data":{"supported":b,"current":"s","canSet":b,
 *  "devices":[{"id":"s","name":"s","kind":"s"},...]}}
 * 返回写入长度（不含结尾 NUL）。 */
int audio_devices_json(char *buf, int bufsize,
                       const struct audio_output_info *info,
                       const struct audio_device *devs, int ndevs);

/* ---- mpd.conf 读取/改写（平台无关的纯文件操作） ----
 * 目标都是「第一个 audio_output {...} 块」，其余内容逐字节原样保留。 */

/* 读取第一个 audio_output 块的 type 值；成功返回 0，写入 out（可能为空串） */
int audio_conf_type(const char *conf_path, char *out, size_t outlen);

/* 读取第一个 audio_output 块的 device 值；成功返回 0（未写该项则 out 为空串），
 * 文件不存在 / 没有 audio_output 块返回 -1 */
int audio_conf_current(const char *conf_path, char *out, size_t outlen);

/* 把 device 值写进第一个 audio_output 块：
 *  arg 非空 -> 写入/覆盖 `device "<arg>"`；arg 为空 -> 删除该行（回到默认设备）。
 * 成功返回 0；失败返回 -1 并填写 err。写入采用「临时文件 + 原子替换」，失败不会
 * 破坏原 mpd.conf。 */
int audio_conf_set_device(const char *conf_path, const char *arg,
                          char *err, size_t errlen);

/* 目标设备现在是否可用（Windows: 用 waveOutOpen 试开一次；其它平台恒为可用）。
 * 成功返回 0，失败返回 -1 并填写 err。 */
int audio_device_check(const struct audio_device *dev, char *err, size_t errlen);

#endif /* __AUDIO_DEVICES_H__ */
