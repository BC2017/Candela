# Downloads test assets (Khronos Sponza, glTF variant) into assets/.
# Assets are gitignored; run this once after cloning.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "assets\Sponza\glTF"

if (Test-Path (Join-Path $dest "Sponza.gltf")) {
    Write-Host "Sponza already present at $dest"
    exit 0
}

New-Item -ItemType Directory -Force $dest | Out-Null
$api = "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/Sponza/glTF"
Write-Host "Fetching file list..."
$files = Invoke-RestMethod $api -Headers @{ "User-Agent" = "candela-engine" }

$i = 0
foreach ($file in $files) {
    $i++
    Write-Host ("[{0}/{1}] {2}" -f $i, $files.Count, $file.name)
    Invoke-WebRequest $file.download_url -OutFile (Join-Path $dest $file.name)
}
Write-Host "Done: $dest"
