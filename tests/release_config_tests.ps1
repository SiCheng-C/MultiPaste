$ErrorActionPreference = 'Stop'

$project = Split-Path -Parent $PSScriptRoot
$ciPath = Join-Path $project '.github\workflows\ci.yml'
$releasePath = Join-Path $project '.github\workflows\release.yml'
$installerPath = Join-Path $project 'installer\MultiPaste.iss'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

Assert-True (Test-Path -LiteralPath $ciPath) 'CI workflow is missing'
Assert-True (Test-Path -LiteralPath $releasePath) 'Release workflow is missing'
Assert-True (Test-Path -LiteralPath $installerPath) 'Inno Setup definition is missing'

$ci = Get-Content -LiteralPath $ciPath -Raw
$release = Get-Content -LiteralPath $releasePath -Raw
$installer = Get-Content -LiteralPath $installerPath -Raw

Assert-True ($ci -match '(?m)^\s*pull_request:') 'CI must run for pull requests'
Assert-True ($ci -match 'runs-on:\s*windows-2022') 'CI must use the Visual Studio 2022 runner'
Assert-True ($release -match 'runs-on:\s*windows-2022') 'Release must use the Visual Studio 2022 runner'
Assert-True ($ci -match 'actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0') 'CI must pin actions/checkout v7'
Assert-True ($release -match 'actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0') 'Release must pin actions/checkout v7'
Assert-True ($ci -match 'ctest') 'CI must run tests'
Assert-True ($release -match "tags:\s*\r?\n\s*-\s*'v\*'") 'Release must trigger on v* tags'
Assert-True ($release -match 'contents:\s*write') 'Release needs explicit contents write permission'
Assert-True ($release -match 'gh release create') 'Release must publish with GitHub CLI'
Assert-True ($release -match 'MultiPaste-\$env:VERSION-win-x64\.zip') 'Portable ZIP naming is missing'
Assert-True ($installer -match 'PrivilegesRequired=lowest') 'Installer must support per-user installation'
Assert-True ($installer -notmatch 'ChineseSimplified\.isl') 'Installer must use a bundled Inno Setup language'
Assert-True ($installer -match 'DefaultDirName=\{localappdata\}\\Programs\\MultiPaste') 'Installer must use a per-user directory'
Assert-True ($installer -match 'CloseApplications=yes') 'Installer must close a running MultiPaste before updating'

Write-Output 'Release configuration tests passed.'
