# 开发与发布速查

> **目的**：下一次改动不必重新探索仓库，读这一页就够。所有命令均已实测。
> 功能与接口清单见 [FEATURES.md](FEATURES.md)；已修复问题见 [FIXES.md](FIXES.md)。

## 仓库结构（只需要记这些）

| 路径 | 作用 |
| --- | --- |
| `src/mpd_client.c/.h` | WS 命令表（`MPD_CMDS(X)` 宏）、mpd 连接与轮询线程、互斥锁 |
| `src/audio_devices.c/.h` | 音频输出设备枚举（winmm / ALSA）、`audio_devices` JSON、mpd.conf 的 audio_output 读写（纯函数，可离线单测） |
| `src/http_server.cpp` | REST 路由（`/api/*`）；**已知路由的方法校验在此**（不匹配返回 405） |
| `src/mpdqueue.cpp/.hpp` | 内部队列 = mpd 队列的镜像（`syncFromMpd()`） |
| `src/main.cpp` | 事件循环、退出标志（`volatile sig_atomic_t`）、Windows 管道线程 |
| `src/winmain.c` | Windows 启动器：托盘、开机自启（注册表）、`EnsureDataDirectories()`、子进程 Job |
| `aurial/src/js/**` | 前端（preact + semantic-ui）；`mpdws.js` = 带发送队列的 WS 封装 |
| `aurial/src/js/i18n.js` | 中英词条（两套键名必须一致，缺失回退键名） |
| `aurial/src/js/version.js` | 版本号 + 本仓库/上游地址（**改版本只改这里和 CMakeLists**） |
| `docs/` | 面向用户的文档与截图 |

## 构建

```bash
# 后端（Linux）
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
# 后端（Windows 交叉编译）
cmake --build build-win -j2          # 产物 build-win/{aurmpd.exe,winaurmpd.exe}
# 前端（产物直出 build/htdocs，服务从磁盘读，无需重启）
cd aurial && NODE_OPTIONS=--openssl-legacy-provider npm run dist
```

## 测试

- **WS 探针**（不占端口）：`node /tmp/ws.js "MPD_API_GET_QUEUE,0"`；服务状态用 `/tmp/wsstate.js`
- **REST**：`curl -s http://127.0.0.1:8600/api/queue | python3 -m json.tool | head`
- **真浏览器**：puppeteer（`/tmp/pptr/probe2.js`）。启动参数 `{args:['--no-sandbox','--disable-dev-shm-usage','--disable-gpu']}`；
  **不要用 `waitUntil:'networkidle2'`**（页面从 cdnjs 加载 semantic-ui，永不 idle），用 `'domcontentloaded'` + 等待
- **看不了图片**：模型无图像输入，排版结论必须来自 `getBoundingClientRect()` 几何量测；截图只给用户看
- **不抢 8600**：需要独立实例时用 `unshare -rn sh -c 'ip link set lo up; cd build && ./aurmpd &'`
  （真 mpd 集成检查：同一个 `unshare -rn` 里先 `mpd --no-daemon /home/loong/projects/aurmpd-testenv/mpd.conf &`）
- **离线单测**（不需要 mpd / 声卡 / CMake）：`cd /tmp && gcc -I <repo>/src <repo>/test/audio_devices_test.c <repo>/src/audio_devices.c <repo>/src/json_encode.c -o t && ./t`；同一份用例可用 `x86_64-w64-mingw32-gcc ... -lwinmm` 编译后 `wine t.exe` 验证 Windows 代码路径
- **前端断言（真浏览器 + 桩后端）**：`/tmp/pptr/wsstub.py`（静态站点 + 最小 WS，按 `SCENARIO` 回答新命令）+ `/tmp/pptr/check-audio.js`（用 harness 拦 CDN）
- 有 Subsonic 相关改动时：`/tmp/fake-subsonic.py`（本地假服务器，8897，已带 CORS 与 `.view` 兼容）

## 发布流程（vX.Y.Z）

1. 改 `aurial/src/js/version.js` 的 `APP_VERSION` + `CMakeLists.txt` 的 `CPACK_PACKAGE_VERSION_*`
2. 三个构建（见上）
3. 打包：`jukebox-X.Y.Z-win64.zip`、`jukebox-X.Y.Z-linux-x86_64.tar.gz`（zip 用 python 显式写目录项）
4. 更新下载页 `index.html` 里的文件名（旧版包删除，避免混淆）
5. `git add -A && git commit && git push origin main`
6. `git tag -a vX.Y.Z && git push origin vX.Y.Z`
7. `gh release create vX.Y.Z --repo DragonTTDream/aurmpd-jukebox --title "…" --notes-file … <两个包>`

## 踩过的坑（别重复）

| 坑 | 规避 |
| --- | --- |
| zip 里空目录丢失（如 `.mpd/playlists`）→ mpd 起不来 | 打包脚本**显式写入目录项** |
| `pkill -f "<模式>"` 会匹配到自己的命令行并自杀 | 用 `pgrep -x` 或按端口 `ss -ltnp` 找 pid 再 kill |
| 无头浏览器截图中文变方框 | 先确认 `fc-list :lang=zh` 非空（已装 Noto Sans CJK SC） |
| `git push` 偶发 `HTTP2 framing layer` 错误 | `git -c http.version=HTTP/1.1 push` |
| `gh release` 默认可能解析到 `upstream` 远程 | 始终显式 `--repo DragonTTDream/aurmpd-jukebox` |
| 加锁时提前 `return` 未解锁 → 退出时死锁 | 锁区间内只用单一出口（`goto out`） |
| 跨线程共享的退出标志被优化掉 | 声明为 `volatile sig_atomic_t` |
