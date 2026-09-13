# aurmpd · 本地曲库点歌机版

> **本仓库基于 [breezecloud/aurmpd](https://github.com/breezecloud/aurmpd) 修改**（上游采用 GPL-2.0，本仓库同样以 GPL-2.0 发布）。
> 上游原版是「浏览器界面 + mpd 播放」；**本分支把它做成一台真正的公共点歌机**：局域网内任何设备打开网页，看到的都是**同一份播放队列、同一套控制**，谁都能暂停、切歌、点歌。
>
> 上游 README 原文完整保留在 [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md)，以明确来源与传承。

![主界面](docs/screenshots/01-main.png)

## 这是什么

- **后端**：`aurmpd`（C/C++）内嵌 [mongoose](https://mongoose.ws) HTTP/WebSocket 服务，通过 `libmpdclient` 控制 [mpd](https://www.musicpd.org/)；mpd 负责扫描本地音乐库与解码播放。
- **前端**：改造自 [aurial](https://github.com/shrimpza/aurial) 的网页界面（preact + semantic-ui），通过 REST + WebSocket 与后端通信。
- **形态**：Windows 下打包 `winaurmpd.exe`（托盘启动器）+ `mpd.exe`，解压即用；Linux 下只需本机装好 mpd。

## 相比上游的主要改动

### 修复（上游版本实际会踩到的问题）

| 问题 | 根因 |
| --- | --- |
| 页面**白屏**（本地/局域网都打不开） | 前端在 WebSocket 连接建立前就 `send()`，抛 `InvalidStateError` 中断整页渲染；改为**带发送队列**的封装 |
| **曲库读不出来**（换了配置也没用） | 压缩包缺少 `.mpd` 空目录 → mpd 打不开 `.mpd/log` 直接启动失败；已在包内补齐并由启动器兜底创建 |
| 改音乐目录**不生效** | mpd 不会自动重扫旧数据库；新增**空库自动扫描**与曲库面板 `Scan library` 手动重扫 |
| 托盘退出后**进程残留**、目录删不掉 | 退出标志非 `volatile`（`-O3` 下多线程读不到）+ 加锁区提前 `return` 未解锁导致退出清理永久阻塞；已修，并引入 Job 对象保证子进程一并结束 |
| 重开页面**失去播放记录、无法控制** | 队列的"真相"原本只在应用内存里；现改为 **mpd 是队列的唯一真相**，启动/重连时自动镜像同步 |
| 错误方法请求接口**打崩进程** | 方法不匹配时不回包，随后日志越界读取响应缓冲；现返回 **405**，畸形 JSON 返回 **400** |
| 多人同时点歌**断连/丢命令** | mpd 连接被多线程无共享锁；已加递归互斥锁 |
| 界面**搜索框一直转圈** | 搜索走的是 Subsonic 接口且失败不结束；现改为过滤**本地曲库**，并给所有请求加超时与错误透传 |
| 曲库读取时**崩溃** | 音频标签为空/非数字时 `std::stoi` 抛异常；已改为安全解析 |
| 艺术家区显示「**加载失败，请检查设置**」 | 程序默认填了演示 Subsonic 服务器，导致"未配置"判断永不成立；现未配置时**整块不渲染** |

### 新增功能

- **共享队列语义**：多终端同一队列、同一控制，重启不丢（本项目的核心目标）
- **循环 / 单曲 / 消费**播放模式按钮（多端状态同步）
- **本地歌单**：保存当前队列 / 列出 / 查看 / 加载 / 删除（走 mpd 原生歌单）
- **曲库关键词检索**：直接列出匹配的**曲目行**（带播放 / 入队），并可一键加入队列
- **批量点歌**：勾选 + 全选 + 批量加入队列
- **专辑行内展开**：点专辑标题就地展开曲目，无需跳页
- **进度条点击/拖动跳转**、**音量条支持触摸**、**手机端响应式布局**
- **中英双语**界面（设置页切换，即时生效）
- **开机自启**：托盘右键 + 设置页开关（写入当前用户注册表，无需管理员权限）

### 打包与工程

- Windows 交叉编译（mingw-w64）产物：`aurmpd.exe` 静态链接，仅依赖 `libmpdclient-2.dll`
- 附带 `mpd.conf.tmp` 模板、`.mpd/` 数据目录、诊断脚本
- Windows 启动器：固定工作目录、子进程用绝对路径拉起、`music_directory` 自动把 `\` 规范化为 `/`

## 界面一览

| 曲库（含检索、批量点歌、扫描） | 设置（中英双语、开机自启） |
| --- | --- |
| ![曲库](docs/screenshots/02-library.png) | ![设置](docs/screenshots/03-settings-zh.png) |

![队列](docs/screenshots/04-queue.png)

> 每个页面/分栏的详细说明、HTTP 与 WebSocket 接口清单、排错手册，见 **[docs/FEATURES.md](docs/FEATURES.md)**。

## 两个版本的区别（Windows / Linux）

> 前端功能**完全相同**（中英双语、本地歌单、曲库检索、批量点歌、进度条跳转、手机端适配…），
> 差异都在「运行方式」与「系统集成」上：

| 能力 | **Windows 版**（`aurmpd-1.0.0-win64.zip`） | **Linux 版**（`aurmpd-1.0.0-linux-x86_64.tar.gz`） |
| --- | --- | --- |
| 一键启动（内含 mpd） | ✅ 自带 `mpd.exe`（0.23.9）与 `libmpdclient-2.dll` | ❌ 需自行安装并启动 mpd（`apt install mpd` 等） |
| 启动方式 | 双击 `winaurmpd.exe`：后台运行 + 系统托盘图标，自动拉起 mpd 与 aurmpd | `./aurmpd`（前台运行，推荐用 systemd 托管） |
| 系统托盘图标 | ✅ 可右键退出 / 开机自启 | ❌ 无 |
| **开机自启** | ✅ **托盘右键**「开机自启」+ **设置页开关**（写当前用户注册表 `HKCU\...\Run`，**无需管理员权限**） | ❌ **不支持**（设置页会自动隐藏这一项）；请用 `systemctl enable` 托管服务 |
| 首次配置 | 首次运行自动生成 `mpd.conf`；`music_directory` 的 `\` 会自动转成 `/` | 自行编写 `mpd.conf`（路径用**正斜杠**） |
| 退出方式 | 托盘右键 → Exit（会连带结束 mpd 与 aurmpd） | `Ctrl+C` / `systemctl stop` |
| 目录依赖 | 可任意解压目录（启动器会固定工作目录） | 建议从解压目录启动，或显式指定 `htdocs` 根目录 |
| 端口与播放 | `0.0.0.0:8600`，多终端共享同一队列 | 同上，完全一致 |
| 页面无鉴权 | 是（公共点歌机取舍，请只在可信局域网） | 同上 |

**Linux 开机自启的正确做法**（系统级服务，比 Windows 注册表方式更规范）：

```ini
# /etc/systemd/system/aurmpd.service
[Unit]
Description=aurmpd local jukebox
After=mpd.service network.target
Wants=mpd.service

[Service]
ExecStart=/opt/aurmpd/aurmpd
WorkingDirectory=/opt/aurmpd
Restart=on-failure
User=mpd

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now aurmpd
```

## 快速开始

### Windows（推荐，开箱即用）

1. 解压发布包到**无中文、无空格**的目录；
2. 双击 `winaurmpd.exe`：托盘出现图标，自动拉起 mpd 与 aurmpd；
3. 浏览器打开 `http://127.0.0.1:8600`（局域网内用 `http://<本机IP>:8600`），首次请 **Ctrl+F5 强刷**；
4. 在 `mpd.conf` 里把 `music_directory` 指向你的音乐目录，然后用曲库面板的 **Scan library** 扫描（或重启程序）。

### Linux

1. 安装并启动 mpd（`libmpdclient` 开发库需可用）；
2. 构建：`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)`；
3. 前端：`cd aurial && NODE_OPTIONS=--openssl-legacy-provider npm run dist`；
4. 运行：`cd build && ./aurmpd`（默认监听 `0.0.0.0:8600`，请只在可信网络暴露）。

## 配置要点

| 项 | 说明 |
| --- | --- |
| `music_directory` | 用**绝对路径 + 正斜杠**，如 `music_directory "D:/Music"`（启动器也会自动把 `\` 转成 `/`） |
| `.mpd/` 目录 | mpd 的数据库/日志/歌单都放这里，**不要删除**（缺失会导致 mpd 启动失败、曲库全空） |
| 改目录后没生效 | 点曲库面板的 **Scan library**；空曲库时程序也会自动扫描一次 |
| 安全 | 页面**无登录鉴权**——这是"公共点歌机"的有意取舍，请只在可信局域网内使用 |

## 来源与许可

- **上游项目**：[breezecloud/aurmpd](https://github.com/breezecloud/aurmpd) ，License: **GPL-2.0**
- **本仓库**：在上游 `v0.2.0` 基础上修改（详见上文「相比上游的主要改动」），同样以 **GPL-2.0** 发布，并保留上游全部版权声明与 `LICENSE`。
- 上游 README 原文： [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md)

### 致谢

- [breezecloud/aurmpd](https://github.com/breezecloud/aurmpd) —— 本项目的直接来源
- [aurial](https://github.com/shrimpza/aurial) —— 前端界面原型
- [mpd](https://www.musicpd.org/) / [libmpdclient](https://www.musicpd.org/libs/libmpdclient/) / [mongoose](https://mongoose.ws) —— 播放、控制与网络层
