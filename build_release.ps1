param(
    [Parameter(Mandatory)] [string]$Version,
    [Parameter(Mandatory)] [string]$MinVersion,
    [Parameter(Mandatory)] [AllowEmptyString()] [string]$Changelog,
    [string]$CMake = 'cmake',
    [string]$Remote = 'root@9.247.229.203',
    [ValidateRange(1, 65535)] [int]$Port = 22,
    [string]$DependencyDirectory = (Join-Path $PSScriptRoot 'build\vs2022-x64\Debug'),
    [string]$ReleaseDirectory = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'release')
)

$ErrorActionPreference = 'Stop'
foreach ($value in $Version, $MinVersion) {
    if ($value -notmatch '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$' -or
        @($value.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count -gt 0) {
        throw 'Versions must use numeric X.Y.Z components between 0 and 65535.'
    }
}
if ([version]$MinVersion -gt [version]$Version) { throw 'Minimum exceeds release version.' }
$bytes = [Text.Encoding]::UTF8.GetBytes($Changelog)
if ($bytes.Length -gt 4096 -or $Changelog.Contains([char]0)) { throw 'Invalid changelog.' }
if ($CMake -eq 'cmake' -and -not (Get-Command $CMake -ErrorAction SilentlyContinue)) {
    $candidate = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $candidate) { $CMake = $candidate }
}
$build = Join-Path $PSScriptRoot "build\scarlex-release-$Version"
& $CMake -S $PSScriptRoot -B $build -G 'Visual Studio 18 2026' -A x64 "-DSCARLEX_VERSION=$Version" '-DSCARLEX_PUBLIC_RELEASE=ON' '-DSCARLEX_DIAGNOSTICS=OFF'
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $CMake --build $build --config Release --target rob_run_tool scarlex_loader
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
$payload = Join-Path $build 'Release\rob_run_tool.exe'
$releaseFile = Get-Item -LiteralPath $payload
$info = $releaseFile.VersionInfo
if ("$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart)" -ne $Version) {
    throw 'Built EXE version differs from release.'
}
$runtime = Join-Path $build 'runtime.pak'
& node (Join-Path $PSScriptRoot 'deploy\scarlex-vps\package-runtime.mjs') (Join-Path $PSScriptRoot 'icons') $DependencyDirectory $runtime
if ($LASTEXITCODE -ne 0) { throw 'Runtime packaging failed.' }
$uiFiles = @(
    @{ Source = 'ui\index.html'; Name = 'index.html' },
    @{ Source = 'ui\style.css'; Name = 'style.css' },
    @{ Source = 'ui\app.js'; Name = 'app.js' },
    @{ Source = 'ui\rob-run.jpg'; Name = 'rob-run.jpg' },
    @{ Source = 'ui\LUCIDE-LICENSE.txt'; Name = 'LUCIDE-LICENSE.txt' },
    @{ Source = 'assets\menu_logo.png'; Name = 'logo.png' },
    @{ Source = 'assets\menu_inter.ttf'; Name = 'inter.ttf' },
    @{ Source = 'assets\Inter-OFL.txt'; Name = 'Inter-OFL.txt' }
)

$changes64 = [Convert]::ToBase64String($bytes)
$stage = & ssh -o StrictHostKeyChecking=yes -p $Port $Remote 'mktemp -d /tmp/scarlex-publish.XXXXXXXX'
if ($LASTEXITCODE -ne 0 -or $stage -notmatch '^/tmp/scarlex-publish\.[A-Za-z0-9]+$') { throw 'Could not prepare VPS staging.' }
try {
    & scp -o StrictHostKeyChecking=yes -P $Port $payload "${Remote}:$stage/rob_run_tool.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Payload upload failed.' }
    & scp -o StrictHostKeyChecking=yes -P $Port $runtime "${Remote}:$stage/runtime.pak"
    if ($LASTEXITCODE -ne 0) { throw 'Runtime upload failed.' }
    & ssh -o StrictHostKeyChecking=yes -p $Port $Remote "mkdir -- '$stage/ui'"
    if ($LASTEXITCODE -ne 0) { throw 'Could not create UI staging.' }
    foreach ($asset in $uiFiles) {
        & scp -o StrictHostKeyChecking=yes -P $Port (Join-Path $PSScriptRoot $asset.Source) "${Remote}:$stage/ui/$($asset.Name)"
        if ($LASTEXITCODE -ne 0) { throw 'UI upload failed.' }
    }
    $publish = @'
set -eu
app=/opt/scarlex-server
releases=/var/lib/scarlex-server/releases
test -f "$app/publish.js"
temp=$(mktemp "$releases/.upload.XXXXXXXX")
runtime=$(mktemp "$releases/.runtime.XXXXXXXX")
trap 'rm -f -- "$temp" "$runtime"' EXIT
install -m 0600 -o scarlex -g scarlex '__STAGE__/rob_run_tool.exe' "$temp"
install -m 0600 -o scarlex -g scarlex '__STAGE__/runtime.pak' "$runtime"
changelog=$(printf '%s' '__CHANGELOG__' | base64 -d; printf .)
cd "$app"
runuser -u scarlex -- env SCARLEX_RELEASE_DIR="$releases" \
  SCARLEX_SIGNING_PRIVATE_KEY="$app/private.pem" \
  /opt/scarlex-node20/bin/node "$app/publish.js" "$temp" '__VERSION__' --min '__MIN__' --changelog "${changelog%.}" --runtime "$runtime"
install -d -m 0750 -o scarlex -g scarlex "$releases/launcher"
for name in index.html style.css app.js rob-run.jpg LUCIDE-LICENSE.txt logo.png inter.ttf Inter-OFL.txt; do
    install -m 0640 -o root -g scarlex "__STAGE__/ui/$name" "$releases/launcher/$name"
done
'@
    $publish = $publish.Replace('__STAGE__', $stage).Replace('__VERSION__', $Version).
        Replace('__MIN__', $MinVersion).Replace('__CHANGELOG__', $changes64)
    $response = ($publish -replace '`r', '') | & ssh -o StrictHostKeyChecking=yes -p $Port $Remote 'tr -d "\r" | bash -s'
    if ($LASTEXITCODE -ne 0) { throw 'Server publication failed.' }
    $manifest = $response | Out-String | ConvertFrom-Json
    "URL: $($manifest.payload_url)"
    "SHA-256: $($manifest.payload_sha256)"
    New-Item -ItemType Directory -Path $ReleaseDirectory -Force | Out-Null
    if (Get-ChildItem -LiteralPath $ReleaseDirectory -Force | Where-Object Name -ne 'scarlex_loader.exe') {
        throw 'Release folder contains other files; choose an empty release folder.'
    }
    Copy-Item -LiteralPath (Join-Path $build 'Release\scarlex_loader.exe') -Destination (Join-Path $ReleaseDirectory 'scarlex_loader.exe') -Force
    "Launcher: $(Join-Path $ReleaseDirectory 'scarlex_loader.exe')"
} finally {
    $cleanup = "rm -f -- '$stage/rob_run_tool.exe' '$stage/runtime.pak'; "
    foreach ($asset in $uiFiles) { $cleanup += "rm -f -- '$stage/ui/$($asset.Name)'; " }
    $cleanup += "rmdir -- '$stage/ui' 2>/dev/null || true; rmdir -- '$stage'"
    & ssh -o StrictHostKeyChecking=yes -p $Port $Remote $cleanup
}
