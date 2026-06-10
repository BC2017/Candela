# Downloads test assets into assets/ (gitignored); run once after cloning.
# - Khronos Sponza (glTF variant)
# - Poly Haven HDRI for image-based lighting (CC0)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$sponzaDest = Join-Path $root "assets\Sponza\glTF"
if (Test-Path (Join-Path $sponzaDest "Sponza.gltf")) {
    Write-Host "Sponza already present at $sponzaDest"
} else {
    New-Item -ItemType Directory -Force $sponzaDest | Out-Null
    $api = "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/Sponza/glTF"
    Write-Host "Fetching Sponza file list..."
    $files = Invoke-RestMethod $api -Headers @{ "User-Agent" = "candela-engine" }
    $i = 0
    foreach ($file in $files) {
        $i++
        Write-Host ("[{0}/{1}] {2}" -f $i, $files.Count, $file.name)
        Invoke-WebRequest $file.download_url -OutFile (Join-Path $sponzaDest $file.name)
    }
    Write-Host "Done: $sponzaDest"
}

$hdriDest = Join-Path $root "assets\hdri"
$hdriFile = Join-Path $hdriDest "kloofendal_48d_partly_cloudy_puresky_2k.hdr"
if (Test-Path $hdriFile) {
    Write-Host "HDRI already present at $hdriFile"
} else {
    New-Item -ItemType Directory -Force $hdriDest | Out-Null
    Write-Host "Downloading HDRI (Poly Haven, CC0)..."
    Invoke-WebRequest "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_puresky_2k.hdr" -OutFile $hdriFile
    Write-Host "Done: $hdriFile"
}
