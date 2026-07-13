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

Or use:

```powershell
.\build-msbuild.ps1 -Configuration Debug -RunTests
```

## Dependencies

Expected local paths:

- KswordFrame3.0: `D:\Projects\KswordFrame3.0\KswordFrame3.0`
- Boost.Asio headers: `cpp-gui\vcpkg_installed\x64-windows\include`

If Boost headers are missing, restore them with the existing vcpkg executable:

```powershell
C:\Users\Administrator\Downloads\vcpkg.exe install --triplet x64-windows --x-manifest-root=. --x-install-root=vcpkg_installed
```

You can override dependency paths when building:

```powershell
msbuild .\JiYuTeacherCppGui.sln /m /p:Configuration=Debug /p:Platform=x64 /p:KswordFrameRoot=D:\Projects\KswordFrame3.0\KswordFrame3.0 /p:JiYuVcpkgInstalled=D:\path\to\vcpkg_installed\x64-windows
```
