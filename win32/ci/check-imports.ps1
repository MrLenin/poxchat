#Requires -Version 5.1
<#
.SYNOPSIS
	Name every DLL the staged tree imports but does not carry.

.DESCRIPTION
	Windows has no ldd, and a staged tree missing one dependency reports it as
	STATUS_DLL_NOT_FOUND (0xC0000135) the moment the process starts, with no
	name attached.  The smoke test catches that a build is broken; it cannot say
	what is missing, so finding the set costs one runner cycle per file.

	This reads the import and delay-import tables out of the PE headers itself
	-- no dumpbin, no VC environment -- and reports everything unresolved at
	once, saying where in -Prefix each missing file can be found.

	Resolution follows the loader for the process directory: an import is
	satisfied if it sits beside poxchat.exe in -Root, is an API set, or is a
	system DLL.  DLLs in subdirectories of the tree (plugins, lib\enchant-2)
	are scanned too -- their imports resolve against the process directory, not
	their own -- so they are checked the same way.
#>

[CmdletBinding()]
param (
	# The staged tree, i.e. the directory poxchat.exe runs from.
	[Parameter(Mandatory = $true)]
	[string] $Root,

	# Directories to search when reporting where a missing DLL could come from.
	[string[]] $Prefix = @()
)

$ErrorActionPreference = 'Stop'

# --- PE reading -------------------------------------------------------------

function Get-PeString ($bytes, [int] $offset)
{
	if ($offset -lt 0 -or $offset -ge $bytes.Length) { return $null }
	$end = $offset
	while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
	return [System.Text.Encoding]::ASCII.GetString($bytes, $offset, $end - $offset)
}

function Get-PeImports ([string] $Path)
{
	$bytes = [System.IO.File]::ReadAllBytes($Path)
	if ($bytes.Length -lt 0x40) { return @() }
	if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return @() }		# MZ

	$pe = [BitConverter]::ToInt32($bytes, 0x3C)
	if ($pe -le 0 -or $pe + 24 -gt $bytes.Length) { return @() }
	if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) { return @() }	# PE\0\0

	$sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
	$optSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
	$opt = $pe + 24
	$magic = [BitConverter]::ToUInt16($bytes, $opt)

	# PE32+ widens ImageBase to 8 bytes and pushes the data directory along
	# with it.
	if ($magic -eq 0x20B) {
		$dirs = $opt + 112
	} elseif ($magic -eq 0x10B) {
		$dirs = $opt + 96
	} else {
		return @()
	}
	$dirCount = [BitConverter]::ToUInt32($bytes, $dirs - 4)

	# The section table follows the optional header and is what turns an RVA
	# into a file offset.
	$sections = @()
	for ($i = 0; $i -lt $sectionCount; $i++) {
		$at = $opt + $optSize + $i * 40
		if ($at + 40 -gt $bytes.Length) { break }
		$virtual = [BitConverter]::ToUInt32($bytes, $at + 8)
		$raw = [BitConverter]::ToUInt32($bytes, $at + 16)
		$sections += [pscustomobject]@{
			Rva = [BitConverter]::ToUInt32($bytes, $at + 12)
			Span = [Math]::Max($virtual, $raw)
			File = [BitConverter]::ToUInt32($bytes, $at + 20)
		}
	}

	$toOffset = {
		param([uint32] $rva)
		foreach ($sec in $sections) {
			if ($rva -ge $sec.Rva -and $rva -lt $sec.Rva + $sec.Span) {
				return [int]($sec.File + ($rva - $sec.Rva))
			}
		}
		return -1
	}

	$names = @()

	# Import directory: 20-byte descriptors, DLL name RVA at +12, terminated by
	# an all-zero descriptor.
	if ($dirCount -gt 1) {
		$rva = [BitConverter]::ToUInt32($bytes, $dirs + 8)
		if ($rva -ne 0) {
			$at = & $toOffset $rva
			while ($at -ge 0 -and $at + 20 -le $bytes.Length) {
				$nameRva = [BitConverter]::ToUInt32($bytes, $at + 12)
				if ($nameRva -eq 0) { break }
				$name = Get-PeString $bytes (& $toOffset $nameRva)
				if ($name) { $names += $name }
				$at += 20
			}
		}
	}

	# Delay-import directory: 32-byte descriptors, name at +4.  Attribute bit 0
	# says the fields are RVAs; the original 1990s format stored virtual
	# addresses instead, and nothing that can build this tree emits it, so that
	# form is left alone rather than guessed at.
	if ($dirCount -gt 13) {
		$rva = [BitConverter]::ToUInt32($bytes, $dirs + 13 * 8)
		if ($rva -ne 0) {
			$at = & $toOffset $rva
			while ($at -ge 0 -and $at + 32 -le $bytes.Length) {
				$attrs = [BitConverter]::ToUInt32($bytes, $at)
				$nameRva = [BitConverter]::ToUInt32($bytes, $at + 4)
				if ($nameRva -eq 0) { break }
				if (-not ($attrs -band 1)) {
					Write-Warning "$Path uses address-based delay imports; not walking them"
					break
				}
				$name = Get-PeString $bytes (& $toOffset $nameRva)
				if ($name) { $names += $name }
				$at += 32
			}
		}
	}

	return $names
}

# --- what counts as resolved ------------------------------------------------

$Root = (Resolve-Path -LiteralPath $Root).Path

$beside = @{}
foreach ($file in Get-ChildItem -LiteralPath $Root -File) {
	$beside[$file.Name.ToLowerInvariant()] = $true
}

# System32 covers the Windows SDK import libraries the tree links against;
# api-ms-win-* / ext-ms-win-* are handled separately below, since the loader
# resolves those from a schema rather than from files on disk.
$system = @{}
foreach ($dir in @("$env:SystemRoot\System32", "$env:SystemRoot\SysWOW64")) {
	if (-not (Test-Path -LiteralPath $dir)) { continue }
	foreach ($file in Get-ChildItem -LiteralPath $dir -Filter *.dll -File -ErrorAction SilentlyContinue) {
		$system[$file.Name.ToLowerInvariant()] = $true
	}
}

# Where a missing DLL could be picked up from, for the report.
$available = @{}
foreach ($dir in $Prefix) {
	if (-not (Test-Path -LiteralPath $dir)) { continue }
	foreach ($file in Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue) {
		$key = $file.Name.ToLowerInvariant()
		if (-not $available.ContainsKey($key)) { $available[$key] = $file.FullName }
	}
}

# --- walk -------------------------------------------------------------------

$binaries = Get-ChildItem -LiteralPath $Root -File -Recurse |
	Where-Object { $_.Extension -in '.exe', '.dll', '.pyd' }

$missing = @{}
foreach ($binary in $binaries) {
	foreach ($import in Get-PeImports $binary.FullName) {
		$key = $import.ToLowerInvariant()
		if ($beside.ContainsKey($key)) { continue }
		if ($system.ContainsKey($key)) { continue }
		if ($key -match '^(api|ext)-ms-win-') { continue }
		if (-not $missing.ContainsKey($key)) { $missing[$key] = @() }
		$missing[$key] += $binary.FullName.Substring($Root.Length).TrimStart('\')
	}
}

Write-Host "scanned $($binaries.Count) binaries under $Root"

if ($missing.Count -eq 0) {
	Write-Host 'every import resolves inside the staged tree'
	exit 0
}

Write-Host ''
Write-Host "$($missing.Count) unresolved:"
foreach ($key in ($missing.Keys | Sort-Object)) {
	$from = if ($available.ContainsKey($key)) { $available[$key] } else { 'NOT IN THE DEPENDENCY PREFIX EITHER' }
	Write-Host "  $key"
	Write-Host "      wanted by: $(($missing[$key] | Sort-Object -Unique) -join ', ')"
	Write-Host "      found at:  $from"
}

throw "$($missing.Count) DLL(s) missing from the staged tree"
