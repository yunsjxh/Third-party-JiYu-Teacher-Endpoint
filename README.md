# 第三方极域教师端 / Third-party JiYu Teacher Endpoint

[![GitHub stars](https://img.shields.io/github/stars/yunsjxh/Third-party-JiYu-Teacher-Endpoint?style=social)](https://github.com/yunsjxh/Third-party-JiYu-Teacher-Endpoint/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/yunsjxh/Third-party-JiYu-Teacher-Endpoint?style=social)](https://github.com/yunsjxh/Third-party-JiYu-Teacher-Endpoint/network/members)

一个用 Python 实现的**第三方极域课堂管理系统 V6.0 教师端模拟器**。本项目基于对官方教师端可执行文件的 **IDA Pro 静态逆向分析**以及真实网络抓包，还原出教师端与学生端之间的 UDP 通信协议，从而让学生端将本机识别为合法教师机，并接收登录、保活、控制请求以及屏幕缩略图等数据。

> 本项目仅用于协议学习、网络安全研究与教学演示。**请在合法授权范围内使用，严禁用于未经许可的监控、入侵或干扰他人设备。**

---

## 项目背景

本项目的协议字段、包结构、握手时序以及教师端 GUID 等信息，主要来源于：

1. **IDA Pro 静态逆向分析**官方教师端可执行文件，提取协议魔数（Magic）、版本号、字段偏移与关键常量；
2. 在隔离实验环境中抓取教师端与学生端之间的真实 UDP 多播/单播报文，进行逐字节比对与验证；
3. 将还原出的二进制协议用 Python 重新实现，使其能在不依赖官方客户端的情况下模拟教师端行为。

因此，本仓库既是一个可运行的教师端模拟器，也是一份关于极域 V6.0 教师-学生通信协议的公开技术笔记。

---

## 功能特性

### 教师端核心功能
- **教师端在线宣告**：周期性发送 `OONC` / `CANC` / `ANNO` 广播，使学生端发现本机。
- **学生端上线处理**：响应 `LOGI` 登录宣告，回送 `MESS`、`LPNT`、`DMOC` 等握手包。
- **保活与发现**：处理 `KACA` 保活请求并回送 `WACA`；响应 `TRMC`、`DENT` 等控制/注册包。
- **聊天消息**：通过 MESS 协议向指定学生发送 UTF-16LE 编码的聊天消息。
- **缩略图接收与重组**：接收学生端分片发送的 `LANT`/`TNAL` 预览图数据，拼接成完整 JPEG。
- **图像修正**：针对极域学生端截图的 bottom-up DIB 与 YCbCr 色度反相特性，生成正向、颜色正常的修复图。

### 黑屏/锁屏控制 🔒
- **黑屏安静**：通过 MESS 协议（type=0x20）发送黑屏命令，进入学生端 `sub_44CA70` 路径，正确设置 bit 0x20 状态标志。
- **键鼠锁定**：同步 COMD（case 6）路径，锁定学生端键盘和鼠标。
- **可选的键鼠锁定**：黑屏时可选择只黑屏不锁键鼠（`lock=0`）。
- **自动解锁**：教师端（模拟器）定时器到期后自动发送解锁包（MESS flags=0x90000000），无需手动操作。
- **自定义文字**：支持黑屏时显示自定义提示文字，并可通过 COLORREF 指定文字颜色（默认黄色 `0x0000FFFF`，匹配真实教师端）。

### 远程开关机 ⚡
- **关机/重启**：通过 COMD 应用命令（cmdId `0x14`=关机、`0x13`=重启）调用学生端 `Shutdown.exe`（`-f|-nf -b|-nb`）。
- **两种模式**：强制立即（cmdId 或上 `0x10000000`，无提示）或倒计时模式（学生端弹出倒计时气泡，可附带自定义提示文字）。

### 学生信息建档 🗂️
- **信息请求**：登录后自动发送（也可 `info <ip>` 手动触发）MESS 信息请求（type `0x100000`）。
- **系统信息**：计算机名称、登录用户、MAC 地址、操作系统名称/版本、CPU 厂商/型号、内存大小（IP 取 UDP 源地址）。
- **进程列表**：type 6 分片重组，`ps <ip>` 查看。
- **窗口列表**：type 7 分片重组，`wins <ip>` 查看。

### 逐字节还原
- 包结构参照真实教师端抓包（Line 223/283 等），保留原始 GUID、Magic、版本号与尾部字段。
- MESS 黑屏/解锁包精确匹配教师端 `sub_54C4E0` / `sub_54C5C0` 构造逻辑。

---

## 关键技术发现

### 黑屏命令的双路径之谜

逆向分析发现，学生端存在**两条独立的命令处理路径**：

| 路径 | 协议 | 入口 | 黑屏窗口 | 设置 bit 0x20 | 锁键鼠 |
|------|------|------|----------|--------------|--------|
| COMD (case 6) | `0x434F4D44` | `sub_44BD50 → sub_44A490` | ✅ | ❌ | ✅ |
| **MESS (type=0x20)** | `0x5353454D` | MESS handler (`sub_44CA70`) | ✅ | ✅ | ❌ |

- **bit 0x20** 是学生端黑屏激活状态的关键标志（位于 `CStudentDlg+0x23C8`）
- 只有 MESS 路径会设置此标志；没有它，COMD 路径的解锁命令 (`case 0x0E`) 无法正确触发 `sub_436040`（真正的解锁函数），而是误杀前台进程
- **真实教师端同时使用两条路径**：MESS 负责黑屏窗口和状态、COMD 负责实际键鼠锁定

### MESS 黑屏包结构（39 字节基础）

```
Offset  Size  Field         Value
[0..3]  4     total_len     0x27=39 (+ optional UTF-16LE text)
[4..7]  4     type          0x20 (黑屏)
[8..11] 4     flags         0x80000000 (启动) / 0x90000000 (取消)
[12..15] 4    lock_input    1=锁定, 0=不锁
[16..19] 4    field_1       0x01
[20..23] 4    timeout       超时秒数 (0=永久)
[24..27] 4    has_text      0/1
[28..31] 4    text_color    Windows COLORREF (0x00BBGGRR)
[32..35] 4    field_5       0x00000000
[36..38] 3    padding       0xA00520 (has_text=0 时)
[36..]  var   text          UTF-16LE 文本 (has_text=1 时)
```

### COMD/LCMD 应用命令包结构

两种魔数共用同一套"应用命令"负载格式（`COMD` 带事务应答，`LCMD` 为轻量版）：

```
公共头 (0x1C 字节):
  [0..3]   magic       0x434F4D44='COMD' / 0x4C434D44='LCMD'
  [4..7]   version     0x00010000
  [8..11]  bodyLen     0x0D + payloadLen
  [12..27] GUID        事务 GUID(接收端不校验)
COMD body:
  [0..3]   0x4E20      事务超时 20000ms
  [4..7]   事务id      学生端用它回 TRMC 应答
  [8..11]  payloadLen
  [12..]   payload
应用命令 payload:
  +0x00    msgId       分发器不读(任意值)
  +0x04    category    0x200=应用命令(0x200000=VR 等)
  +0x08    flags       高字节须为 0
  +0x0C    body        [cmdId][参数...]
```

> **实现细节坑**：学生端按包头 len 从 body+0xC 复制负载，会吃掉 payload 尾部 4 字节——构造时末尾需多补 4 字节（本项目的 `send_shutdown`/黑屏 case 6 均已处理）。

**cmdId 速查**（学生端 `sub_44A490` 分发）：

| cmdId | 动作 | cmdId | 动作 |
|-------|------|-------|------|
| `0x13` | 重启 | `0x0F` | 远程运行程序 |
| `0x14` | 关机 | `0x12` | 终止远程进程 |
| `0x02` | 关闭所有程序 | `0x18` | 打开网址 |
| `0x15` | 退出学生端 | `0x06` | 黑屏/锁键鼠 |
| `0x05` | 修改显示名 | `0x0A/0x0B` | 锁屏开关/解锁密码 |

cmdId 或上 `0x10000000` = 强制执行（跳过学生端倒计时提示）。关机/重启体：`[cmdId][延迟秒数][8B 保留][UTF-16LE 提示文字]`，执行端为学生端目录下 `Shutdown.exe`（`-b`=重启、`-nb`=关机、`-f`=强制）。

### 学生信息上报协议

学生端**不会主动上报**，需教师端通过会话通道（5512）发送 MESS 信息请求：

```
请求 payload:  [total_len=16][0x100000][flags=0][reportType]
reportType:    0=全部(type 5+6+7)  1=进程列表  2=窗口列表  3/4=关窗口/杀进程

回复 payload:  [total_len][0][0x800000][type][数据...]
type:          5=系统信息  6=进程列表(分片)  7=窗口列表(分片)
```

> **解析坑**：MESS 状态消息（窗口标题/WiFi 等）的 subtype 与信息上报的 type 同位置，必须先判断 category（`payload[8:12]`）：`0x800000`=信息上报、`0x03`=周期状态，再解释 subtype，否则进程 PID 会被误读成"WiFi 数量"。

**type 5 系统信息结构**（自 `payload+0x0C` 起，UTF-16LE 定长字段）：

```
+0x00  u32 type=5      +0x04  计算机名称[32]   +0x44  学生ID
+0x48  MAC 地址[6]     +0x4E  登录用户[32]     +0x8E  操作系统名称[32]
+0xCE  操作系统版本    +0x20E CPU 厂商[32]    +0x24E CPU 型号[64]
+0x2CE 内存大小 "xxxx MB"
```

**type 6/7 分片**：`[type][flag(1=首片)][{u32 id, wchar name\0}...]`，每条目 6+2×len 字节（type 6 id=PID，type 7 id=HWND）；无结束标记，收到首片时重置累积。

---

## 运行环境

- Python 3.8+
- Pillow
- 一张能访问外网的网卡（用于自动获取本机 IP）
- 与学生端处于同一二层/广播域（UDP 多播可达）

---

## 安装与运行

```bash
# 安装依赖
pip install Pillow

# 运行
python "teacher_sim .py"
```

程序启动后会打开两个窗口：
- **主窗口**（命令行）：`teacher>` 交互提示符
- **日志窗口**（PowerShell）：实时显示详细日志

监听以下两个多播组：

| 通道 | 多播地址 | 端口 | 用途 |
|------|----------|------|------|
| 主通道 | `224.50.50.42` | `4705` | 控制、缩略图、COMD 命令 |
| 会话通道 | `225.2.2.1` | `5512` | 登录宣告、MESS 命令、状态广播 |

当学生端连接后，缩略图会保存到桌面：

- `preview_<ip>_<n>.jpg` —— 原始 JPEG
- `preview_<ip>_<n>_fixed.jpg` —— 经过翻转与色度修正后的图片

---

## 命令参考

| 命令 | 说明 |
|------|------|
| `help` / `?` | 显示帮助 |
| `list` / `ls` | 列出已登录学生（含已建档的计算机名/用户/MAC/OS/CPU/内存） |
| `preview <ip>` | 请求指定学生的屏幕预览 |
| `all` | 请求所有学生的屏幕预览 |
| `msg <ip> <text>` | 向指定学生发送聊天消息 |
| `info <ip> [0\|1\|2]` | 请求学生上报信息（0=全部 1=进程列表 2=窗口列表，登录后自动请求一次） |
| `ps <ip>` | 显示学生进程列表（需先 info 请求） |
| `wins <ip>` | 显示学生窗口列表（需先 info 请求） |
| `blackscreen` / `bs <ip> [lock] [text]` | 黑屏安静（默认锁键鼠，10 秒自动解锁） |
| `bsperm` / `bsp <ip> [lock] [text]` | 永久黑屏（需手动 unlock） |
| `unlock <ip>` | 解锁指定学生的黑屏/键鼠锁 |
| `bsall [lock] [text]` | 对所有已登录学生发送黑屏 |
| `unlock_all` | 对所有学生解锁 |
| `shutdown` / `sd <ip> [秒] [text]` | 关闭学生机（不带秒数=立即强制；带秒数=倒计时提示） |
| `reboot` / `rb <ip> [秒] [text]` | 重启学生机（参数同 shutdown） |
| `debug on` / `off` | 切换日志级别 |
| `exit` / `quit` / `q` | 退出程序 |

### 使用示例

```
teacher> bs 192.168.2.139               # 黑屏 + 锁键鼠，10 秒自动解
teacher> bs 192.168.2.139 0             # 只黑屏，不锁键鼠
teacher> bs 192.168.2.139 1 请认真听课   # 黑屏锁键鼠 + 自定义黄色文字
teacher> bsp 192.168.2.139 1            # 永久锁屏（不会自动解开）
teacher> unlock 192.168.2.139           # 手动解锁
teacher> bsall 1 安静                   # 全员黑屏锁键鼠 + "安静"
teacher> msg 192.168.2.139 你好         # 发聊天消息
teacher> preview 192.168.2.139          # 请求屏幕缩略图
teacher> info 192.168.2.139             # 请求学生信息（系统信息+进程+窗口）
teacher> ps 192.168.2.139               # 查看学生进程列表
teacher> shutdown 192.168.2.139         # 立即强制关机
teacher> reboot 192.168.2.139 30 请保存作业  # 倒计时 30 秒重启并提示
```

---

## 协议速览

以下是已实现的常见包类型：

| Magic | 名称 | 方向 | 说明 |
|-------|------|------|------|
| `OONC` | Online Notification | 教师 → 多播 | 教师端在线宣告 |
| `CANC` | Control Announcement | 教师 → 多播 | 控制宣告 |
| `ANNO` | Announcement | 教师 → 多播 | 会话通道状态广播 |
| `KACA` | Keep-Alive / Discovery | 学生 → 教师 | 发现/保活请求 |
| `WACA` | Wake/Acknowledge | 教师 → 学生 | 回应 KACA |
| `LOGI` | Login | 学生 → 教师 | 学生端登录宣告 |
| `MESS` | Message | **双向** | 聊天消息 / 黑屏控制 / 状态上报 / 信息上报（0x100000 请求，type 5/6/7 回复） |
| `LPNT` | Login/Preview Notification | 教师 → 学生 | subtype=2/3 握手与就绪通知 |
| `DMOC` | Display/Control Info | 教师 → 学生 | 显示参数与控制信息 |
| `COMD` | Command | 教师 → 学生 | 应用命令（键鼠锁、关机 cmdId=0x14、重启 cmdId=0x13 等，magic=0x434F4D44） |
| `LCMD` | Lite Command | 教师 → 学生 | 轻量应用命令（与 COMD 同构，无事务应答，magic=0x4C434D44） |
| `TRMC` | Control Request | 学生 → 教师 | 学生端控制信息请求 |
| `DENT` | Device Registration Done | 学生 → 教师 | 设备注册完成 |
| `TRNT` | Thumbnail Ready | 学生 → 教师 | 学生端准备好发送缩略图 |
| `LANT` / `TNAL` | Thumbnail Fragment | 学生 → 教师 | 缩略图分片 |
| `SRNT` | Server Response | 教师 → 学生 | 回应 DENT |
| `TNRS` | Thumbnail Request | 教师 → 学生 | 主动请求缩略图 |

---

## 项目结构

```
.
├── teacher_sim .py   # 主程序
├── README.md         # 项目说明
├── index.html        # 项目主页（GitHub Pages）
├── script.js         # 主页动效脚本
├── styles.css        # 主页样式
├── .gitignore        # Git 忽略规则
└── LICENSE           # MIT 许可证
```

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
