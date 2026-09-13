/* 让 PKEY_* 等 GUID 在本编译单元内直接定义（Windows SDK 与 MinGW 通用做法），
   否则 functiondiscoverykeys_devpkey.h 只留 extern 声明，链接时报 undefined reference */
#define INITGUID

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

// 自定义布尔类型
typedef int bool;
#define true 1
#define false 0

// 定义托盘图标 ID
#define IDI_AurmpdICON 1
// 定义菜单 ID
#define IDM_EXIT 100
#define IDM_OPEN 101
#define IDM_ABOUT 102
// 开机自启勾选项（100~102 已被占用，勿冲突）
#define IDM_AUTOSTART 103

// 定时器 ID：检查「音频输出设备切换」重启请求
#define IDT_RESTART_CHECK 1002

// 音频输出设备切换：aurmpd 写好 mpd.conf 后留下请求文件，启动器看到就重启 mpd
#define RESTART_REQUEST_FILE L".mpd\\restart-request"
#define RESTART_RESULT_FILE  L".mpd\\restart-result"

#define TARGET_URL L"http://127.0.0.1:8600"
#define PIPE_NAME L"\\\\.\\pipe\\AurmpdPipe"
#define BUFFER_SIZE 1024

// 全局变量，用于存储窗口句柄
HWND hWnd;
// 全局变量，用于存储子进程的进程句柄
HANDLE hChildProcess_mpd = NULL;
HANDLE hChildProcess_aurmpd = NULL;

// 子进程所属的 Job 对象：设置 KILL_ON_JOB_CLOSE，这样启动器无论以何种方式退出
//（正常退出 / 崩溃 / 被任务管理器结束）都会连带结束 mpd 与 aurmpd，
// 避免残留进程占着目录导致旧版本删不掉。
static HANDLE g_job = NULL;

// 把子进程加入 Job。Job 创建失败、或当前进程已在别的 Job 中导致加入失败时，
// 静默降级为“仅靠显式关闭逻辑”，不影响正常使用。
static void AssignChildToJob(HANDLE hProcess) {
    if (g_job == NULL) {
        g_job = CreateJobObjectW(NULL, NULL);
        if (g_job != NULL) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
            memset(&jeli, 0, sizeof(jeli));
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
                CloseHandle(g_job);
                g_job = NULL;
            }
        }
    }
    if (g_job != NULL && hProcess != NULL) {
        AssignProcessToJobObject(g_job, hProcess);
    }
}

// exe 所在目录（不含结尾反斜杠）。启动时确定一次，之后 mpd.conf / mpd.exe /
// aurmpd.exe / htdocs 一律相对它解析，不再依赖进程初始工作目录。
static wchar_t g_exeDir[MAX_PATH] = {0};

// 前置声明 GracefullyCloseProcess 函数
BOOL GracefullyCloseProcess(HANDLE hProcess);

// 开机自启辅助函数（定义在文件后部，WndProc 中需提前使用）
static BOOL IsAutostartEnabled(void);
static BOOL SetAutostartEnabled(BOOL enable);

// 取 exe 所在目录；失败时回退到当前工作目录，保证 g_exeDir 非空
BOOL GetExeDirectory(wchar_t* dir, size_t cch) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash != NULL) {
            *slash = L'\0';
            if (wcsncpy_s(dir, cch, path, _TRUNCATE) == 0) {
                return TRUE;
            }
        }
    }
    if (GetCurrentDirectoryW((DWORD)cch, dir) != 0) {
        return TRUE;
    }
    dir[0] = L'\0';
    return FALSE;
}

// 把 exe 目录转成 UTF-8、正斜杠形式，供 mpd.conf 的 music_directory 使用
static void ExeDirToUtf8Slashes(char* out, size_t cch) {
    char tmp[MAX_PATH * 4];
    size_t j = 0;
    int len;
    if (cch == 0) return;
    out[0] = '\0';
    if (g_exeDir[0] == L'\0') return;
    len = WideCharToMultiByte(CP_UTF8, 0, g_exeDir, -1, tmp, sizeof(tmp), NULL, NULL);
    if (len <= 0) return;
    for (int i = 0; tmp[i] != '\0' && j + 1 < cch; ++i) {
        out[j++] = (tmp[i] == '\\') ? '/' : tmp[i];
    }
    out[j] = '\0';
}

/* 规范化 mpd.conf 里的 music_directory：
 *   - 反斜杠 \ 一律换成 /（mpd.conf 里 \ 是转义符，Windows 习惯写法会解析失败）
 *   - 相对路径补成「相对 exe 目录」的绝对路径
 * 只处理生效的 music_directory 行；原本被 # 注释掉的行原样保留。
 * 其余行逐字节原样写回。 */
static void NormalizeMusicDirectory(const char* conf_path) {
    static const char key[] = "music_directory";
    size_t klen = sizeof(key) - 1;
    FILE* f;
    long sz;
    size_t rd, cap, olen = 0;
    char *data, *out, *p;
    char exeUtf8[MAX_PATH * 4];

    f = fopen(conf_path, "rb");
    if (f == NULL) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return; }
    rewind(f);
    data = (char*)malloc((size_t)sz + 1);
    if (data == NULL) { fclose(f); return; }
    rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[rd] = '\0';

    ExeDirToUtf8Slashes(exeUtf8, sizeof(exeUtf8));

    cap = (size_t)sz * 2 + 256;
    out = (char*)malloc(cap);
    if (out == NULL) { free(data); return; }

    p = data;
    while (*p != '\0') {
        char* nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p); /* 不含 '\n' */
        size_t i = 0, v, vend;
        int handled = 0;

        while (i < llen && (p[i] == ' ' || p[i] == '\t')) i++;
        if (p[i] != '#' && llen >= i + klen && strncmp(p + i, key, klen) == 0 &&
            (i + klen == llen || p[i + klen] == ' ' || p[i + klen] == '\t')) {
            v = i + klen;
            while (v < llen && (p[v] == ' ' || p[v] == '\t')) v++;
            vend = llen;
            while (vend > v && (p[vend - 1] == '\r' || p[vend - 1] == ' ' || p[vend - 1] == '\t')) vend--;
            if (vend > v) {
                char val[MAX_PATH * 4];
                char* inner;
                size_t vlen = vend - v;
                int absolute;
                if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
                memcpy(val, p + v, vlen);
                val[vlen] = '\0';
                inner = val;
                if (strlen(inner) >= 2 && inner[0] == '"' && inner[strlen(inner) - 1] == '"') {
                    inner[strlen(inner) - 1] = '\0';
                    inner++;
                }
                for (char* q = inner; *q != '\0'; ++q) {
                    if (*q == '\\') *q = '/';
                }
                absolute = (inner[0] == '/') ||
                           (isalpha((unsigned char)inner[0]) && inner[1] == ':');
                if (!absolute && exeUtf8[0] != '\0') {
                    olen += (size_t)snprintf(out + olen, cap - olen,
                        "%.*smusic_directory \"%s/%s\"", (int)i, p, exeUtf8, inner);
                } else {
                    olen += (size_t)snprintf(out + olen, cap - olen,
                        "%.*smusic_directory \"%s\"", (int)i, p, inner);
                }
                handled = 1;
            }
        }
        if (!handled) {
            memcpy(out + olen, p, llen);
            olen += llen;
        }
        if (nl != NULL) {
            out[olen++] = '\n';
            p = nl + 1;
        } else {
            break;
        }
    }
    out[olen] = '\0';

    f = fopen(conf_path, "wb");
    if (f != NULL) {
        fwrite(out, 1, olen, f);
        fclose(f);
    }
    free(out);
    free(data);
}


// 获取默认音频播放设备名称
BOOL GetDefaultAudioDeviceName(wchar_t* deviceName, size_t bufferSize) {
    HRESULT hr;
    IMMDeviceEnumerator* deviceEnumerator = NULL;
    IMMDevice* defaultDevice = NULL;
    IPropertyStore* propertyStore = NULL;
    PROPVARIANT varName;
    // 手动定义GUID
    static const GUID CLSID_MMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    static const GUID IID_IMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    // 初始化 COM 库
    hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        return FALSE;
    }
    // 创建设备枚举器实例
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (LPVOID*)&deviceEnumerator);
    if (FAILED(hr)) {
        CoUninitialize();
        return FALSE;
    }
    // 获取默认音频渲染设备
    hr = deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(deviceEnumerator, eRender, eConsole, &defaultDevice);
    if (FAILED(hr)) {
        deviceEnumerator->lpVtbl->Release(deviceEnumerator);
        CoUninitialize();
        return FALSE;
    }
    // 获取设备属性存储
    hr = defaultDevice->lpVtbl->OpenPropertyStore(defaultDevice, STGM_READ, &propertyStore);
    if (FAILED(hr)) {
        defaultDevice->lpVtbl->Release(defaultDevice);
        deviceEnumerator->lpVtbl->Release(deviceEnumerator);
        CoUninitialize();
        return FALSE;
    }
    // 初始化属性值变量
    PropVariantInit(&varName);
    // 获取设备友好名称
    hr = propertyStore->lpVtbl->GetValue(propertyStore, &PKEY_Device_FriendlyName, &varName);
    if (SUCCEEDED(hr)) {
        if (varName.vt == VT_LPWSTR) {
            wcsncpy_s(deviceName, bufferSize, varName.pwszVal, _TRUNCATE);
        }
        PropVariantClear(&varName);
    }
    // 释放资源
    propertyStore->lpVtbl->Release(propertyStore);
    defaultDevice->lpVtbl->Release(defaultDevice);
    deviceEnumerator->lpVtbl->Release(deviceEnumerator);
    CoUninitialize();
    return SUCCEEDED(hr);
}

// 检查并复制文件
BOOL CheckAndCopyFiles() {
    FILE *src, *dst;
    char buffer[1024];
    size_t bytesRead;
    wchar_t deviceName[256];

    // 检查 mpd.conf 文件是否存在
    if (GetFileAttributes("mpd.conf") == INVALID_FILE_ATTRIBUTES) {
        // 检查 mpd.conf.tmp 文件是否存在
        if (GetFileAttributes("mpd.conf.tmp") != INVALID_FILE_ATTRIBUTES) {
            // 打开源文件和目标文件
            src = fopen("mpd.conf.tmp", "rb");
            dst = fopen("mpd.conf", "wb");
            if (src == NULL || dst == NULL) {
                MessageBox(NULL, "Failed to open mpd.conf files", "Error", MB_OK | MB_ICONERROR);
                return FALSE;
            }
            if (src && dst) {
                // 复制文件内容
                while ((bytesRead = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                    fwrite(buffer, 1, bytesRead, dst);
                }

                // 获取默认音频播放设备名称
                if (GetDefaultAudioDeviceName(deviceName, sizeof(deviceName) / sizeof(wchar_t))) {
                    // 显示 deviceName 的值
                    //MessageBoxW(NULL, deviceName, L"Default Audio Device Name", MB_OK | MB_ICONINFORMATION);
                    const char* audio_output_start = "\naudio_output {\n    type \"winmm\"\n    name \"";
                    fwrite(audio_output_start, strlen(audio_output_start), 1, dst);
                    // 转换为 UTF - 8 并写入文件
                    int len = WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, NULL, 0, NULL, NULL);
                    char* utf8DeviceName = (char*)malloc(len+1);
                    if (utf8DeviceName) {
                        WideCharToMultiByte(CP_UTF8, 0, deviceName, -1, utf8DeviceName, len, NULL, NULL);
                        fwrite(utf8DeviceName, 1, len -1 , dst);
                        free(utf8DeviceName);
                    }
                    const char* audio_output_end = "\"\n}\n";
                    fwrite(audio_output_end, strlen(audio_output_end), 1, dst);                    
                }

                // 关闭文件
                fclose(src);
                fclose(dst);
                // 规范化新生成的 music_directory（斜杠 + 绝对路径）
                NormalizeMusicDirectory("mpd.conf");
                return TRUE;
            } else {
                if (src) fclose(src);
                if (dst) fclose(dst);
                return FALSE;
            }
        } else {
            return FALSE;
        }
    }
    return TRUE;
}

// 启动 mpd 子进程（首次启动与「切换音频输出后重启」共用）。
// 新进程一律 AssignProcessToJobObject 到 g_job，这样启动器无论怎么退出
// （正常 / 崩溃 / 任务管理器结束）都会连带结束 mpd，不会残留。
static BOOL StartMpdProcess(void) {
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION pi;
    wchar_t command[MAX_PATH * 3];

    si.cb = sizeof(STARTUPINFOW);
    si.wShowWindow = SW_HIDE; // 隐藏子进程窗口
    si.dwFlags |= STARTF_USESHOWWINDOW;

    // 子进程与命令行路径一律用 exe 目录下的绝对路径
    if (g_exeDir[0] != L'\0') {
        _snwprintf(command, MAX_PATH * 3, L"\"%s\\mpd.exe\" \"%s\\mpd.conf\"", g_exeDir, g_exeDir);
    } else {
        wcscpy(command, L"mpd.exe mpd.conf");
    }

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL,
                        g_exeDir[0] ? g_exeDir : NULL, &si, &pi)) {
        return FALSE;
    }
    if (hChildProcess_mpd != NULL) {
        CloseHandle(hChildProcess_mpd);
    }
    hChildProcess_mpd = pi.hProcess;
    AssignChildToJob(pi.hProcess);
    CloseHandle(pi.hThread); // 不需要线程句柄
    return TRUE;
}

// 把重启结果写进 .mpd\restart-result，供 aurmpd 回一条可见错误/日志
static void WriteRestartResult(BOOL ok) {
    HANDLE h = CreateFileW(RESTART_RESULT_FILE, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        const char* text = ok ? "ok\r\n" : "failed to restart mpd.exe\r\n";
        DWORD written = 0;
        WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(h);
    }
}

// 检查 aurmpd 是否请求重启 mpd（切换音频输出设备）；返回时请求文件已删除
static void CheckMpdRestartRequest(void) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok;

    if (!GetFileAttributesExW(RESTART_REQUEST_FILE, GetFileExInfoStandard, &fad)) {
        return;
    }
    OutputDebugStringA("aurmpd: restart-request detected, restarting mpd\n");

    // 旧进程先请它退出，超时就强杀（与退出流程同一套逻辑）
    if (hChildProcess_mpd != NULL) {
        if (WaitForSingleObject(hChildProcess_mpd, 0) != WAIT_OBJECT_0) {
            GracefullyCloseProcess(hChildProcess_mpd);
            WaitForSingleObject(hChildProcess_mpd, 5000);
        }
        CloseHandle(hChildProcess_mpd);
        hChildProcess_mpd = NULL;
    }

    ok = StartMpdProcess();
    DeleteFileW(RESTART_REQUEST_FILE);
    WriteRestartResult(ok);
    OutputDebugStringA(ok ? "aurmpd: mpd restarted\n" : "aurmpd: failed to restart mpd\n");
}

// 处理窗口消息的回调函数
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            if (!CheckAndCopyFiles()) {
                MessageBox(hwnd, "Failed to copy or create mpd.conf", "Error", MB_OK | MB_ICONERROR);
            }            
            // 创建系统托盘图标
            NOTIFYICONDATA nid;
            nid.cbSize = sizeof(NOTIFYICONDATA);
            nid.hWnd = hwnd;
            nid.uID = IDI_AurmpdICON;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_USER + 1;
            // 加载自定义图标
            HICON hIcon = (HICON)LoadImage(
                GetModuleHandle(NULL),
                //MAKEINTRESOURCE(IDI_AurmpdICON),
                "IDI_AurmpdICON",
                IMAGE_ICON,
                0, 0, // 指定需要的尺寸
                LR_DEFAULTCOLOR
            );
            if (hIcon == NULL) {
                DWORD err = GetLastError();
                TCHAR msg[256];
                FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, msg, 256, NULL);
                MessageBox(NULL, msg, "Failed to load icon", MB_OK | MB_ICONERROR);
                hIcon = LoadIcon(NULL, IDI_APPLICATION);
            }
            nid.hIcon = hIcon; 
            // 设置托盘图标提示信息
            lstrcpy(nid.szTip, "Aurmpd Application");
            Shell_NotifyIcon(NIM_ADD, &nid);

            // 启动后自动最小化到系统托盘
            ShowWindow(hwnd, SW_MINIMIZE);

            // 启动子进程
            STARTUPINFOW si = { sizeof(STARTUPINFOW) };
            si.cb = sizeof(STARTUPINFOW);
            si.wShowWindow = SW_HIDE; // 隐藏子进程窗口
            si.dwFlags |= STARTF_USESHOWWINDOW;
            PROCESS_INFORMATION pi;
            // 创建mpd子进程（带托盘图标后立刻启动；音频输出切换后的重启也用同一个函数）
            if (!StartMpdProcess()) {
                MessageBox(hwnd, "Failed to start mpd process", "Error", MB_OK | MB_ICONERROR);
            }
            // 创建aurmpd子进程   
            wchar_t command2[MAX_PATH * 2];
            if (g_exeDir[0] != L'\0') {
                _snwprintf(command2, MAX_PATH * 2, L"\"%s\\aurmpd.exe\"", g_exeDir);
            } else {
                wcscpy(command2, L"aurmpd.exe");
            }
            if (!CreateProcessW(NULL, command2, NULL, NULL, FALSE, 0, NULL, g_exeDir[0] ? g_exeDir : NULL, &si, &pi)) {
                MessageBox(hwnd, "Failed to start aurmpd process", "Error", MB_OK | MB_ICONERROR);
            } else {
                // 保存子进程的句柄
                hChildProcess_aurmpd = pi.hProcess;
                AssignChildToJob(pi.hProcess);
                // 关闭线程句柄，因为我们不需要它
                CloseHandle(pi.hThread);                 
            }
            
            // 启动后打开浏览器并访问指定网址
            ShowWindow(hwnd, SW_HIDE); 
            ShellExecuteW(hwnd, L"open", TARGET_URL, NULL, NULL, SW_SHOWNOACTIVATE);

            // 定时检查「音频输出设备切换」请求（aurmpd 写 .mpd\restart-request）
            SetTimer(hwnd, IDT_RESTART_CHECK, 1000, NULL);
            break;
        }
        case WM_TIMER: {
            // 1s 一次的「请求重启 mpd」检查（切换音频输出设备用）
            if (wParam == IDT_RESTART_CHECK) {
                CheckMpdRestartRequest();
            }
            break;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, IDT_RESTART_CHECK);
            // 移除系统托盘图标
            NOTIFYICONDATA nid = { sizeof(NOTIFYICONDATA) };
            nid.hWnd = hwnd;
            nid.uID = IDI_AurmpdICON;
            if (!Shell_NotifyIcon(NIM_DELETE, &nid)) {
                MessageBox(hwnd, "Failed to remove tray icon", "Error", MB_OK | MB_ICONERROR);
            }

            // 先请求 aurmpd 优雅退出（命名管道 CLOSE），然后等它**真的**退出；
            // 等不到就强杀，再等一次。原先只发消息不等结果，是残留进程的主因。
            if (hChildProcess_aurmpd && (hChildProcess_aurmpd != INVALID_HANDLE_VALUE)) {
                HANDLE hPipe = CreateFileW(
                    PIPE_NAME,
                    GENERIC_WRITE,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    0,
                    NULL
                );
                if (hPipe != INVALID_HANDLE_VALUE) {
                    const char* message = "CLOSE";
                    DWORD bytesWritten;
                    if (!WriteFile(hPipe, message, (DWORD)strlen(message), &bytesWritten, NULL)) {
                        wprintf(L"Failed to send message. Error code: %lu\n", GetLastError());
                    }
                    CloseHandle(hPipe);
                }
                // 最多等 3 秒优雅退出，否则强杀并再等一次
                if (WaitForSingleObject(hChildProcess_aurmpd, 3000) != WAIT_OBJECT_0) {
                    wprintf(L"aurmpd did not exit gracefully, terminating it.\n");
                    GracefullyCloseProcess(hChildProcess_aurmpd);
                    WaitForSingleObject(hChildProcess_aurmpd, 3000);
                }
            }
            // mpd 没有退出接口，直接终止并等它结束，避免残留进程占着 .mpd 目录
            if (hChildProcess_mpd && (hChildProcess_mpd != INVALID_HANDLE_VALUE)) {
                if (WaitForSingleObject(hChildProcess_mpd, 0) != WAIT_OBJECT_0) {
                    GracefullyCloseProcess(hChildProcess_mpd);
                    WaitForSingleObject(hChildProcess_mpd, 3000);
                }
            }
            // 关闭进程句柄
            if (hChildProcess_mpd != NULL) {
                CloseHandle(hChildProcess_mpd);
            }
            if (hChildProcess_aurmpd != NULL) {
                CloseHandle(hChildProcess_aurmpd);
            }
    
            PostQuitMessage(0);
            break;
        }
        case WM_USER + 1: {
            // 处理托盘图标消息
            switch (LOWORD(lParam)) {
                case WM_RBUTTONUP: {
                    // 每次弹出前都重新读一次实际状态（不缓存，避免状态过期）
                    BOOL autostartOn = IsAutostartEnabled();
                    // 创建弹出菜单
                    HMENU hMenu = CreatePopupMenu();
                    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open");
                    AppendMenuW(hMenu, MF_STRING | (autostartOn ? MF_CHECKED : MF_UNCHECKED), IDM_AUTOSTART, L"开机自启 / Auto start");
                    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About");
                    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

                    // 获取鼠标位置
                    POINT pt;
                    GetCursorPos(&pt);

                    // 显示弹出菜单，避免激活主窗口
                    UINT cmd = TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_NOANIMATION | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, hwnd, NULL);
                    if (cmd > 0) {
                        SendMessage(hwnd, WM_COMMAND, cmd, 0);
                    }
                    DestroyMenu(hMenu);
                    break;
                }
            }
            break;
        }
        case WM_COMMAND: {
            // 处理菜单命令
            switch (LOWORD(wParam)) {
                case IDM_EXIT: {                 
                    // 退出应用程序
                    DestroyWindow(hwnd);
                    break;
                }
                case IDM_OPEN: {
                    // 打开浏览器并访问指定网址
                    ShowWindow(hwnd, SW_HIDE); 
                    ShellExecuteW(hwnd, L"open", TARGET_URL, NULL, NULL, SW_SHOWNOACTIVATE);
                    break;
                }
                case IDM_AUTOSTART: {
                    // 切换开机自启；写入后立即用实际状态提示结果
                    BOOL wantEnable = IsAutostartEnabled() ? FALSE : TRUE;
                    if (!SetAutostartEnabled(wantEnable)) {
                        MessageBoxW(hwnd,
                                    wantEnable ? L"无法启用开机自启：写入注册表失败。"
                                               : L"无法关闭开机自启：删除注册表值失败。",
                                    L"aurmpd", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
                    }
                    break;
                }
                case IDM_ABOUT: {
                    const wchar_t* aboutText = L"aurmpd music player version 0.2.0\n"
                                               L"By luping(luping@189.cn)\n"
                                               L"https://github.com/breezecloud/aurmpd/";
                    // 确保主窗口隐藏
                    ShowWindow(hwnd, SW_HIDE);                                               
                    MessageBoxW(hwnd, aboutText, L"About",MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
                    break;
                }                
            }
            break;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// 优雅关闭子进程
BOOL GracefullyCloseProcess(HANDLE hProcess) {
    if (hProcess == NULL) {
        return TRUE;
    }

    // 获取进程 ID
    DWORD dwProcessId = GetProcessId(hProcess);
    if (dwProcessId == 0) {
        return FALSE;
    }

    // 尝试向进程发送关闭信号
    /*
    if (GenerateConsoleCtrlEvent(CTRL_C_EVENT, dwProcessId)) {        
        // 等待进程退出
        if (WaitForSingleObject(hProcess, 5000) == WAIT_OBJECT_0) { 
            return TRUE;
        }
    }
    */
    // 强制终止进程
    return TerminateProcess(hProcess, 0);
}

// 确保 mpd 需要的 .mpd / .mpd\playlists 目录存在。
// 压缩包解压时可能丢失空目录（zip 里没有目录项），而 mpd 一旦打不开
// .mpd/log 或 .mpd/database 就会直接启动失败、曲库全空 —— 这里兜底创建。
static void EnsureDataDirectories(void) {
    CreateDirectoryW(L".mpd", NULL);
    CreateDirectoryW(L".mpd\\playlists", NULL);
}

/* --------------------- 开机自启（与后端同一契约） ---------------------
 * HKCU\Software\Microsoft\Windows\CurrentVersion\Run
 *   值名   : "aurmpd"
 *   值内容 : "<exe 目录>\winaurmpd.exe"（带引号，路径可含空格）
 * 存在即“已启用”，不存在即“未启用”。
 * 注意：与 aurmpd.exe 里实现的是同一个键名/值名，两处必须保持一致。
 * -------------------------------------------------------------------- */
#define AUTOSTART_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTOSTART_VALUE_NAME L"aurmpd"
#define AUTOSTART_REG_ACCESS (KEY_READ | KEY_WRITE | KEY_WOW64_64KEY)

// 取 "<exe 目录>\winaurmpd.exe"，成功返回 TRUE
static BOOL GetLauncherPath(wchar_t* out, size_t cch) {
    wchar_t dir[MAX_PATH];
    size_t need;

    if (!GetExeDirectory(dir, MAX_PATH)) {
        return FALSE;
    }
    if (dir[0] == L'\0') {
        return FALSE;
    }
    need = wcslen(dir) + 1 + wcslen(L"winaurmpd.exe") + 1;
    if (need > cch) {
        return FALSE;
    }
    wcscpy(out, dir);
    wcscat(out, L"\\winaurmpd.exe");
    return TRUE;
}

// 每次弹出菜单前读一次实际状态，不缓存
static BOOL IsAutostartEnabled(void) {
    HKEY hkey;
    LONG rc;

    rc = RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0, AUTOSTART_REG_ACCESS, &hkey);
    if (rc != ERROR_SUCCESS) {
        return FALSE;
    }
    rc = RegQueryValueExW(hkey, AUTOSTART_VALUE_NAME, NULL, NULL, NULL, NULL);
    RegCloseKey(hkey);
    return (rc == ERROR_SUCCESS) ? TRUE : FALSE;
}

// 写入 / 删除自启值，成功返回 TRUE
static BOOL SetAutostartEnabled(BOOL enable) {
    HKEY hkey;
    LONG rc;

    if (enable) {
        wchar_t path[MAX_PATH + 8];
        wchar_t quoted[MAX_PATH + 12];

        if (!GetLauncherPath(path, sizeof(path) / sizeof(path[0]))) {
            return FALSE;
        }
        quoted[0] = L'"';
        wcscpy(quoted + 1, path);
        wcscat(quoted, L"\"");

        rc = RegCreateKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0, NULL, 0,
                             AUTOSTART_REG_ACCESS, NULL, &hkey, NULL);
        if (rc != ERROR_SUCCESS) {
            return FALSE;
        }
        rc = RegSetValueExW(hkey, AUTOSTART_VALUE_NAME, 0, REG_SZ,
                            (const BYTE*)quoted,
                            (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
        RegCloseKey(hkey);
        return (rc == ERROR_SUCCESS) ? TRUE : FALSE;
    }

    rc = RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0, AUTOSTART_REG_ACCESS, &hkey);
    if (rc != ERROR_SUCCESS) {
        // 键不存在 => 本来就没启用，当作成功
        return (rc == ERROR_FILE_NOT_FOUND) ? TRUE : FALSE;
    }
    rc = RegDeleteValueW(hkey, AUTOSTART_VALUE_NAME);
    RegCloseKey(hkey);
    return (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) ? TRUE : FALSE;
}

/* 路径自愈：程序目录被改名 / 移动后（本项目就是靠“解压到新目录”升级），
 * 旧注册表值会指向不存在的路径，开机自启静默失效。启动时若自启已启用，
 * 就把值刷新成当前 exe 目录下的 winaurmpd.exe。
 * ⚠ 只在已启用时刷新；未启用绝不能因为启动而创建自启项。 */
static void RefreshAutostartPathIfEnabled(void) {
    if (!IsAutostartEnabled()) {
        return;
    }
    if (!SetAutostartEnabled(TRUE)) {
        OutputDebugStringA("aurmpd: failed to refresh autostart path\n");
    }
}

// 主函数
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int iCmdShow) {
    int argc;
    LPWSTR* argv;
    bool debugMode = false;

    // 先把工作目录定到 exe 所在目录：双击 / 快捷方式 / 从任意目录启动，
    // mpd.conf、mpd.exe、aurmpd.exe、htdocs 都解析到同一处。
    if (GetExeDirectory(g_exeDir, MAX_PATH)) {
        SetCurrentDirectoryW(g_exeDir);
    }
    EnsureDataDirectories();
    // 自启路径自愈：仅在已启用时把注册表值刷新为当前 exe 目录
    RefreshAutostartPathIfEnabled();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"-d") == 0) {
            debugMode = true;
            break;
        }
    }

    if (!debugMode) {
        // 在后台执行
        /*
        if (GetConsoleWindow()) {
            FreeConsole();
        }*/
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        wchar_t szCommandLine[MAX_PATH];
        GetModuleFileNameW(NULL, szCommandLine, MAX_PATH);
        wcscat_s(szCommandLine, MAX_PATH, L" -d");
        if (CreateProcessW(NULL, szCommandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return 0;
        }
    }

    LocalFree(argv); 
       
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "AurmpdAppClass";
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_AurmpdICON));
    
    if (wc.hIcon == NULL) {
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION); // 使用默认图标作为备用
    }
    RegisterClass(&wc);

    // 创建窗口，使用 WS_EX_TOOLWINDOW WS_POPUP 样式并设置大小为 0x0，使得窗口不可见
    hWnd = CreateWindowEx(WS_EX_TOOLWINDOW, wc.lpszClassName, "Aurmpd Application", WS_POPUP | WS_EX_TOOLWINDOW,
        0, 0, 0, 0, NULL, NULL, hInstance, NULL);        

    if (hWnd == NULL) {
        return 0;
    }
    
    // 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);  
    }
    return msg.wParam;
}