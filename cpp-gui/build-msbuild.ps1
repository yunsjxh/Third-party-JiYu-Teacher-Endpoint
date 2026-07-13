param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Solution = Join-Path $Root "JiYuTeacherCppGui.sln"

Push-Location $Root
try {
    msbuild $Solution /m /p:Configuration=$Configuration /p:Platform=x64
    if ($RunTests) {
        $TestExe = Join-Path $Root "out\$Configuration\JiYuProtocolTests.exe"
        & $TestExe
    }
}
finally {
    Pop-Location
}
