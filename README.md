# 第三方极域教师端 / Third-party JiYu Teacher Endpoint

[![GitHub stars](https://img.shields.io/github/stars/yunsjxh/Third-party-JiYu-Teacher-Endpoint?style=social)](https://github.com/yunsjxh/Third-party-JiYu-Teacher-Endpoint/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/yunsjxh/Third-party-JiYu-Teacher-Endpoint?style=social)](https://github.com/yunsjxh/Third-party-JiYu-Teacher-Endpoint/network/members)

一个用 Python 实现的**第三方极域课堂管理系统 V6.0 教师端模拟器**。本项目基于对官方教师端与学生端可执行文件的 **IDA Pro 静态逆向分析**以及真实网络抓包，还原出教师端与学生端之间的 UDP 通信协议，从而让学生端将本机识别为合法教师机，并接收登录、保活、控制请求、屏幕缩略图以及学生端主动上报的状态/消息等数据。

> 本项目仅用于协议学习、网络安全研究与教学演示。**请在合法授权范围内使用，严禁用于未经许可的监控、入侵或干扰他人设备。**

## 项目网站

仓库内已包含一个静态项目网站，直接用浏览器打开 [`index.html`](./index.html) 即可预览。

---

## 项目背景

本项目的协议字段、包结构、握手时序以及教师端 GUID 等信息，主要来源于：

1. **IDA Pro 静态逆向分析**官方教师端与学生端可执行文件，提取协议魔数（Magic）、版本号、字段偏移与关键常量；
2. 在隔离实验环境中抓取教师端与学生端之间的真实 UDP 多播/单播报文，进行逐字节比对与验证；
3. 将还原出的二进制协议用 Python 重新实现，使其能在不依赖官方客户端的情况下模拟教师端行为。

因此，本仓库既是一个可运行的教师端模拟器，也是一份关于极域 V6.0 教师-学生通信协议的公开技术笔记。

---

## 功能特性

- **教师端在线宣告**：周期性发送 `OONC` / `CANC` / `ANNO` 广播，使学生端发现本机。
- **学生端上线处理**：响应 `LOGI` 登录宣告，回送 `MESS`、`LPNT`、`DMOC` 等握手包。
- **保活与发现**：处理 `KACA` 保活请求并回送 `WACA`；响应 `TRMC`、`DENT` 等控制/注册包。
- **缩略图接收与重组**：接收学生端分片发送的 `LANT`/`TNAL` 预览图数据，拼接成完整 JPEG。
- **图像修正**：针对极域学生端截图的 bottom-up DIB 与 YCbCr 色度反相特性，生成正向、颜色正常的修复图。
- **接收学生端消息与状态**：解析学生端通过 `MESS` 上报的聊天消息、当前活动窗口标题、WiFi 可用网络数量、IE/浏览器 URL 信息、系统性能数据等。
- **逐字节还原**：包结构参照真实教师端抓包，保留原始 GUID、Magic、版本号与尾部字段。

---

## 功能展示与路线图

| 功能 | 状态 | 说明 |
|------|------|------|
| 教师端在线宣告（`OONC` / `CANC` / `ANNO`） | ✅ 已实现 | 周期性多播广播，让学生端发现本机 |
| 学生登录握手（`LOGI` → `MESS` / `LPNT` / `DMOC`） | ✅ 已实现 | 登录后立即回送握手包建立连接 |
| 保活与发现（`KACA` / `WACA` / `TRMC` / `DENT`） | ✅ 已实现 | 维持学生端在线状态 |
| 缩略图接收与重组（`LANT` / `TNAL`） | ✅ 已实现 | 分片拼接并保存原始图与颜色修正图 |
| 学生聊天消息接收（`MESS` subtype `0x800`） | ✅ 已实现 | 控制台显示 `[学生消息]` |
| 学生状态接收（窗口标题 / WiFi / 系统信息） | ✅ 已实现 | 控制台显示 `[学生状态]` |
| *向学生端发送聊天消息* | *🚧 待实现* | 需构造 `MESS` subtype `0x800` 发送包 |
| *远程命令 / 锁屏 / 解锁控制* | *🚧 待实现* | 基于 `CCommandPacketHandler` 命令通道 |
| *文件传输 / 块数据接收（`CBlockPacketHandler`）* | *🚧 待实现* | 大块数据分片协议 |
| *教师退出 / 断连处理（`CTeacherExitPacketHandler`）* | *🚧 待实现* | 优雅断开与重连机制 |
| *语音教学 / 分组教学* | *🚧 待实现* | 对应 IDA 中 `Enable_VoiceTeaching` / `Enable_GroupTeaching` |
| *实时屏幕广播（非缩略图）* | *🚧 待实现* | 全屏流接收 |
| *USB / 进程监控钩子联动* | *🚧 待实现* | 与 `libTDMaster` 等模块交互 |

---

## 运行环境

- Python 3.8+
- Pillow
- Windows（学生端与教师端模拟器均基于 Windows 网络环境）
- 一张能访问外网的网卡（用于自动获取本机 IP）
- 与学生端处于同一二层/广播域（UDP 多播可达）

---

## 安装与运行

```bash
# 安装依赖
pip install Pillow

# 运行
python "teacher_sim.py"
```

程序启动后会监听以下两个多播组：

| 通道 | 多播地址 | 端口 | 用途 |
|------|----------|------|------|
| 主通道 | `224.50.50.42` | `4705` | 控制、缩略图、保活 |
| 会话通道 | `225.2.2.1` | `5512` | 登录宣告、状态广播、聊天/消息 |

当学生端连接后，缩略图会保存到桌面：

- `preview_<ip>_<n>.jpg` —— 原始 JPEG
- `preview_<ip>_<n>_fixed.jpg` —— 经过翻转与色度修正后的图片

---

## 运行示例

```text
2026-07-10 08:33:13 [INFO] Teacher 192.168.2.203 started on 224.50.50.42:4705 + 225.2.2.1:5512
2026-07-10 08:33:17 [INFO] [Login] 192.168.2.139 登录成功
[学生状态] 192.168.2.139: [窗口标题] Program Manager
[学生状态] 192.168.2.139: [窗口标题] 远程消息
[学生状态] 192.168.2.139: [WiFi可用网络数量] -1
[学生消息] 192.168.2.139: 你好
2026-07-10 08:33:48 [INFO] [Preview] 已保存修复图 preview_192_168_2_139_0_fixed.jpg
```

> 状态消息（窗口标题、WiFi 数量等）由学生端定时器周期上报；聊天消息在学生端主动发送时才会出现。

---

## 协议速览

以下是已实现的常见包类型：

| Magic | 名称 | 方向 | 说明 |
|-------|------|------|------|
| `OONC` | Online Notification | 教师 → 多播 | 教师端在线宣告 |
| `CANC` | Control Announcement | 教师 → 多播 | 控制宣告 |
| `ANNO` | Announcement | 教师/学生 → 多播 | 会话通道状态/上线广播 |
| `KACA` | Keep-Alive / Discovery | 学生 → 教师 | 发现/保活请求 |
| `WACA` | Wake/Acknowledge | 教师 → 学生 | 回应 KACA |
| `LOGI` | Login | 学生 → 教师 | 学生端登录宣告 |
| `MESS` | Message | 双向 | subtype=0x800 为学生聊天；subtype=6 为窗口标题；subtype=7 为 WiFi 数量；subtype=1/3 为 IE URL/系统性能 |
| `LPNT` | Login/Preview Notification | 教师 → 学生 | subtype=2/3 握手与就绪通知 |
| `DMOC` | Display/Control Info | 教师 → 学生 | 显示参数与控制信息 |
| `TRMC` | Control Request | 学生 → 教师 | 学生端控制信息请求 |
| `DENT` | Device Registration Done | 学生 → 教师 | 设备注册完成 |
| `TRNT` | Thumbnail Ready | 学生 → 教师 | 学生端准备好发送缩略图 |
| `LANT` / `TNAL` | Thumbnail Fragment | 学生 → 教师 | 缩略图分片 |
| `SRNT` | Server Response | 教师 → 学生 | 回应 DENT |
| `TNRS` | Thumbnail Request | 教师 → 学生 | 主动请求缩略图 |

### MESS 负载子类型

`MESS` 包的负载格式因用途不同而不同：

| 子类型 | payload 格式 | 含义 |
|--------|--------------|------|
| `0x800` | `[0]=总长 [4]=0x800 [16..]=UTF-16-LE 字符串` | 学生端聊天消息 |
| `6` | `[0]=总长 [12]=6 [16]=字符串缓冲长度 [20]=PID [24..]=UTF-16-LE 标题` | 学生端当前活动窗口标题 |
| `7` | `[0]=总长 [12]=7 [16]=4 [20]=数量` | 周围可用 WiFi 网络数量（`-1` 表示检测失败） |
| `1` | `[0]=总长 [12]=1 [16]=4 [20]=额外数据` | IE/浏览器 URL 相关信息 |
| `3` | `[0]=总长 [12]=3 [16]=32` | 系统性能与进程信息 |
| `0` | `[0]=总长 [12]=0` | 清空/重置窗口标题 |

---

## 逆向分析笔记

以下是在 `StudentMain.exe` 中发现的关键函数，对应学生端各类状态/消息的发送逻辑：

| IDA 函数 | 地址 | 功能 |
|----------|------|------|
| `sub_434790` | `0x434790` | 调用 `GetForegroundWindow` / `GetWindowTextW` 获取当前窗口标题，通过 `MESS` subtype=6 上报；若标题为 `"Program Manager"` 则 PID 置 0 |
| `sub_43B080` | `0x43B080` | 通过 `MESS` subtype=7 上报周围可用 WiFi 数量，实际计算在 `sub_434930` 中调用 `Wlanapi.dll` |
| `sub_434930` | `0x434930` | 使用 `WlanOpenHandle` / `WlanEnumInterfaces` / `WlanGetAvailableNetworkList` 枚举可用 WiFi |
| `sub_434600` | `0x434600` | 当前前台进程为 `IEXPLORE.EXE` 时，通过 `MESS` subtype=1 上报 IE/浏览器 URL 信息 |
| `sub_4343A0` | `0x4343A0` | 通过 `MESS` subtype=3 上报系统性能、CPU/内存及进程列表信息 |
| `sub_434AC0` | `0x434AC0` | 另一条活动窗口文本上报路径（使用不同的 payload 类型 `0x1000000`） |
| `sub_433A80` | `0x433A80` | 构造学生端发出的聊天/消息负载（`MESS` subtype=0x800） |

---

## 项目结构

```
.
├── teacher_sim.py   # 主程序
├── README.md        # 项目说明
├── .gitignore       # Git 忽略规则
└── LICENSE          # MIT 许可证
```

---

## 更新日志

### 2026-07-10

- 增加 `MESS` 包接收与解析，支持学生端聊天消息、窗口标题、WiFi 数量、系统状态等。
- 根据 IDA 分析结果完善 MESS subtype 6/7/1/3/0 的识别与展示。
- 优化控制台输出，状态消息与聊天消息分开标识，避免混淆。

### 2026-07-04

- 项目初始发布，实现第三方极域教师端模拟器核心框架。
- 周期性发送 `OONC` / `CANC` / `ANNO` 在线宣告，使学生端发现模拟教师机。
- 处理学生端 `LOGI` 登录宣告，回送 `MESS`、`LPNT`、`DMOC` 等握手包。
- 响应 `KACA` / `WACA`、`TRMC`、`DENT` 等保活与控制请求。
- 接收 `LANT` / `TNAL` 缩略图分片并重组为完整 JPEG。
- 针对极域学生端截图的 bottom-up DIB 与 YCbCr 色度反相特性，生成正向修复图。
- 基于真实抓包与 IDA 分析还原协议魔数、GUID 与包结构。

---

## 免责声明

1. 本项目是**独立第三方实现**，与极域电子教室官方软件无任何关联。
2. 项目源码仅用于**协议分析、安全研究与教学演示**。研究者应仅在**自己拥有合法管理权限**的网络与设备上使用。
3. 未经他人/学校/单位明确授权，不得将本工具用于获取、监控或控制任何非本人所有的计算机设备。
4. 使用本工具所产生的一切法律后果由使用者自行承担，作者不承担任何连带责任。

---

## 许可证

MIT License —— 详见 [LICENSE](./LICENSE) 文件。

---

## 交流与贡献

欢迎提交 Issue 与 Pull Request。如果你发现了新的协议字段或更稳定的握手顺序，欢迎一起分享，共同完善这份协议笔记。
