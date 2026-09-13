# aurmpd 功能详解（面向使用者 + 兼顾实现）

> **文档对象**：本项目（GPLv2，基于 breezecloud/aurmpd 修改）
> **撰写日期**：2026-09-12。
> **取证方式**：源码静态阅读（`src/*.c|*.cpp|*.h`、`aurial/src/js/**`）+ 本机只读实测（`curl` / `mpc` / WebSocket 探针）。
> **忽略的 vendored 文件**：`src/mongoose.c`、`src/mongoose.h`、`src/json.hpp`（以及 `aurial/node_modules/`）。
>
> **行号约定**：本文所有 `文件:行号` 均指**当前工作区文件的实际行号**。
> 注意 `ANALYSIS.md` 的行号基于更早的修订版（例如它写 `callback_mpd` 在 `mpd_client.c:72-351`，当前实际是 `:109-448`）；引用接口清单时以本文为准。
> 凡无法从代码或实测确认的，显式标注「**未验证**」，不做推断性描述。

---

## 目录

1. [项目是什么 / 架构总览](#1-项目是什么--架构总览)
2. [面向使用者的功能清单](#2-面向使用者的功能清单)
3. [配置详解](#3-配置详解)
4. [实现映射（功能 → 代码位置 → 通信路径）](#4-实现映射)
5. [常见问题排查](#5-常见问题排查)
6. [功能边界与已知限制](#6-功能边界与已知限制)

---

## 1. 项目是什么 / 架构总览

### 1.1 一句话定位

aurmpd 是一个**局域网音乐播放/点歌服务**：它把 [aurial](https://github.com/shrimpza/aurial) 的前端界面和 [ympd](https://www.ympd.org/) 的架构结合，用**内嵌的 mongoose HTTP/WebSocket 服务**提供网页，用 **libmpdclient** 驱动 **mpd** 真正播放音乐。浏览器只是一个控制面板，音乐与队列都在后台的 mpd 里，**关掉浏览器不会中断播放**（`README.md`「特点」第 3 条）。

### 1.2 分层结构

```
浏览器（Preact 前端，build/htdocs/index.js）
   │  HTTP  :8600   ── /api/queue*、/api/library、/api/hello、静态文件
   │  WebSocket /ws ── MPD_API_* 文本命令（客户端→服务端）
   │                    服务端→客户端广播 JSON（state/outputs/queue/song_change/…）
   ▼
aurmpd 进程（单进程，内嵌 mongoose 事件循环）
   │  libmpdclient（唯一一条共享连接 mpd.conn）
   ▼
mpd  (127.0.0.1:6600)  ── 真实的播放引擎、本地曲库 DB、原生歌单目录
```

- **监听地址/端口**：`0.0.0.0:8600`（`src/main.cpp:22`）——绑定全网卡，局域网任意机器可访问。
- **静态站点根目录**：`./htdocs`（`src/main.cpp:24`），相对于进程**当前工作目录**。
- **mpd 默认地址**：`127.0.0.1:6600`（`src/main.cpp:153-154`），可在页面里改（见 §2.9）。
- **响应缓冲区**：单条 JSON 消息上限 `MAX_SIZE = 1024*100`（`src/mpd_client.h:34`）。

### 1.3 线程模型

| 线程 | 启动位置 | 职责 |
|---|---|---|
| **主线程 / mongoose 事件循环** | `src/main.cpp:177-179`（`mg_mgr_poll(&mgr, 200)`，200ms 超时） | 处理 HTTP 请求、WS 收发、以及 `MG_EV_WAKEUP` 广播 |
| **mpd 轮询线程（1 Hz）** | `src/main.cpp:57-70`（`start_thread` 于 `:105`），循环内 `sleep(1)` 后调 `mpd_poll()` | 每秒向 mpd 取一次 `status`/`outputs`，并在曲目/队列变化时推事件 |
| **Windows 命名管道线程** | `src/main.cpp:72-92`（`start_thread` 于 `:160`，仅 `_WIN32`） | 轮询命名管道，收到 `CLOSE` 就置退出标志并兜底 `ExitProcess(0)` |

**关键设计：单条共享 mpd 连接 + 递归互斥锁**。libmpdclient 的连接不是线程安全的，而它同时被事件线程（`callback_mpd` / HTTP 处理）和轮询线程（`mpd_poll`）使用，因此所有访问都串行化在一把锁上：

- 锁的实现在 `src/mpd_client.c:41-69`：Windows 下是 `CRITICAL_SECTION`，Linux 下是 `PTHREAD_MUTEX_RECURSIVE`。
- `mpd_lock_init()` 必须在任何线程启动前调用（`src/main.cpp:156`；声明 `src/mpd_client.h:135`）。
- 加锁点示例：`callback_mpd` `src/mpd_client.c:123`、`mpd_poll` `:485`、`Library::getMpdDB` `src/library.cpp:43`、`Mpdqueue::addSong` `src/mpdqueue.cpp:9`。

**WS 发送模型**：处理器**不直接** `mg_ws_send`，而是把 JSON 塞进全局 `mpd.buf` 再用 `mg_wakeup(mgr, conn_id, buf, n)` 交给主循环（`src/mpd_client.c:442-446`，注释见 `:18-21`）。主循环在 `MG_EV_WAKEUP` 里向所有打了 `c->data[0]=='W'` 标记的连接广播（`src/main.cpp:129-138`）——这就是「多终端同步」的实现基础。

### 1.4 启动流程

1. `main()`：Windows 下先 `chcp 65001` 切 UTF-8 控制台（`src/main.cpp:144-147`）。
2. 设置日志级别（`:151`，默认 `MG_LL_INFO`）、mpd 地址（`:153-154`）、初始化锁（`:156`）。
3. `mg_mgr_init`（`:158`）→ Windows 启动管道线程（`:160`）→ 注册 `SIGINT` 处理器（`:164`）。
4. `mg_http_listen("0.0.0.0:8600", server_callback, &mgr)`，失败即 `exit(EXIT_FAILURE)`（`:166-170`）。
5. `mg_wakeup_init`（`:175`）→ 进入 `while(!force_exit) mg_mgr_poll(&mgr, 200)`（`:177-179`）。
6. 退出：`mpd_clear_all()`（`:181`）→ `mg_mgr_free`（`:182`）。

**退出时会做什么**（`mpd_clear_all`，`src/mpd_client.c:958-984`）：停止播放 → 清空 mpd 队列 → 触发一次 `update` → 清错误 → `mpd_disconnect()`。⚠️ 也就是说**退出即丢失当前播放队列**，与「队列放后台」的卖点有冲突（见 §6）。

### 1.5 前端构建产物如何被服务

- 前端源码在 `aurial/src/`，用 webpack 2 + babel 6 构建（`aurial/package.json:23-31`）。
- 构建输出目录硬编码为 `../../build/htdocs`（`aurial/src/webpack.dist.config.js:9-12`），产物是单文件 `index.js` + `index.html` + `css/` + `js/jquery-2.2.1.min.js`。
- 构建命令：`cd aurial && NODE_OPTIONS=--openssl-legacy-provider npm run dist`。
- 后端**不做任何内嵌**，只是 `mg_http_serve_dir(root_dir="./htdocs")` 把该目录当静态站点（`src/http_server.cpp:192-195`）——所以改了前端必须重新 `npm run dist`，并让进程的工作目录能找到 `htdocs`。
- ⚠️ `index.html` 通过 **CDN** 加载 semantic-ui 的 JS/CSS（`aurial/src/index.html:8-9`），离线局域网下 UI 会失效。
- ⚠️ webpack 配置里 uglify 被注释掉（`webpack.dist.config.js:19-26`），产物未压缩（实测本地 ~897 KB）。

---

## 2. 面向使用者的功能清单

> 页面布局：上方是 `ArtistList`（本地 Library 手风琴 + 可选 Subsonic 艺术家区），下方是播放器条 + 四个标签页（`src/../jsx/app.js:116-122`）：**Selection / Playlists / Queue / Settings**。

### 2.1 本地曲库浏览与播放

| 项 | 说明 |
|---|---|
| **作用** | 把 mpd 扫描到的本地音乐按**专辑**分组展示，可展开/选择、播放、入队 |
| **入口** | 页面左侧 `Library` 手风琴（`aurial/src/js/jsx/browser.js:429-455`）；点击专辑行 = 选中（`viewAlbum` `:351-360`）；行内 `Play` / `Queue` 按钮（`:501-508`） |
| **可见反馈** | 专辑列表：`专辑名 [年份] N tracks`（`:499`）；点 `Play` 后用整张专辑替换队列并播放；右上角弹出消息（`Messages`，`jsx/app.js:150-224`） |
| **依赖** | mpd 已连上且有曲库；数据来自 `GET /api/library`（`browser.js:249-264`） |

实现要点：
- 列表页进来就加载（`componentDidMount` `browser.js:243-246`）。
- 展开某专辑的曲目走 `POST /api/library`，body `{"album":"<id>"}`（`browser.js:275-296`），返回对象含 `song[]`。
- 专辑 `id` 取的是 **mpd 的 album tag**（`src/library.cpp:75` `Album newAlbum(album,artist,album,year)`）。**tag 缺失时全部塌成一个 `id:"unknown"` 专辑**——实测本机就是这一种（见 §4.6 证据）。
- `Play`/`Queue` 复用播放器已有的 `REPLACE`/`ADD` 分支（`browser.js:327-348` → `player.js:175-231`）。

### 2.2 队列管理

| 操作 | 入口 | 通信 |
|---|---|---|
| 单曲加入队列 | 曲目表的 `+`（`jsx/tracklist.js:98-104, 134-137`） | `POST /api/queue/add/` |
| 单曲「加入并播放」 | 曲目表 `▶`（非队列上下文，`tracklist.js:90-96`） | `POST /api/queue/add/play` |
| 整张专辑加入队列 | 专辑行 `Queue`（`browser.js:505-508`）或 Selection 页 `Add to Queue`（`jsx/selection.js:59-61`） | `POST /api/queue/add/` |
| 替换队列并播放 | 专辑行 `Play`（`browser.js:342-344`）、Selection `Play`（`selection.js:55-57`） | `POST /api/queue/replace/play` |
| 从队列删除单曲 | 队列行 `−`（`tracklist.js:99-100`） | `POST /api/queue/del`，body `{"queue_sid":N}` |
| 点歌播放（已在队列中） | 队列行 `▶`（`tracklist.js:91-92`） | WS `MPD_API_PLAY_TRACK,<queue_sid>` |
| 清空队列 | Queue 页 `Clear Queue`（`jsx/queue.js:46-49, 76`） | `POST /api/queue/replace/play`，body `[]` |

实现要点：
- HTTP 侧：`addTrackTompd`（`src/http_server.cpp:47-114`）逐条转成 `Song` 并 `Mpdqueue::addSong`；`Mpdqueue::addSong` 发 `mpd_run_add_id` 拿 mpd 的 queue id（`src/mpdqueue.cpp:7-21`），随后 `makeupPos()` 回填 `pos`（`http_server.cpp:112` → `mpdqueue.cpp:24-47`）。
- 后端有一个同步保护：**当本地队列长度为 0 时，先 `mpd_run_clear`**（`src/http_server.cpp:62-67`），避免本地/远端队列错位。
- 删除是**先删本地、再删 mpd**（`src/mpdqueue.cpp:76-88`），若 mpd 删除失败本地已丢（代码注释自认）。

### 2.3 多终端同步（同一队列，多浏览器）

- **作用**：多个浏览器/终端打开 `http://<host>:8600`，看到同一份队列与播放状态，任意一端操作都在另一端可见。
- **实现**：服务端只有**一份**队列状态（全局 `SQ`，`src/http_server.cpp:13`）与**一条** mpd 连接；状态变化由 1 Hz 轮询线程生成 JSON，经 `mg_wakeup` → `MG_EV_WAKEUP` **广播给所有 WS 连接**（`src/main.cpp:129-138`）。
- 前端把广播消息在 `player.js:47-69` 分发：`state`→`mpdstatus` 事件、`update_queue`→`playerEnqueued`（触发重拉 `/api/queue`）、其余→`mpdMessage`（歌单等组件消费）。
- 玩家按钮的「点亮」状态一律**跟随服务端广播**，而不是本地点击（`player.js:750-794` 注释明确写出这一点）。

### 2.4 播放控制（含本批新增的 循环 / 单曲 / 消费）

| 控件 | UI 位置 | 发送命令 |
|---|---|---|
| 上一首 / 下一首 | `player.js:313,316` | `MPD_API_SET_PREV` / `MPD_API_SET_NEXT`（`player.js:113-119`） |
| 播放/暂停（切换） | `player.js:314` | `MPD_API_SET_PAUSE` 或 `MPD_API_SET_PLAY`（`player.js:154-159`） |
| 停止 | `player.js:315` | `MPD_API_SET_STOP`（`player.js:161-163`） |
| 音量 | 播放器条右侧滑块（`player.js:453+`） | `MPD_API_SET_VOLUME,<0-100>`（`player.js:165-173`） |
| 随机 | `PlayerShuffleButton`（`player.js:705-748`） | `MPD_API_TOGGLE_RANDOM,0|1` |
| **循环（repeat）** | `PlayerModeButton`（`player.js:318`） | `MPD_API_TOGGLE_REPEAT,0|1` |
| **单曲（single）** | 同上（`player.js:319`） | `MPD_API_TOGGLE_SINGLE,0|1` |
| **消费（consume）** | 同上（`player.js:320`） | `MPD_API_TOGGLE_CONSUME,0|1` |

- 三个新增按钮共用一个通用组件 `PlayerModeButton`（`player.js:750-794`）：它订阅 `mpdstatus`，用 `param`（`repeat`/`single`/`consume`）从服务端 state 里取值点亮 `red`，点击时发送**与当前状态相反**的值（`:783-785`）。
- 服务端 cmd 处理：`src/mpd_client.c:176-195`（random/repeat/consume/single/crossfade），实际调用 `mpd_run_repeat/single/consume`（`:182/186/190`）。
- 状态回传字段在 `state` JSON 里：`repeat`/`single`/`consume`/`random`/`crossfade`（`src/mpd_client.c:616-635`）。
- ⚠️ Settings 页那个 **"Repeat queue" 复选框对 mpd 播放无效**（`settings.js:196-201` 标签已注明 legacy HTML5 player only）——它只影响已废弃的 HTML5 播放路径。

### 2.5 搜索（本地曲库搜索，本批新增）

| 项 | 说明 |
|---|---|
| **作用** | 过滤**本地曲库专辑**（不是 Subsonic 的） |
| **入口** | Library 下方的 `Search...` / `Search library...` 输入框（`browser.js:109-112`） |
| **匹配范围** | 专辑名 / 艺术家 / 年份，以及**已取回过的**该专辑曲目名（`filteredAlbums` `browser.js:397-414`） |
| **可见反馈** | 专辑列表实时收窄；同时 `Select all`/`Add selected` 只作用于过滤后的结果（`toggleAll` `:311-317`） |

**与 Subsonic 搜索的区别（重要）**：
- 这个输入框**同一个框**在配置了 Subsonic 时也会过滤 Subsonic 艺术家列表（`browser.js:70-79`）。
- 真正的 **Subsonic 服务端搜索**是另一条链路：`Subsonic.search()` → Subsonic `/rest/search2.view`，由 `ArtistList` 的艺术家区使用，**依赖 Subsonic 服务器**。
- **本地场景下**：只有「本地曲库搜索」这一条可用；未配置 Subsonic 时艺术家区根本不渲染（`browser.js:63-90`），所以该框只作用于本地库。
- 注意 `filteredAlbums` 对曲目名的匹配依赖 `albumCache`（`browser.js:226`）——**只有点开过的专辑**曲目名才会参与匹配。

### 2.6 批量操作（勾选 + 全选 + 批量加入队列，本批新增）

| 控件 | 位置 | 行为 |
|---|---|---|
| 每专辑复选框 | `LibraryAlbum`（`browser.js:493-495`） | 选中/取消（`toggleSelect` `:304-308`） |
| `Select all` | Library 控制条（`browser.js:437-440`） | 对**当前过滤结果**全选/全不选（`toggleAll`） |
| `Add selected (N)` | Library 控制条（`browser.js:441-444`） | 取回所有选中专辑曲目，汇总成一个数组后**一次**入队（`addSelectedToQueue` `:363-387`） |

- 实现：`Promise.all(ids.map(fetchAlbum))` → `tracks.concat(album.song)` → 发 `playerEnqueue {action:"ADD", tracks}`（`:370-381`）→ `player.js:205-231` 的 `ADD` 分支一次 `POST /api/queue/add/`。
- 无选中或选中专辑无曲目时给 warning 消息，不静默（`:365-378`）。
- 入队成功后清空勾选状态（`:381`）。

### 2.7 本地歌单（走 mpd 原生歌单，本批新增）

| 操作 | 入口 | 通信 |
|---|---|---|
| 列出歌单 | Playlists 标签页（`playlist.js:383-537`），进入即 `refresh()` | WS `MPD_API_GET_PLAYLISTS`（`playlist.js:436`） |
| 查看歌单内容 | 下拉选择某歌单 | WS `MPD_API_GET_PLAYLIST_SONGS,<name>`（`playlist.js:441`） |
| 保存当前队列为歌单 | `Save current queue` 按钮（`playlist.js:525`） | WS `MPD_API_SAVE_QUEUE,<name>`（`playlist.js:454`） |
| 加载歌单（**替换队列**） | `Load to queue` 按钮（`playlist.js:526`） | WS `MPD_API_ADD_PLAYLIST,<name>`（`playlist.js:463`）→ mpd `load` |
| 删除歌单 | `Delete` 按钮（`playlist.js:527`） | WS `MPD_API_RM_PLAYLIST,<name>`（`playlist.js:474`） |

实现要点：
- **分支逻辑**：页面在**未配置 Subsonic** 时渲染 `LocalPlaylistManager`（`playlist.js:184-187`），否则保持原有 Subsonic 歌单界面。判据是 `subsonic.url && localStorage.getItem('url')`（`:35-37`，因为 `index.js:45-51` 会给 url 兜底一个 demo 地址，只判 url 不够）。
- **后端**：`MPD_API_GET_PLAYLISTS`（`src/mpd_client.c:304-306` → `mpd_put_playlists` `:824-858`）、`GET_PLAYLIST_SONGS`（`:308-322` → `:862-903`）、`SAVE_QUEUE`（`:282-302`，`mpd_run_save`）、`ADD_PLAYLIST`（`:266-280`，`mpd_run_load`）、`RM_PLAYLIST`（`:324-344`，`mpd_run_rm`）。
- 保存/删除歌单后，后端会**顺带回一份新的歌单列表**（`:292-293`、`:334-335`），前端据此刷新。
- **Queue 标签页的 "Add to Playlist" 在本地模式下 = 保存当前队列**（`playlist.js:46-53` 把 `ADD`/`CREATE` 转发给 `localManager.saveQueue()`）。
- 歌单实际落在 mpd 的 `playlist_directory`（本机实测目录 `aurmpd-testenv/playlists/`，其中 `PartyMix.m3u` 被接口读到）。

### 2.8 曲库扫描（空库自动扫描 + 手动重扫，本批新增）

| 场景 | 触发 | 实现 |
|---|---|---|
| **空库自动扫描** | 每次与 mpd 连接成功后，若 `mpd_run_stats` 报告 songs==0 | `src/mpd_client.c:520-538` |
| **手动重扫** | Library 控制条 `Scan library` 按钮（`browser.js:445-448`） | WS `MPD_API_UPDATE_DB`（`browser.js:392`）→ `mpd_run_update(conn, NULL)`（`src/mpd_client.c:134-136`） |

- 自动扫描只判断「**库为空**」（刻意避免每次启动都为超大曲库做全量扫描，代码注释 `:520-523`）。
- 手动扫描是**全量** `update`，耗时可能很长；前端提示用户扫完刷新页面（`browser.js:393`）。
- ⚠️ 全量 `update` 完成时间不可知，本版**没有**"扫描完成"回执（**未验证**是否存在 mpd 侧事件推送）。

### 2.9 Subsonic 集成（可选；本地曲库场景可不用）

| 功能 | 前端入口 | 说明 |
|---|---|---|
| 连接配置 | Settings → Subsonic Connection（`settings.js:149-165`） | URL / 用户名 / 密码；密码用 `md5(password+salt)` 生成 token 存 localStorage（`settings.js:41-45`、`subsonic.js:40-42`） |
| 艺术家/专辑浏览 | 页面左侧 artist 区（`browser.js:69-90`、`Artist` `:119-177`、`Album` `:179-215`） | 只有配置了 Subsonic 才渲染与加载（`browser.js:22`、`:63-90`） |
| 服务器端歌单 | Playlists 标签页（`playlist.js:155-181`） | 走 `getPlaylists`/`getPlaylist`/`createPlaylist`/`updatePlaylist`/`deletePlaylist` |
| 搜索 | 同 §2.5 的框（过滤 artist 列表） | Subsonic `/rest/search2.view` |
| scrobble | 播放过半自动上报（`playerextra.js:31-80`） | 依赖 Subsonic 服务器；本机未配 → 不生效 |
| 封面 | `CoverArt`（`jsx/common.js:4-43`） | 用 `<img src=Subsonic getCoverArt>`；本地场景下会 404 → 回退占位图 |

**关键结论**：Subsonic 相关功能**全部依赖一个可用的 Subsonic 服务器**（Navidrome/Subsonic/Airsonic…）。做**纯本地曲库点歌机**时，可以不配置；此时艺术家区不渲染、歌单走 mpd 原生、封面走占位图，页面功能完整。

### 2.10 Windows 特性

| 特性 | 位置 | 说明 |
|---|---|---|
| **托盘图标 + 菜单** | `src/winmain.c:320-350`（`Shell_NotifyIcon`）、`:453-476`（右键菜单） | 菜单项 `Open` / `About` / `Exit`（`:459-461`），启动后自动最小化（`:353`） |
| **`winaurmpd.exe` 启动器** | `src/winmain.c:544-610` | 无参数启动时，**把自己以 `-d` 重新拉起**并立即返回（`:565-581`），实现"后台运行、无控制台" |
| **`-d` 调试模式** | `src/winmain.c:558-563` | 带 `-d` 时不重新拉起，保留可见控制台，便于看日志 |
| **`mpd.conf` 生成** | `src/winmain.c:258-315`（`CheckAndCopyFiles`） | 若 `mpd.conf` 不存在则从 `mpd.conf.tmp` 复制，并**自动追加 `audio_output{type "winmm"; name "<系统默认音频设备名>"}`**（`:282-297`） |
| **配置目录兜底** | `src/winmain.c:538-541`（`EnsureDataDirectories`） | 创建 `.mpd` 与 `.mpd\playlists`（zip 解压常丢空目录） |
| **工作目录固定** | `src/winmain.c:71-88`（`GetExeDirectory`）、`:551-553`（`SetCurrentDirectoryW`） | 双击/快捷方式/任意目录启动都解析到同一份 `mpd.conf`/`mpd.exe`/`htdocs` |
| **`music_directory` 规范化** | `src/winmain.c:106-175`（`NormalizeMusicDirectory`） | 把 `\` 换 `/`，相对路径补成「相对 exe 目录」的绝对路径 |
| **命名管道退出** | `src/winpipe.cpp:3-58`（服务端）、`src/winmain.c:408-434`（客户端发 `CLOSE`） | 管道名 `\\.\pipe\AurmpdPipe`（`winpipe.hpp:7`）；收到 `CLOSE` → 置退出标志 → 最多等 3s → 强杀 |
| **Job 对象保证子进程一起结束** | `src/winmain.c:41-61`、`:373`、`:389` | 启动器无论正常退出/崩溃/被任务管理器结束，`KILL_ON_JOB_CLOSE` 都会连带结束 `mpd.exe` 与 `aurmpd.exe` |
| **自动打开浏览器** | `src/winmain.c:394-396`、`:486-490` | `ShellExecuteW` 打开 `http://127.0.0.1:8600`（`TARGET_URL` `:28`） |

### 2.11 启动 / 退出 / 排错

- **Linux 启动**：`cd build && setsid ./aurmpd`（需工作目录能找到 `htdocs`）。默认监听 `0.0.0.0:8600`。
- **Windows 启动**：双击 `winaurmpd.exe`（后台）；排错用 `winaurmpd.exe -d`（保留控制台）。
- **退出**：
  - Linux：`Ctrl-C`（`SIGINT`，`src/main.cpp:37-42`）。⚠️ **只注册了 `SIGINT`**，`SIGTERM`/`kill` 走不到清理路径（`src/main.cpp:164`）。
  - Windows：托盘右键 `Exit` → `WM_DESTROY`（`src/winmain.c:399-452`）→ 向 aurmpd 发管道 `CLOSE` → 等 3s → 强杀 → 再强杀 mpd。
- **日志位置**：
  - aurmpd 自身：`MG_INFO`/`fprintf` 到 **stdout/stderr**（默认 `MG_LL_INFO`，`src/main.cpp:23,151`）。Windows 非 `-d` 模式控制台被隐藏（`CREATE_NO_WINDOW` `:576`），看不到输出——这也是 README 建议 `-d` 的原因。
  - mpd 侧：由 `mpd.conf` 的 `log_file` 决定（本机测试环境 = `aurmpd-testenv/mpd.log`；Windows 官方包 = `.mpd/` 下）。
- **`diagnose.cmd`**：本仓库中**不存在**该文件（`find` 全仓无 `*.cmd`/`diagnose*`），**未验证**，不描述其行为。

---

## 3. 配置详解

### 3.1 `mpd.conf`

本机实测生效的配置（`aurmpd-testenv/mpd.conf`）：

```ini
music_directory     "/path/to/mpd-config/music"
playlist_directory  "/path/to/mpd-config/playlists"
db_file             "/path/to/mpd-config/mpd.db"
log_file            "/path/to/mpd-config/mpd.log"
pid_file            "/path/to/mpd-config/mpd.pid"
state_file          "/path/to/mpd-config/mpd.state"
bind_to_address     "127.0.0.1"
port                "6600"
audio_output {
    type    "null"
    name    "null output (test)"
}
```

| 项 | 规则 / 作用 |
|---|---|
| `music_directory` | **必须绝对路径 + 正斜杠**（Windows 写 `c:/Music`，不能写 `C:\Music\...`——mpd.conf 里 `\` 是转义符）。Windows 包由 `NormalizeMusicDirectory` 自动规范化（`src/winmain.c:106-175`）。**相对路径不可靠**（官方模板注释也承认「相对路径无法播放」）。 |
| `db_file` | 曲库数据库缓存。⚠️ **改 `music_directory` 后 mpd 不会自动重扫**，旧 DB 仍在 → 表现为"改了目录不生效"。见 §5.1。 |
| `playlist_directory` | mpd **原生歌单**存放目录，aurmpd 的本地歌单功能（§2.7）直接读写这里。 |
| `log_file` | mpd 日志。排错第一现场。 |
| `state_file` | 保存 state/volume/queue（mpd 退出时写；本机测试环境曾出现 `Failed to open ... mpd.state` 的 exception 日志，文件后来才生成）。 |
| `audio_output` | **只有一种输出实例**。Windows 包启动时自动写入 `type "winmm"` + 默认设备名（`src/winmain.c:285-296`）；Linux 测试环境用 `type "null"`（无声卡）。⚠️ 本版**没有**多输出/输出切换的 UI 之外的能力边界说明（后端有 `MPD_API_GET_OUTPUTS`/`TOGGLE_OUTPUT`，但页面无对应入口——**未验证**是否有 UI）。 |

### 3.2 `.mpd` 目录（Windows 包）

Windows 官方包里 `.mpd/` 是 mpd 的数据目录（`playlist_directory`/`db_file`/`log_file` 的相对路径基准）：

- 必须存在 `.mpd/` 与 `.mpd/playlists/`，否则 mpd 打不开 log/database 会直接启动失败、曲库全空。
- zip 解压常丢空目录，因此启动器有 `EnsureDataDirectories()` 兜底创建（`src/winmain.c:538-541`）。
- ⚠️ 官方包与自编译包是否都含该目录，属打包问题（`FIX-PLAN.md` §6 记录过自编译包缺失）。

### 3.3 页面内设置项（Settings 标签页）

| 设置项 | 落点 | 实际生效情况 |
|---|---|---|
| **Subsonic URL / 用户名 / 密码** | `settings.js:149-165`；保存到 localStorage（`:38-45`） | 生效；密码仅保存 md5 token+salt |
| Buffer next track | `settings.js:170-177` | 供已废弃的 HTML5 播放路径用；**mpd 路径不读**（`player.js` 无引用） |
| Enable desktop notifications | `settings.js:178-183` | 由 `playerextra.js:122-138` 消费 |
| Enable background art | `settings.js:184-189` | 由 `playerextra.js:85-117` 消费 |
| **Save queue (persistQueue)** | `settings.js:190-195` | ⚠️ **实际无效**：`persist` prop 只在废弃的 `player_audio.js:34,245` / `player_mpd.js:44,287` 里被读取，现役 `player.js` 完全不使用（grep 无命中）；且保存后 `appSettings` 事件未携带 `persistQueue`（`settings.js:66-71` vs `app.js:95-99`），会把值冲成 `undefined` |
| Repeat queue (legacy) | `settings.js:196-201` | 标签已注明仅 legacy HTML5 播放器；mpd 用播放器条的循环按钮 |
| **语言（language）** | — | 仓库中**不存在**语言/i18n 设置项（仅 `index.html:1` 有 `lang="en"` 属性）。**未验证**/不存在 |

---

## 4. 实现映射

### 4.1 HTTP 路由（全部在 `src/http_server.cpp` 的 `callback_http`）

| 方法 | 路径 | 处理 | 行号 | 响应 |
|---|---|---|---|---|
| GET | `/api/hello` | 内联 | `:136-137` | `{"status":1}` |
| POST | `/api/queue/add/*` | `addTrackTompd` | `:138-150` | 队列 JSON 数组 |
| POST | `/api/queue/add/play` | `addTrackTompd` + `SQ.playSid` | `:147-148` | 同上 |
| POST | `/api/queue/replace/*` | `SQ.clear()` + `addTrackTompd` | `:151-164` | 同上 |
| POST | `/api/queue/replace/play` | 同上 + `SQ.playPos(0)` | `:161-162` | 同上 |
| POST | `/api/queue/del` | `rmTrackBySid`（body `{"queue_sid":N}`） | `:165-173` | 同上 |
| GET | `/api/queue` | `SQ.toJson()` | `:174-177` | 队列 JSON 数组 |
| GET | `/api/library` | `LQ.clear()` + `getMpdDB()` + `allAlbumToJson()` | `:178-183` | 专辑列表（不含 song） |
| POST | `/api/library` | `queryAlbum`（body `{"album":id}`） | `:184-191` | 专辑（含 `song[]`） |
| GET | `/ws` | `mg_ws_upgrade`（在 `src/main.cpp:110-114`） | — | 101 Switching Protocols |
| * | 其余 | `mg_http_serve_dir(root_dir="./htdocs")` | `:192-195` | 静态文件 / 404 |

**实测要点**：
- 路径匹配**尾斜杠敏感**：`POST /api/queue/add`（无尾斜杠）→ **404**（落到静态分支）；带尾斜杠才命中。前端恰好都带尾斜杠（`player.js:210,238,262`；`browser.js:249,278`）。
- 体校验失败 → **400** `{"error":"invalid request body"}`（`replyBadRequest` `:128-131`；实测已确认）。
- ⚠️ **方法不匹配时既不回包、也不返回错误 → 进程 SIGSEGV**（`callback_http` 直接返回，随后 `src/main.cpp:118-120` 的 `MG_INFO` 在空 send 缓冲上做 `c->send.buf + 9` 越界读）。**已实测复现**，见 §5.4。

### 4.2 WebSocket 命令（`MPD_API_*`，客户端 → 服务端）

命令集由 `MPD_CMDS(X)` 宏定义（`src/mpd_client.h:39-74`），解析入口 `get_cmd_id`（`src/mpd_client.c:88-95`）为**最长前缀匹配**（`strncmp`），参数为 `,` 分隔**纯文本**。

| 命令 | 参数 | 动作 | 行号 |
|---|---|---|---|
| `MPD_API_GET_QUEUE` | `,offset` | 返回 `queue` 分页（每页 512） | `:216-219` |
| `MPD_API_GET_BROWSE` | `,offset,path` | 返回 `browse` | `:220-234` |
| `MPD_API_GET_MPDHOST` | — | 返回 `mpdhost` | `:401-405` |
| `MPD_API_GET_DIRBLEAPITOKEN` | — | 返回 `dirbleapitoken`（恒为空串） | `:406-409` |
| `MPD_API_ADD_TRACK` | `,uri` | `mpd_run_add` | `:235-248` |
| `MPD_API_ADD_PLAY_TRACK` | `,uri` | `mpd_run_add_id` + `mpd_run_play_id` | `:249-264` |
| `MPD_API_ADD_PLAYLIST` | `,name` | `mpd_run_load`（**替换队列**） | `:266-280` |
| `MPD_API_PLAY_TRACK` | `,song_id` | `mpd_run_play_id` | `:171-175` |
| `MPD_API_SAVE_QUEUE` | `,name` | `mpd_run_save` → 回 `playlists` | `:282-302` |
| `MPD_API_RM_TRACK` | `,id` | `mpd_run_delete_id` | `:155-158` |
| `MPD_API_RM_RANGE` | `,from,to` | `mpd_run_delete_range` | `:159-162` |
| `MPD_API_RM_ALL` | — | `mpd_run_clear` | `:152-154` |
| `MPD_API_MOVE_TRACK` | `,from,to`（1-based，内部各减 1） | `mpd_run_move` | `:163-170` |
| `MPD_API_SEARCH` | `,query` | 返回 `search`（上限 300） | `:345-358`（上限在 `:936`） |
| `MPD_API_SEND_MESSAGE` | `,channel,msg` | `mpd_run_send_message` | `:359-379` |
| `MPD_API_SET_VOLUME` | `,n`（≤100 才生效） | `mpd_run_set_volume` | `:208-211` |
| `MPD_API_SET_PAUSE` | — | `mpd_run_toggle_pause` | `:137-139` |
| `MPD_API_SET_PLAY` | — | `mpd_run_play` | `:146-148` |
| `MPD_API_SET_STOP` | — | `mpd_run_stop` | `:149-151` |
| `MPD_API_SET_SEEK` | `,id,pos` | `mpd_run_seek_id` | `:212-215` |
| `MPD_API_SET_NEXT` | — | `mpd_run_next` | `:143-145` |
| `MPD_API_SET_PREV` | — | `mpd_run_previous` | `:140-142` |
| `MPD_API_SET_MPDHOST` | `,port,host` | 改 host/port + `MPD_RECONNECT` | `:382-400` |
| `MPD_API_SET_MPDPASS` | `,pass` | 改密码 + `MPD_RECONNECT` | `:410-426` |
| `MPD_API_UPDATE_DB` | — | `mpd_run_update` | `:134-136` |
| `MPD_API_GET_OUTPUTS` | — | 返回 `outputnames` | `:196-199` |
| `MPD_API_TOGGLE_OUTPUT` | `,id,0|1` | enable/disable output | `:200-207` |
| `MPD_API_TOGGLE_RANDOM` | `,0|1` | `mpd_run_random` | `:176-179` |
| `MPD_API_TOGGLE_REPEAT` | `,0|1` | `mpd_run_repeat` | `:180-183` |
| `MPD_API_TOGGLE_CONSUME` | `,0|1` | `mpd_run_consume` | `:184-187` |
| `MPD_API_TOGGLE_SINGLE` | `,0|1` | `mpd_run_single` | `:188-191` |
| `MPD_API_TOGGLE_CROSSFADE` | `,0|1` | `mpd_run_crossfade` | `:192-195` |
| `MPD_API_GET_PLAYLISTS` | — | 返回 `playlists` | `:304-306` |
| `MPD_API_GET_PLAYLIST_SONGS` | `,name` | 返回 `playlist` | `:308-322` |
| `MPD_API_RM_PLAYLIST` | `,name` | `mpd_run_rm` → 回 `playlists` | `:324-344` |

> `SET_MPDHOST`/`GET_MPDHOST`/`SET_MPDPASS`/`GET_DIRBLEAPITOKEN` 位于 `#ifdef WITH_MPD_HOST_CHANGE`（`:380-427`）内，**本机构建启用**（含 `src/config.h:26`；实测 `MPD_API_GET_OUTPUTS` 等均正常）。
> 未识别命令（`get_cmd_id==-1`）**静默忽略**（`:118-119`）。连接未就绪（`conn_state != MPD_CONNECTED`）时，除上述 4 条外**所有命令被直接丢弃且不回包**（`:125-130`）。

### 4.3 JSON 契约（服务端 → 客户端）

| `type` | 产生处 | 字段 |
|---|---|---|
| `state` | `mpd_put_state` `:604-641` | `state`(1停/2播/3暂停), `volume`, `repeat`, `single`, `crossfade`, `consume`, `random`, `songpos`, `elapsedTime`, `totalTime`, `currentsongid`, `queueLength`, `queueVersion` |
| `outputs` | `mpd_put_outputs` `:643-674` | `{"<id>": 0|1}` |
| `outputnames` | 同上（`names=1`） | `{"<id>": "<name>"}` |
| `queue` | `mpd_put_queue` `:702-741` | 数组：`id,pos,duration,title,artist,album` |
| `browse` | `mpd_put_browse` `:743-820` | `{type:"song",uri,duration,title}` / `{type:"directory",dir}` / `{type:"playlist",plist}` / `{type:"wrap",count}` |
| `search` | `mpd_search` `:905-949` | `{type:"song",uri,duration,title,artist,album}` / `{type:"wrap"}`（上限 300） |
| `song_change` | `mpd_put_current_song` `:676-700` | `{pos,title,artist,album}` |
| `update_queue` | `mpd_notify_callback` `:473-478` | 无 data |
| `disconnected` | `mpd_notify_callback` `:461-464` | 无 data |
| `error` | `:430-438` / `mg_ws_send_error` `:98-107` | `{data:"<mpd 错误文本>"}` |
| `playlists` | `mpd_put_playlists` `:824-858` | `[{name,lastmodified}]` |
| `playlist` | `mpd_put_playlist_songs` `:862-903` | `{name,song:[{uri,pos,duration,title,artist,album}]}` |
| `mpdhost` | `:401-405` | `{host,port,passwort_set}` |
| `dirbleapitoken` | `:406-409` | `{data}` |

### 4.4 JSON 契约（客户端 → 服务端 POST body）

`addTrackTompd`（`src/http_server.cpp:47-114`）接受的数组元素字段（`:83-100`）：

| 字段 | 类型 | 必需 | 用途 |
|---|---|---|---|
| `id` | string | 是 | 歌曲 id（本地场景 = 文件路径） |
| `url` | string | 是（否则 mpd add 失败） | **直接作为 mpd uri** |
| `title`/`artist`/`album` | string | 否 | 元数据 |
| `duration`/`track`/`year` | int | 否 | 元数据 |
| `coverArt` | string | 否 | 封面 id |

其余：`/api/queue/del` body `{"queue_sid":<int>}`（`:120`）；`/api/library` POST body `{"album":"<id>"}`（`:40`）。
`/api/queue` 返回元素 = `Song::toJson`（`src/song.cpp:75-90`）：`id,artist,title,album,url,track,year,duration,pos,queue_sid(仅 ≥0),coverArt`。
专辑 JSON = `Album::toJson`（`src/album.cpp:26-45`）/ `albumToJson`（`:47-56`）：`id,artist,name,coverart,year,duration,songCount[,song[]]`。

### 4.5 功能 → 代码位置索引

| 功能 | 前端 | 后端 |
|---|---|---|
| 本地曲库列表 | `browser.js:217-457`（`Library`），`loadAlbums` `:248-264` | `GET /api/library` → `Library::allAlbumToJson` `library.cpp:119-128` |
| 专辑曲目展开 | `fetchAlbum` `browser.js:275-296` | `POST /api/library` → `queryAlbum` `http_server.cpp:36-45` → `AlbumToJson` `library.cpp:131-138` |
| 曲库扫描（自动） | — | `mpd_client.c:520-538` |
| 曲库扫描（手动） | `browser.js:390-394` | `mpd_client.c:134-136` |
| 单曲入队 | `tracklist.js:98-104` | `addTrackTompd` `http_server.cpp:47-114` + `Mpdqueue::addSong` `mpdqueue.cpp:7-21` |
| 批量入队 | `browser.js:363-387` | 同上（一次数组） |
| 队列渲染 | `queue.js:25-44`、`tracklist.js:45-77` | `GET /api/queue` → `Queue::toJson` `queue.cpp:95-104` |
| 点歌播放 | `tracklist.js:90-92` → `player.js:108` | `MPD_API_PLAY_TRACK` → `mpd_client.c:171-175` |
| 播放/暂停/停止/上下首 | `player.js:113-163` | `mpd_client.c:137-151` |
| 音量 | `player.js:165-173` | `mpd_client.c:208-211` |
| 随机 | `player.js:705-748` | `mpd_client.c:176-179` |
| 循环/单曲/消费 | `player.js:318-320`、`PlayerModeButton` `:750-794` | `mpd_client.c:180-191` |
| 本地搜索 | `browser.js:397-414`、输入框 `:109-112` | 纯前端（无后端搜索接口） |
| 本地歌单 全部 | `playlist.js:383-537` | `mpd_client.c:266-344`、`:824-903` |
| 多端同步广播 | `player.js:47-69` | `mpd_notify_callback` `mpd_client.c:458-480` + `MG_EV_WAKEUP` `main.cpp:129-138` |
| WS 连接与排队 | `mpdws.js:13-38` | `/ws` 升级 `main.cpp:110-114` |

### 4.6 实测证据（本机 `:8600` 只读探针）

```bash
$ curl -s http://127.0.0.1:8600/api/hello
{"status":1}

$ curl -s http://127.0.0.1:8600/api/library
[{"artist":"unknown","coverart":"unknown","duration":1,"id":"unknown","name":"unknown","songCount":1,"year":1900}]
# → 印证 §2.1：本机测试曲目缺 album tag，整库塌成一个 id="unknown" 专辑

$ curl -s -X POST -H 'Content-Type: application/json' -d '{"album":"unknown"}' http://127.0.0.1:8600/api/library
{"artist":"unknown",...,"song":[{"album":"unknown","artist":"unknown","coverArt":"","duration":1,
 "id":"TestAlbum/track1.wav","pos":-1,"title":"TestAlbum/track1.wav","track":0,
 "url":"TestAlbum/track1.wav","year":1900}],"songCount":1,"year":1900}
# → 本地专辑确实带 song[]，url 就是 mpd uri（相对 music_directory 的路径）

$ node /tmp/ws.js 'MPD_API_GET_PLAYLISTS'
{"type":"playlists","data":[ {"name":"PartyMix","lastmodified":1789226353}]}

$ node /tmp/ws.js 'MPD_API_GET_PLAYLIST_SONGS,PartyMix'
{"type":"playlist","data":{"name":"PartyMix","song":[ {"uri":"TestAlbum/track1.wav","pos":0,
 "duration":1,"title":"track1.wav","artist":"","album":""}]}}

$ node /tmp/ws.js 'MPD_API_GET_OUTPUTS'
{"type":"outputnames","data":{ "0":"null output (test)"}}

$ node /tmp/wsstate.js
{"type":"state", "data":{ "state":1, "volume":-1, "repeat":0, "single":0, "crossfade":0,
 "consume":0, "random":0,  "songpos": -1, "elapsedTime": 0, "totalTime":0,
 "currentsongid": -1,"queueLength": 0,"queueVersion": 3}}
# → volume:-1 = mpd 在该输出上报告"无音量"（null 输出）；1 Hz state 广播持续到达

$ curl -s -o /dev/null -w '%{http_code}\n' -X POST -d '[]' http://127.0.0.1:8600/api/queue/add
404          # 尾斜杠敏感

$ curl -s -X POST -H 'Content-Type: application/json' -d 'notjson' http://127.0.0.1:8600/api/queue/del
{"error":"invalid request body"}   [HTTP=400]     # 畸形 body 已被安全处理
```

---

## 5. 常见问题排查

### 5.1 曲库为空 / 改了音乐目录不生效

**症状**：页面 Library 是空的（或还是旧歌），`mpc stats` 里 `Songs: 0`；改了 `music_directory` 重启 mpd 依旧。

**原因（按可能性）**：
1. **`db_file` 缓存旧库**：mpd 只在 `db_file` **不存在**时自动扫描。改目录后旧 DB 还在 → 不会重扫。
2. **`.mpd` 目录缺失**：Windows 包解压丢空目录时，mpd 打不开 `log_file`/`db_file` 直接启动失败 → 曲库全空。
3. **`music_directory` 写法错**：不是「绝对路径 + 正斜杠」；Windows 写 `C:\Music` 会被当转义。
4. **mpd 根本没起来 / 端口被占**：见 §5.5。

**解决**：
- aurmpd 现在有兜底：**连接成功后若 `Songs==0` 会自动 `update`**（`src/mpd_client.c:520-538`）；也可在页面点 `Scan library`（`browser.js:390-394`）。
- 手动彻底重来：停 mpd → 删 `db_file`（或 `mpd --clear-db`）→ 重启，mpd 会全量扫描。
- 校验 `music_directory` 为绝对路径 + `/`。
- 确认 `.mpd/` 与 `.mpd/playlists/` 存在（`src/winmain.c:538-541`）。
- 验证：`mpc stats` 的 `Songs` 由 0 变正；`GET /api/library` 返回非 `[]`。

### 5.2 连不上 mpd（`libmpdclient-2.dll` 版本问题）

**症状**：Windows 下 aurmpd 起来但永远连不上 6600，或启动即报 DLL 相关错误。

**原因**：Windows 需要把 `libmpdclient-2.dll`（编译 libmpdclient 时生成的）放到执行文件目录；`README.md` 的编译章节明确要求手动复制。该 DLL 与编译时用的 libmpdclient 版本/ABI 必须匹配，否则加载失败或行为异常。

**解决**：确认 `winaurmpd.exe`/`aurmpd.exe` 同目录存在匹配版本的 `libmpdclient-2.dll`；用 `winaurmpd.exe -d` 看控制台错误。**未验证**具体版本号要求（README 未给出）。

### 5.3 页面白屏（WebSocket 未连接就 send）

**症状**：打开页面一片空白，浏览器控制台报 `InvalidStateError: Sent before connected`。

**原因**：旧的 `socket.send(...)` 在 socket 仍处于 `CONNECTING` 时被调用会**抛异常**，导致 Preact 首次渲染中断 → 白屏。

**解决**：已修复——所有发送统一走 `sendCommand()`（`aurial/src/js/mpdws.js:30-38`）：`OPEN` 直接发、`CONNECTING` 入队、其它状态只 `console.error` 丢弃。修复后即使后端重启，UI 也只是"卡住/丢命令"而不会崩。

### 5.4 方法不匹配的请求会让 aurmpd 崩溃（本轮实测新发现）

**症状**：向任一 `/api/*` 路由用**错误的方法**发一次请求（例如 `GET /api/queue/del`、`GET /api/queue/add/`），aurmpd **立刻消失**，`curl` 表现为 `empty reply from server`（exit 52），端口 8600 消失。

**原因**：`callback_http` 里方法不匹配时**既不回包也不报错，直接返回**（`src/http_server.cpp:166`、`:175` 等）。随后主循环无条件执行日志：
```c
MG_INFO(("%.*s %.*s %lu -> %.*s %lu", hm->method.len, hm->method.buf,
        hm->uri.len, hm->uri.buf, hm->body.len, 3, c->send.buf + 9, c->send.len));
```
（`src/main.cpp:118-120`）——在**没有响应写入**、send 缓冲为空的情况下做 `c->send.buf + 9` 的长度读取，越界访问 → **SIGSEGV**。

**实测复现**（只读请求，未改任何状态）：
```
$ curl -s -o /dev/null -w 'HTTP=%{http_code}\n' http://127.0.0.1:8600/api/queue/del
HTTP=000     # empty reply，随后 8600 端口消失
$ curl -s http://127.0.0.1:8600/api/hello
curl: (7) Failed to connect           # 进程已死
# 启动侧日志：Segmentation fault (core dumped) setsid ./aurmpd
```
`GET /api/queue/add/` 同样复现。

**解决（临时）**：只能用正确方法访问接口；局域网内任何人不小心用错方法即可让服务下线——这是**远程 DoS**。真正修法有两处：方法不匹配时补一个 `405`/`404` 响应（`http_server.cpp` 各分支的 `else`），以及修掉 `main.cpp:118-120` 的越界日志。

> 说明：`ANALYSIS.md` §4.2 C1 描述的"畸形 body 致 `json::type_error` 崩溃"在当前工作区**已修复**（`parseBody` 现在 `catch (const json::exception&)`，`http_server.cpp:24-33`；实测畸形 body 返回 400 且进程存活）。本条 SIGSEGV 是**另一处**、且更容易触发。

### 5.5 托盘退出后进程残留删不掉（非 volatile 退出标志 + 持锁提前 return 死锁）

**症状**：Windows 点托盘 `Exit` 后，`aurmpd.exe`/`mpd.exe` 仍在，占着安装目录，导致无法删除/覆盖。

**原因（两层，都已在代码里留下修复痕迹）**：
1. **退出标志未 `volatile`**：`force_exit` 被信号处理器/多线程读写，若声明为普通 `int`，`-O3` 下编译器可能把读取优化进寄存器 → 其它线程写的 1 永远读不到 → 「收到 CLOSE 却不退出」。现声明为 `volatile sig_atomic_t`（`src/main.cpp:26-30`，注释即此问题）。
2. **持锁提前 return 造成死锁**：`mpd_poll` 在 `MPD_DISCONNECTED` 分支若 `mpd_connection_new` 失败直接 `return`，而该分支**已持有 `mpd_lock`** → 锁永不释放 → 后续 `mpd_clear_all` 永久阻塞 → 进程退不掉。现改为统一 `goto out` 出口释放锁（`src/mpd_client.c:491-497`、`:563-566`）。

**解决/兜底**：
- 管道线程收到 `CLOSE` 后，除置标志外再 `sleep(2)` 然后 `ExitProcess(0)`（`src/main.cpp:78-87`）。
- 启动器侧先发管道 `CLOSE`，`WaitForSingleObject` 最多 3s，超时 `TerminateProcess` 并再等一次（`src/winmain.c:408-434`）。
- Job 对象 `KILL_ON_JOB_CLOSE` 保证启动器无论怎么死都连带结束子进程（`src/winmain.c:45-61`）。
- 排错：`winaurmpd.exe -d` 观察是否打印 "Received CLOSE command, exiting loop."（`src/winpipe.cpp:39-40`）。

### 5.6 端口 8600 / 6600 被占用

**症状**：aurmpd 启动即 `Cannot listen on 0.0.0.0:8600` 并退出（`src/main.cpp:166-170`）；或 mpd 起来但连不上（6600 上跑的是**别的** mpd 实例、配置不是你以为的那份）。

**原因**：有旧进程残留；或从不同目录启动了第二个 mpd，各自读不同的 `mpd.conf`（相对路径问题，`FIX-PLAN.md` §3 记录过）。

**解决**：
```bash
ss -ltnp | grep -E '8600|6600'      # 找占用者
mpc -h 127.0.0.1 -p 6600 stats      # 确认 6600 上是哪个 mpd、曲库对不对
```
先结束残留进程再启动；Windows 下确认 `winaurmpd.exe` 的工作目录被修正（`src/winmain.c:551-553`），避免"启动了两份 mpd、读了两份配置"。

---

## 6. 功能边界与已知限制

1. **无鉴权 + 绑定全网卡（刻意取舍）**：`0.0.0.0:8600`（`src/main.cpp:22`），HTTP 与 WS 均无任何认证/Origin 校验（`http_server.cpp`、`main.cpp:110-114`）。面向"局域网公共点歌机"是设计选择，但意味着**同一网段任何人都能完整控制播放、读曲库、枚举 mpd 输出，甚至切换 mpd 后端地址 / 设置 mpd 密码**（`MPD_API_SET_MPDHOST`/`SET_MPDPASS` 因 `WITH_MPD_HOST_CHANGE` 启用而在线可用，`src/mpd_client.c:382-426`）。
2. **任意 URI 注入**：客户端 POST 的 `url` 被原样当 mpd uri（`http_server.cpp:101-106` → `mpdqueue.cpp:10`）。可以让 mpd 把任意路径当音频打开——仅限可信局域网。
3. **mpd 单输出**：本机测试环境只有一个 `audio_output`（`type "null"`）。后端有 `MPD_API_GET_OUTPUTS`/`TOGGLE_OUTPUT`（`mpd_client.c:196-207`），但**页面上没有输出切换入口**（**未验证**是否存在隐藏入口）。Windows 包启动时生成的是系统默认输出（`src/winmain.c:285-296`）。
4. **Subsonic 相关功能在本地场景不可用**：艺术家/专辑浏览、服务器端歌单、搜索、scrobble 全部依赖 Subsonic 服务器；未配置时这些区域不渲染（`browser.js:63-90`、`playlist.js:184-187`），scrobble 静默不生效。
5. **`persistQueue` 设置实际无效**：现役 `player.js` 不消费 `persist` prop（grep 无命中；仅存在废弃的 `player_audio.js`/`player_mpd.js` 中），且保存设置时 `appSettings` 未带 `persistQueue`（`settings.js:66-71` vs `app.js:95-99`）。**队列持久化目前不可用**。
6. **退出即清空队列**：`mpd_clear_all()`（`mpd_client.c:958-984`）在退出时停止播放、清空队列。重启后播放队列丢失，与「队列放后台」的卖点冲突。且只处理 `SIGINT`（`src/main.cpp:164`），`kill`/`systemd stop` 走不到清理路径。
7. **专辑分组依赖 album tag**：tag 缺失时整库塌成一个 `unknown` 专辑（`library.cpp:75`；本机实测即如此）。`ANALYSIS.md`/`FIX-PLAN.md` 建议改为 `album_artist+album` 或回退目录名，**尚未落地**。
8. **搜索的曲目名匹配依赖缓存**：只对"已点开过"的专辑生效（`browser.js:397-414` 读 `albumCache`），因为 `GET /api/library` 列表不含 `song[]`。
9. **移动端不兼容**：无 `viewport` meta（`aurial/src/index.html`），布局为固定像素 + `min-width:1000px` 断点；音量条只用 mouse 事件（`player.js` 音量组件）。`README.md`「问题 5」也承认"手机浏览器端不兼容"。
10. **UI 依赖 CDN**：semantic-ui 从 `cdnjs.cloudflare.com` 加载（`src/index.html:8-9`），离线/无外网局域网下样式与组件失效。
11. **前端产物未压缩**：uglify 被注释（`webpack.dist.config.js:19-26`），`index.js` ~0.9 MB。
12. **前端仓库含死代码**：`jsx/player_mpd.js`、`jsx/player_audio.js`、`jsx/browser_bak.js` 未被现役入口引用（`app.js:4` 只引 `./player`），且 `player_mpd.js` 引用了不存在的 `../soaprequest.js`——一旦被引入即构建失败。
13. **`diagnose.cmd` 不存在**：全仓无 `*.cmd`/`diagnose*` 文件；`test/` 仅有一个 `README.TXT`。**未验证**。
14. **未验证项汇总**：手动 `Scan library` 是否有"扫描完成"回执（**未验证**）；Windows `libmpdclient-2.dll` 的确切版本要求（**未验证**）；是否存在输出切换 UI（**未验证**）；语言/i18n 设置（不存在）。

---

## 7. 更新记录（第六、七批）

### 7.1 播放交互补全
| 功能 | 用法 | 实现位置 |
| --- | --- | --- |
| **进度条点击/拖动跳转** | 点进度条任意位置或按住拖动；拖动只做本地预览，**松手才发一次** `MPD_API_SET_SEEK` | `aurial/src/js/jsx/player.js`（进度组件）；后端 `src/mpd_client.c` 的 `MPD_API_SET_SEEK` 分支 |
| **音量条支持触摸** | 鼠标与手指都可拖动（Pointer Events + `setPointerCapture`） | `jsx/player.js` 的 `PlayerVolume` |
| **手机端适配** | 加了 viewport meta 与响应式布局：窄屏上下堆叠、表格内部横滚、触控目标 ≥40px | `aurial/src/index.html`、`aurial/src/css/default.css` |
| **曲库关键词检索（直接出曲目）** | 左侧搜索框输入关键词，**直接列出匹配曲目行**（带 ▶ / ＋）+ 匹配专辑分组 | `jsx/browser.js` 的 `filteredAlbums()` / `matchingTracks` 逻辑 |

### 7.2 设置页清理
- 删除 `Save queue`（实测无效）与 `Repeat queue`（旧版遗留，已由播放器的**循环按钮**取代）。

### 7.3 在线（Subsonic）艺术家区
- **未配置**（`url` 为空）时**整块不渲染**（此前 `js/index.js` 默认填了演示服务器 `https://demo.navidrome.org`，导致"未配置"判断永不成立、页面出现"艺术家加载失败，请检查设置"。现已移除该默认值）。
- **已配置但连不上**时收进与本地曲库同款的**可折叠块**（默认收起），块内只显示一行淡色提示，不再用整块错误面板替换页面。

### 7.4 开机自启（Windows）
| 入口 | 说明 |
| --- | --- |
| 托盘右键 | 可勾选的「开机自启 / Auto start」，每次打开菜单读真实状态 |
| 设置页 | 「开机自启 / Auto start」开关；**后端不支持（Linux）时整项不渲染** |
| 存储 | 当前用户注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，值名 `aurmpd`，内容 `"<程序目录>\winaurmpd.exe"`（无需管理员权限） |
| 路径自愈 | 启动器启动时若该项已存在，会刷新为当前实际路径（换目录后不会静默失效）；未启用则不会擅自开启 |
| 命令 | `MPD_API_GET_AUTOSTART` → `{"type":"autostart","data":{"supported":bool,"enabled":bool}}`；`MPD_API_SET_AUTOSTART,0\|1` 执行后回同样 JSON |
| 实现位置 | 后端 `src/mpd_client.c`（`AUTOSTART_RUN_KEY` / `autostart_get_enabled` / `autostart_set_enabled`）；启动器 `src/winmain.c`（`IDM_AUTOSTART`） |

> 验证边界：注册表读写已在 wine 环境实测（开→值写入、关→值消失）；**真实开机自启流程需在真机验证**（勾选后注销/重启，看托盘是否自动出现）。
