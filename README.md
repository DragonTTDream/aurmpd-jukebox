# aurmpd · 本地曲库点歌机版

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/DragonTTDream/aurmpd-jukebox?label=release)](https://github.com/DragonTTDream/aurmpd-jukebox/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational)](#下载)
[![Based on](https://img.shields.io/badge/based%20on-breezecloud%2Faurmpd-lightgrey)](https://github.com/breezecloud/aurmpd)

局域网内多人同时点歌的音乐播放器：**任何设备打开网页，看到的都是同一份播放队列、同一套控制**。

浏览器界面负责浏览与点歌，播放由后台的 [mpd](https://www.musicpd.org/) 负责——
关掉浏览器音乐不会停，多台设备之间状态实时同步。

> **来源**：本项目是 [breezecloud/aurmpd](https://github.com/breezecloud/aurmpd)（GPL-2.0）的修改版，同样以 **GPL-2.0** 发布。
> 上游 README 原文保留在 [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md)　·　本仓库：<https://github.com/DragonTTDream/aurmpd-jukebox>

![主界面](docs/screenshots/01-main.png)

## 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| **Windows x64** | `aurmpd-1.0.0-win64.zip` | **推荐**：解压即用，自带 mpd 与托盘启动器 |
| **Linux x86_64** | `aurmpd-1.0.0-linux-x86_64.tar.gz` | 需自行安装并运行 mpd |

发布页：<https://github.com/DragonTTDream/aurmpd-jukebox/releases/latest>

## 功能

### 播放与队列

- [x] 同一个 mpd 队列，**多终端共享**：谁都能暂停、切歌、点歌，状态实时同步
- [x] 队列不随浏览器关闭而丢失；aurmpd 重启后自动从 mpd 恢复
- [x] **循环 / 单曲 / 消费** 播放模式（多端状态同步）
- [x] **进度条点击或拖动跳转**（拖动只做本地预览，松手才提交一次）
- [x] 音量 / 随机 / 上一首 / 下一首；音量条支持**触摸**

### 曲库与点歌

- [x] 本地曲库按专辑浏览，**点专辑标题就地展开曲目**（无需跳页）
- [x] **关键词检索**：直接列出匹配的**曲目行**，可一键播放 / 加入队列
- [x] **批量点歌**：勾选 + 全选 + 批量加入队列
- [x] 专辑行内一键「播放全部 / 加入队列」
- [x] **曲库扫描**：新增歌曲后手动重扫（空库时自动扫描一次）

### 歌单与设置

- [x] **本地歌单**：把当前队列存成歌单 / 列出 / 查看 / 加载 / 删除（mpd 原生歌单）
- [x] 清空队列、从队列移除单曲
- [x] **中英双语**界面（设置页切换，即时生效）
- [x] Windows：**开机自启**（托盘右键 + 设置页开关，写入当前用户注册表，无需管理员权限）

### 其他

- [x] 手机端响应式布局（窄屏上下堆叠、表格内部横滚、触控目标 ≥40px）
- [x] 页面无登录鉴权，面向可信局域网（客厅、店铺等）的公共点歌场景

## 两个版本的区别

前端功能**完全相同**，差异在运行方式与系统集成：

| 能力 | Windows 版 | Linux 版 |
| --- | --- | --- |
| 内置 mpd（一键启动） | ✅ 自带 `mpd.exe` 0.23.9 + `libmpdclient-2.dll` | ❌ 需自行安装并启动 mpd |
| 启动方式 | 双击 `winaurmpd.exe`：后台运行 + 托盘图标，自动拉起 mpd 与 aurmpd | `./aurmpd`（前台运行，建议 systemd 托管） |
| 系统托盘图标 | ✅ | ❌ |
| 开机自启 | ✅ 托盘右键 + 设置页开关（写 `HKCU\...\Run`，无需管理员权限） | ❌ 设置页自动隐藏该项；请用 `systemctl enable` |
| 首次配置 | 首次运行自动生成 `mpd.conf`；`music_directory` 的反斜杠自动转正斜杠 | 自行编写 `mpd.conf`（路径用正斜杠；包内附 `mpd.conf.sample`） |
| 退出方式 | 托盘右键 → Exit（连带结束 mpd 与 aurmpd） | `Ctrl+C` / `systemctl stop` |
| 网络与播放 | `0.0.0.0:8600`，多终端共享同一队列 | 相同 |

<details>
<summary><strong>Linux：用 systemd 做开机自启</strong></summary>

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
</details>

## 快速开始

### Windows

1. 解压到**无中文、无空格**的目录（不要覆盖旧目录）；
2. 双击 `winaurmpd.exe` → 托盘出现图标，自动拉起 mpd 与 aurmpd；
3. 浏览器打开 `http://127.0.0.1:8600`（局域网用 `http://<本机IP>:8600`），首次按 **Ctrl+F5 强刷**；
4. 编辑 `mpd.conf` 的 `music_directory` 指向音乐目录，再用曲库面板的 **Scan library** 扫描。

### Linux

```bash
# 1) 安装并启动 mpd（以 Debian/Ubuntu 为例）
sudo apt install mpd libmpdclient2
# 2) 解压发布包后运行
./aurmpd          # 默认监听 0.0.0.0:8600
```

## 配置要点

| 项 | 说明 |
| --- | --- |
| `music_directory` | **绝对路径 + 正斜杠**，如 `music_directory "D:/Music"`（Windows 启动器也会自动把 `\` 转 `/`） |
| `.mpd/` 目录（Windows 包） | mpd 的数据库 / 日志 / 歌单目录，**不要删除**（缺失会导致 mpd 启动失败、曲库为空） |
| 改目录后曲库没更新 | 点曲库面板 **Scan library**；空曲库时程序会自动扫描一次 |
| 页面无鉴权 | 仅建议在**可信局域网**内使用；如需公网访问请自行加反向代理与认证 |

## 修复记录

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 页面白屏，本地/局域网都打不开 | 前端在 WebSocket 连接建立前就 `send()`，抛 `InvalidStateError` 中断整页渲染 | 改为带发送队列的封装：连接前排队、断线不抛异常 |
| 曲库为空 / mpd 起不来 | 发布包缺少 `.mpd` 目录，mpd 无法创建日志与数据库文件 | 包内补齐该目录；启动器额外兜底创建 |
| 改了音乐目录不生效 | mpd 只在数据库文件缺失时才自动扫描 | 新增空库自动扫描 + 曲库面板 `Scan library` 手动重扫 |
| 托盘退出后有残留进程，目录删不掉 | 退出标志未声明 `volatile`（优化后多线程读不到）；加锁区内的提前 `return` 未释放互斥锁，退出清理永久阻塞；启动器发出关闭指令后不等待结果 | 修正标志类型与解锁路径；启动器改为「等待 → 超时强杀 → 再等待」，并引入 Job 对象保证子进程一并结束 |
| 重开页面后失去播放记录、无法控制 | 队列只保存在应用内存中，重启后为空，与 mpd 实际队列不一致 | 内部队列改为 mpd 队列的镜像，连接/重连与队列变化时自动同步；`queue_sid` 直接采用 mpd 歌曲 id |
| 错误方法请求接口导致进程崩溃 | 方法不匹配时不返回响应，随后日志读取空的响应缓冲造成越界 | 已知路由方法不匹配返回 **405**；畸形 JSON 返回 **400** |
| 多人同时点歌时断连 / 丢命令 | mpd 连接被多个线程无锁共享 | 增加递归互斥锁，写路径与轮询串行化 |
| 搜索框一直转圈 | 搜索请求走在线曲库接口，失败时不结束也不报错 | 改为过滤本地曲库，并为所有请求加超时与错误透传 |
| 扫描含异常标签的曲库时崩溃 | 标签为空或非数字时 `std::stoi` 抛异常 | 改为安全解析（空值/非法值取默认值） |
| 艺术家区显示「加载失败，请检查设置」 | 默认配置指向一个演示用的在线服务器，导致「未配置」判断不成立 | 移除该默认值；未配置时整个区域不渲染，配置了但连不上时折叠显示一行提示 |

## 从源码构建

<details>
<summary><strong>后端（C/C++）</strong></summary>

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)          # 产物：build/aurmpd
```

Windows 交叉编译需 mingw-w64 与 Windows 版 libmpdclient，产物为 `aurmpd.exe` 与 `winaurmpd.exe`。
</details>

<details>
<summary><strong>前端（preact + semantic-ui）</strong></summary>

```bash
cd aurial
NODE_OPTIONS=--openssl-legacy-provider npm run dist   # 产物直出 ../build/htdocs
```

> 该前端是 2017 年前后的技术栈（webpack 2 / babel 6），较新的 Node 需要 `--openssl-legacy-provider`。
</details>

## 界面

| 曲库（检索 / 批量点歌 / 扫描） | 设置（双语 / 开机自启 / 关于） |
| --- | --- |
| ![曲库](docs/screenshots/02-library.png) | ![设置](docs/screenshots/03-settings-zh.png) |

| 队列 | 手机端 |
| --- | --- |
| ![队列](docs/screenshots/04-queue.png) | ![手机](docs/screenshots/06-mobile.png) |

> 每个页面/分栏的用途、HTTP 与 WebSocket 接口清单、排错手册：**[docs/FEATURES.md](docs/FEATURES.md)**

## 来源与许可

- **上游**：[breezecloud/aurmpd](https://github.com/breezecloud/aurmpd)（GPL-2.0）—— 本项目的直接来源
- **本仓库**：[DragonTTDream/aurmpd-jukebox](https://github.com/DragonTTDream/aurmpd-jukebox)（同样 **GPL-2.0**，保留上游版权声明与 `LICENSE`）
- **致谢**：[aurial](https://github.com/shrimpza/aurial)（前端原型）、[mpd](https://www.musicpd.org/) / [libmpdclient](https://www.musicpd.org/libs/libmpdclient/)（播放与控制）、[mongoose](https://mongoose.ws)（网络层）
