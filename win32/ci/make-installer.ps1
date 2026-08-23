#Requires -Version 5.1
<#
.SYNOPSIS
	Generate poxchat.iss from its template and compile it with Inno Setup.

.DESCRIPTION
	win32\installer\installer.vcxproj does this as a pre-build event; doing it
	here instead keeps the quoting sane and lets CI build the installer as a
	separate, skippable step.

	poxchat.iss.tt targets Inno Setup 5 and #includes idp.iss from the Inno
	Download Plugin (which fetches vcredist/perl/python at install time).  The
	hosted runners ship Inno Setup 6 and no idp, so -Provision installs both:
	IS5 from jrsoftware, idp from the mirror hexchat kept on its gvsbuild
	releases, the upstream host having gone away years ago.

	The compiled installer lands next to the staged tree, in <BuildDir>\x64\,
	per the iss file's own OutputDir.

.NOTES
	Inno Setup's /d switch cannot take a quoted value ending in a backslash, so
	-RepoRoot must not contain spaces.
#>

[CmdletBinding()]
param (
	[string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
	[Parameter(Mandatory = $true)]
	[string] $BuildDir,
	[string] $Platform = 'x64',
	[switch] $Provision,
	[string] $InnoSetupUrl = 'https://files.jrsoftware.org/is/5/innosetup-5.6.1-unicode.exe',
	[string] $IdpUrl = 'https://github.com/hexchat/gvsbuild/releases/download/hexchat-2.16.2/idpsetup-1.5.1.exe'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 5\ISCC.exe'

if ($Provision -and -not (Test-Path $iscc)) {
	$tmpBase = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
	$tmp = Join-Path $tmpBase 'inno'
	New-Item -ItemType Directory -Force -Path $tmp | Out-Null

	Write-Host "installing Inno Setup 5 from $InnoSetupUrl"
	$setup = Join-Path $tmp 'innosetup.exe'
	Invoke-WebRequest -Uri $InnoSetupUrl -OutFile $setup -UseBasicParsing
	Start-Process -FilePath $setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait

	Write-Host "installing Inno Download Plugin from $IdpUrl"
	$idp = Join-Path $tmp 'idpsetup.exe'
	Invoke-WebRequest -Uri $IdpUrl -OutFile $idp -UseBasicParsing
	Start-Process -FilePath $idp -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
}

if (-not (Test-Path $iscc)) {
	throw "Inno Setup 5 not found at $iscc (re-run with -Provision to install it)"
}

$binDir = Join-Path $BuildDir "$Platform\bin"
$relDir = Join-Path $BuildDir "$Platform\rel"
if (-not (Test-Path $relDir)) {
	throw "no staged tree at $relDir -- build the 'copy' project first"
}

# version-template.ps1 reads the version out of ${SOLUTIONDIR}meson.build, so
# SOLUTIONDIR is the repo root *with* a trailing separator.
$env:SOLUTIONDIR = $RepoRoot.TrimEnd('\') + '\'
$template = Join-Path $RepoRoot 'win32\installer\poxchat.iss.tt'
$iss = Join-Path $binDir 'poxchat.iss'
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
& (Join-Path $RepoRoot 'win32\version-template.ps1') $template $iss

$projectDir = (Join-Path $RepoRoot 'win32\installer').TrimEnd('\') + '\'
& $iscc /dPROJECTDIR=$projectDir /dAPPARCH=$Platform $iss
if ($LASTEXITCODE -ne 0) {
	throw "ISCC failed with exit code $LASTEXITCODE"
}

Get-ChildItem -Path (Join-Path $BuildDir $Platform) -Filter '*.exe' | ForEach-Object {
	Write-Host "built $($_.FullName)"
}
