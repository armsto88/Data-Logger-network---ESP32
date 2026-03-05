$ErrorActionPreference = "Stop"

$cases = @(
    "anemometer_u1_yaw0",
    "anemometer_u5_yaw0",
    "anemometer_u10_yaw0",
    "anemometer_u1_yaw45",
    "anemometer_u5_yaw45",
    "anemometer_u10_yaw45"
)

$root = Split-Path -Parent $MyInvocation.MyCommand.Path

foreach ($case in $cases) {
    $casePath = Join-Path $root $case
    if (-not (Test-Path $casePath)) {
        Write-Host "Skipping missing case: $case"
        continue
    }

    Write-Host "===================================================="
    Write-Host "Running case: $case"
    Write-Host "===================================================="

    Push-Location $casePath
    try {
        blockMesh
        surfaceFeatureExtract
        snappyHexMesh -overwrite
        checkMesh
        simpleFoam
    }
    finally {
        Pop-Location
    }
}

Write-Host "All matrix cases processed."
