[CmdletBinding()]
param(
    [string]$LlvmDirectory = (Join-Path $env:ProgramFiles 'LLVM/bin'),
    [string]$BuildDirectory,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string[]]$Targets = @('ppc_tests', 'unit_tests', 'rexgpu-xenos'),
    [ValidateRange(1, 128)][int]$Jobs = 12,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$sourceDirectory = Split-Path $PSScriptRoot -Parent
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $sourceDirectory 'out/build/win-amd64-msvc'
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$clang = (Resolve-Path -LiteralPath (Join-Path $LlvmDirectory 'clang.exe')).Path
$clangCpp = (Resolve-Path -LiteralPath (Join-Path $LlvmDirectory 'clang++.exe')).Path
$triple = (& $clangCpp -dumpmachine | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $triple -notmatch '^x86_64-.*windows-msvc') {
    throw "The Windows SDK requires LLVM's x86_64 Windows MSVC target; selected: $triple"
}
$cache = Join-Path $BuildDirectory 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    foreach ($entry in @(@('CMAKE_C_COMPILER', $clang), @('CMAKE_CXX_COMPILER', $clangCpp))) {
        $match = Select-String -LiteralPath $cache -Pattern ("^" + $entry[0] + ":[^=]+=(.*)$")
        if ($match) {
            $cached = [IO.Path]::GetFullPath($match.Matches[0].Groups[1].Value)
            if ($cached -ine $entry[1]) {
                throw "Build directory pins another compiler ($cached). Use a new -BuildDirectory; existing outputs are preserved."
            }
        }
    }
}
# Keep validation binaries beside this build cache, away from game deployments.
$outputDirectory = Join-Path $BuildDirectory 'bin'
Push-Location $sourceDirectory
try {
    & cmake --preset win-amd64 -B $BuildDirectory `
        "-DCMAKE_C_COMPILER:FILEPATH=$clang" "-DCMAKE_CXX_COMPILER:FILEPATH=$clangCpp" `
        "-DREXGLUE_OUTPUT_DIRECTORY:PATH=$outputDirectory" -DREXGLUE_BUILD_TESTS=ON `
        -DREXGLUE_ENABLE_FIDELITYFX=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
    if (-not $ConfigureOnly) {
        & cmake --build $BuildDirectory --config $Configuration --target @Targets -j $Jobs
        if ($LASTEXITCODE -ne 0) { throw "SDK build failed ($LASTEXITCODE)." }
    }
} finally {
    Pop-Location
}
