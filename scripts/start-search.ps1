param([int]$Port = 8090, [switch]$WithoutCache)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
$pythonPath = if ($pythonCommand) { $pythonCommand.Source } else { $null }
if (-not $pythonPath) {
    $runtimePython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (Test-Path -LiteralPath $runtimePython) { $pythonPath = $runtimePython }
}
if (-not $pythonPath) { throw 'Python 3.10 이상을 설치하고 python 명령을 PATH에 추가하세요.' }
if ($Port -lt 1 -or $Port -gt 65535) { throw '포트는 1~65535 사이여야 합니다.' }
Push-Location -LiteralPath $projectRoot
try {
    $cacheMode = if ($WithoutCache) { 'off' } else { 'on' }
    Write-Host "Open http://127.0.0.1:$Port (Ctrl+C to stop)"
    # C 서버는 기존 터미널에서 실행한다. 자동 실행하지 않아 수명/백신 알림을 숨기지 않는다.
    & $pythonPath -X utf8 -u examples/search_app.py --port $Port --cache $cacheMode
    if ($LASTEXITCODE -ne 0) { throw "검색 웹 종료 코드: $LASTEXITCODE" }
} finally {
    Pop-Location
}
