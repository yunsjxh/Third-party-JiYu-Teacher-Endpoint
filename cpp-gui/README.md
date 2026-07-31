# Third-party JiYu Teacher Endpoint - Native C++ GUI

This folder is a Visual Studio/MSBuild native C++ project. It does not require CMake.

## Open in Visual Studio

Open this solution directly:

```text
D:\Projects\Third-party-JiYu-Teacher-Endpoint\cpp-gui\JiYuTeacherCppGui.sln
```

Projects:

- `JiYuTeacherGui` - KswordFrame3.0/FLTK GUI + Boost.Asio protocol service.
- `JiYuProtocolTests` - protocol packet and preview reassembly self-tests.

## Command-line build

```powershell
cd D:\Projects\Third-party-JiYu-Teacher-Endpoint\cpp-gui
msbuild .\JiYuTeacherCppGui.sln /m /p:Configuration=Debug /p:Platform=x64
.\out\Debug\JiYuProtocolTests.exe
```

Release:

```powershell
msbuild .\JiYuTeacherCppGui.sln /m /p:Configuration=Release /p:Platform=x64
```

Release 产物：

```text
D:\Projects\Third-party-JiYu-Teacher-Endpoint\cpp-gui\out\Release\JiYuTeacherGui.exe
D:\Projects\Third-party-JiYu-Teacher-Endpoint\cpp-gui\out\Release\JiYuProtocolTests.exe
```

## 2026.07.31 main 同步能力

GUI-C++ 已同步远程 `main` 的协议研究功能：

- 频道 1..32 的动态会话组播地址/端口，支持 `NANC`、`CANC`、广播、定向发现和 `WACA` 频道字段。
- 预览策略版本重启（LPNT）和 `SRNT` 缺片确认/重传请求，保留帧序号并处理重复分片。
- 连续屏幕观看：先探测学生端 TCP 端口，再发送 feature 8，使用 TCP/UMSP channel 10 接收 H.264/JPEG 桌面记录。
- H.264 使用 Windows Media Foundation 解码并在 FLTK 窗口显示，自动交换 JiYu 码流中的 U/V 色度平面；JPEG/H.264 可保存到桌面 `teacher_remote_view`。
- 远程控制：通过 `MCMD` 启动学生端输入端点，发送严格 28 字节鼠标/键盘 UDP 输入包，支持 Windows 低级键盘钩子和远端 `SPUC` 光标反馈。
- GUI 设置窗口提供频道、网卡、学生定向 IP、会话端点、观看 TCP、远控端口/模式、失败冷却和 H.264 保存选项。画面和状态文本提供右键复制。

启动服务前可在“设置”窗口修改连接参数。频道对应的默认会话端点为
`225.2.(频道+1).1` 和 `5000 + 频道 * 512`。远控端口填 `0` 时自动使用高位随机端口。

Or use:

```powershell
.\build-msbuild.ps1 -Configuration Debug -RunTests
```

## Dependencies

The GUI project is self-contained for the Ksword/FLTK UI layer:

- Vendored KswordFrame3.0: `cpp-gui\vendor\KswordFrame3.0`
- Vendored FLTK headers/libs: `cpp-gui\vendor\KswordFrame3.0\fltk`
- Boost.Asio headers: `cpp-gui\vcpkg_installed\x64-windows\include`

You no longer need `D:\Projects\KswordFrame3.0` on another machine unless you
explicitly want to override the bundled framework copy.

If Boost headers are missing, restore them with the existing vcpkg executable:

```powershell
C:\Users\Administrator\Downloads\vcpkg.exe install --triplet x64-windows --x-manifest-root=. --x-install-root=vcpkg_installed
```

You can override dependency paths when building:

```powershell
msbuild .\JiYuTeacherCppGui.sln /m /p:Configuration=Debug /p:Platform=x64 /p:KswordFrameRoot=D:\path\to\KswordFrame3.0 /p:JiYuVcpkgInstalled=D:\path\to\vcpkg_installed\x64-windows
```
