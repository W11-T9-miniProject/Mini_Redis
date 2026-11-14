param([switch]$Test)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$previousInclude = $env:INCLUDE
$previousLib = $env:LIB
Push-Location -LiteralPath $projectRoot
try {
    # Developer PowerShell이 아니어도 설치된 MSVC/Windows SDK를 찾아 사용한다.
    # MSBuild가 사용자 프로필의 SDK 경로에 접근하지 못하는 환경도 피할 수 있다.
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = $null
    if (Test-Path -LiteralPath $vsWhere) {
        $vsRoot = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (-not $vsRoot) {
        $vsRoot = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*' -Directory |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'VC\Tools\MSVC') } |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $vsRoot) { throw 'Visual Studio C++ Build Tools가 필요합니다. Linux에서는 make를 사용하세요.' }
    $toolset = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $sdk = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt\stdio.h') } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $sdk) { throw 'Windows 10/11 SDK가 필요합니다.' }
    $env:INCLUDE = @(
        (Join-Path $toolset.FullName 'include'),
        (Join-Path $sdk.FullName 'ucrt'),
        (Join-Path $sdk.FullName 'shared'),
        (Join-Path $sdk.FullName 'um')
    ) -join ';'
    $env:LIB = @(
        (Join-Path $toolset.FullName 'lib\x64'),
        (Join-Path $sdkRoot "Lib\$($sdk.Name)\ucrt\x64"),
        (Join-Path $sdkRoot "Lib\$($sdk.Name)\um\x64")
    ) -join ';'
    $compiler = Join-Path $toolset.FullName 'bin\Hostx64\x64\cl.exe'
    New-Item -ItemType Directory -Path build -Force | Out-Null
    $flags = @('/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/Zi', '/Iinclude', '/D_WIN32_WINNT=0x0600', '/Fobuild\', '/Fdbuild\compiler.pdb')
    $core = @('src/common.c', 'src/hash_table.c', 'src/store.c', 'src/resp.c', 'src/command.c')
    $tests = @('tests/test_main.c', 'tests/test_hash_table.c', 'tests/test_store.c', 'tests/test_resp.c', 'tests/test_command.c')
    & $compiler @flags @core @tests '/Febuild\unit-tests.exe'
    if ($LASTEXITCODE -ne 0) { throw '단위 테스트 빌드 실패' }
    $objects = $core | ForEach-Object { 'build/' + [IO.Path]::GetFileNameWithoutExtension($_) + '.obj' }
    & $compiler @flags src/main.c src/server.c @objects '/Febuild\mini-redis.exe' /link ws2_32.lib
    if ($LASTEXITCODE -ne 0) { throw '서버 빌드 실패' }
    if ($Test) {
        & .\build\unit-tests.exe all
        if ($LASTEXITCODE -ne 0) { throw '단위 테스트 실패' }
    }
    Write-Host 'Build complete: .\build\mini-redis.exe 6379'
} finally {
    $env:INCLUDE = $previousInclude
    $env:LIB = $previousLib
    Pop-Location
}
