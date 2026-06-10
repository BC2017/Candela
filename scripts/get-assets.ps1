# Downloads test assets into assets/ (gitignored); run once after cloning.
# - Khronos glTF sample models (Sponza, FlightHelmet, DamagedHelmet,
#   MetalRoughSpheres)
# - Poly Haven HDRI for image-based lighting (CC0)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Get-KhronosModel {
    param([string]$Name, [string]$MarkerFile)
    $dest = Join-Path $root "assets\$Name\glTF"
    if (Test-Path (Join-Path $dest $MarkerFile)) {
        Write-Host "$Name already present at $dest"
        return
    }
    New-Item -ItemType Directory -Force $dest | Out-Null
    $api = "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/$Name/glTF"
    Write-Host "Fetching $Name file list..."
    $files = Invoke-RestMethod $api -Headers @{ "User-Agent" = "candela-engine" }
    $i = 0
    foreach ($file in $files) {
        $i++
        Write-Host ("[{0}/{1}] {2}" -f $i, $files.Count, $file.name)
        Invoke-WebRequest $file.download_url -OutFile (Join-Path $dest $file.name)
    }
    Write-Host "Done: $dest"
}

Get-KhronosModel -Name "Sponza" -MarkerFile "Sponza.gltf"
Get-KhronosModel -Name "FlightHelmet" -MarkerFile "FlightHelmet.gltf"
Get-KhronosModel -Name "DamagedHelmet" -MarkerFile "DamagedHelmet.gltf"
Get-KhronosModel -Name "MetalRoughSpheres" -MarkerFile "MetalRoughSpheres.gltf"

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
