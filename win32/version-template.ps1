param ([string] $templateFilename, [string] $outputFilename)

# $versionFull keeps the full marketing string from meson.build (e.g.
# "3.0.0~alpha1"), used wherever a human-readable version belongs:
# StringFileInfo's ProductVersion/FileVersion (shown in the Windows file
# properties dialog), PACKAGE_VERSION in config.h (About dialog, user-agent,
# tarball name), etc.
#
# $versionParts is the stripped numeric form (e.g. ["3","0","0"]) used where
# only digits are valid: Win32 VERSIONINFO's top-level FILEVERSION and
# PRODUCTVERSION, which require four numeric components and reject pre-
# release tags like ~alpha1.
$versionFull = Select-String -Path "${env:SOLUTIONDIR}meson.build" -Pattern "  version: '([^']+)',$" | Select-Object -First 1 | %{ $_.Matches[0].Groups[1].Value }
$versionParts = $versionFull.Split('.') | %{ if ($_ -match '^(\d+)') { $Matches[1] } else { '0' } }

[string[]] $contents = Get-Content $templateFilename -Encoding UTF8 | %{
	while ($_ -match '^(.*?)<#=(.*?)#>(.*?)$') {
		$_ = $Matches[1] + $(Invoke-Expression $Matches[2]) + $Matches[3]
	}
	$_
}

[System.IO.File]::WriteAllLines($outputFilename, $contents)
