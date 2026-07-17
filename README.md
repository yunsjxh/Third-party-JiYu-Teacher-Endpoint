# 第三方课堂管理教师端 / Third-party JiYu Teacher Endpoint

[![GitHub stars](https://img.shields.io/github/stars/yunsjxh/Third-party-JiYu-Teacher-Endpoint?style=social)](https://github.com/yunsjxh/Third-party-JiYu-Teacher-Endpoint/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/yunsjxh/Third-party-JiYu-Teacher-Endpoint?style=social)](https://github.com/yunsjxh/Third-party-JiYu-Teacher-Endpoint/network/members)

一个使用 Python 编写的第三方课堂管理教师端工具，可在经过授权的局域网环境中与特定版本的课堂管理学生端配合使用。

项目以实用性和易操作性为目标，提供设备发现、课堂提示、屏幕预览、信息查看和常用管理操作。底层兼容方式及通信实现细节不在本文档中公开。

> 本项目仅用于兼容性研究、教学演示以及合法授权的设备管理。请勿将其用于未经许可的监控、控制或干扰。

---

## 功能概览

- 自动发现并维护局域网内的学生端连接
- 查看已连接设备及基本系统信息
- 请求并保存学生端屏幕预览图
- 向指定学生端发送课堂提示消息
- 查看学生端的进程和窗口列表
- 在授权设备上结束指定进程或应用
- 在授权设备上打开网页、文件或运行程序
- 执行黑屏提示、键鼠锁定和解锁
- 执行关机或重启操作
- 在程序运行期间输出状态和诊断日志

部分功能是否可用取决于学生端版本、网络环境和系统权限。

---

## 运行环境

- Windows
- Python 3.8 或更高版本
- Pillow
- 教师端与学生端位于可互相通信的局域网中
- 当前账号拥有相应设备的合法管理权限

---

## 安装与运行

安装依赖：

```bash
pip install Pillow
```

启动程序：

```bash
python "teacher_sim .py"
```

程序启动后会显示交互命令行和运行日志。输入 `help` 或 `?` 可以查看当前版本支持的命令及参数。

屏幕预览图会保存到桌面，其中带有 `_fixed` 后缀的文件为经过方向和颜色修正的版本。

---

## 常用命令

| 命令 | 说明 |
|------|------|
| `help` / `?` | 显示帮助 |
| `list` / `ls` | 列出已连接设备 |
| `preview <ip>` | 请求指定设备的屏幕预览 |
| `all` | 请求所有设备的屏幕预览 |
| `msg <ip> <text>` | 向指定设备发送消息 |
| `info <ip>` | 刷新指定设备的信息 |
| `ps <ip>` | 查看进程列表 |
| `wins <ip>` | 查看窗口列表 |
| `kill <ip> <pid或进程名>` | 结束指定进程 |
| `closeapp <ip> <窗口句柄或标题>` | 关闭指定应用窗口 |
| `openurl <ip> <网址或路径>` | 打开网页或文件 |
| `run <ip> <路径> [参数]` | 运行程序 |
| `bs <ip> [lock] [text]` | 显示临时黑屏提示 |
| `bsp <ip> [lock] [text]` | 显示持续黑屏提示 |
| `unlock <ip>` | 解除黑屏或输入锁定 |
| `bsall [lock] [text]` | 向所有已连接设备显示黑屏提示 |
| `unlock_all` | 解除所有设备的黑屏或输入锁定 |
| `shutdown <ip> [秒] [text]` | 关闭指定设备 |
| `reboot <ip> [秒] [text]` | 重启指定设备 |
| `debug on` / `debug off` | 切换详细日志 |
| `exit` / `quit` / `q` | 退出程序 |

涉及远程控制或电源操作的命令，请先确认目标地址和授权范围。具体参数以程序内置帮助为准。

---

## 兼容性说明

本项目面向特定版本的课堂管理环境，不保证兼容其他版本或不同厂商的软件。系统升级、防火墙设置、网络隔离策略和安全软件都可能影响连接或部分功能。

为避免公开暴露不必要的实现信息，README 不提供通信格式、固定标识、地址端口、内部调用路径或数据结构。相关实现仅保留在项目代码中，供维护者在合法授权范围内审阅。

---

## 项目结构

```text
.
├── teacher_sim .py   # 主程序
├── README.md         # 使用说明
├── index.html        # 项目展示页面
├── script.js         # 页面脚本
├── styles.css        # 页面样式
├── .gitignore        # Git 忽略规则
└── LICENSE           # 开源许可证
```

---

## 安全与合规

1. 本项目是独立的第三方兼容工具，与相关商业软件厂商不存在隶属、授权或合作关系。
2. 只能在本人所有或已获得明确管理授权的网络与设备上使用。
3. 未经许可，不得获取他人设备信息、查看屏幕、运行程序或执行控制操作。
4. 建议在隔离的测试网络中验证功能，并遵守所在地区的法律法规和组织管理制度。
5. 使用者应自行承担因不当使用造成的风险和责任。

---

## 许可证

本项目采用 MIT License，详见 [LICENSE](./LICENSE)。

---

## 交流与贡献

欢迎提交 Issue 或 Pull Request。公开讨论时，请优先描述功能表现、兼容性和复现步骤，避免披露不必要的底层通信细节或可被滥用的信息。
