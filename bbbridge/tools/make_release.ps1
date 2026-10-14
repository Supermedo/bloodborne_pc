# Builds the release and packs what players download: dist\bbbridge-<version>-windows.zip
# holding a "bbbridge" folder to drop into bbport's folder (next to Bloodborne.exe).
# Never includes an account file, logs or build output.
#   powershell -ExecutionPolicy Bypass -File tools\make_release.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vc)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vc = Join-Path (& $vswhere -latest -products * -property installationPath) 'VC\Auxiliary\Build\vcvars64.bat'
}
cmd /c "`"$vc`" >nul && cd /d `"$root`" && cmake --preset x64-release >nul && cmake --build --preset x64-release --target bbbridge"
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(bbbridge VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$stage = Join-Path $root "dist\stage\bbbridge"
Remove-Item (Join-Path $root 'dist\stage') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$root\build\x64-release\bbbridge.exe", "$root\README.md", "$root\CHANGELOG.md", "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.txt" $stage
$zip = Join-Path $root "dist\bbbridge-$version-windows.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip
Remove-Item (Join-Path $root 'dist\stage') -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path -Leaf $zip)" | Set-Content -Encoding ascii "$zip.sha256"
Write-Output "$zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB)  sha256 $hash"
