#Requires -Version 5.1
<#
.SYNOPSIS
	Build the Windows dependency stack that win32/poxchat.props expects.

.DESCRIPTION
	Everything lands under -BuildRoot (default C:\gtk-build), in the layout the
	solution's UserMacros point at:

	  gtk\<plat>\release  gvsbuild prefix: GTK4, OpenSSL, libxml2, sqlite,
	                      luajit, libcurl, enchant, and gettext (win32\nls
	                      compiles the .po catalogues with its msgfmt.exe)
	  jansson\            static jansson -- DepLibs wants jansson.lib and
	                      copy.vcxproj ships no jansson dll
	  libwebsockets\      static libwebsockets, same reasoning
	  WinSparkle\<plat>\  prebuilt release, for the upd plugin
	  cert\cacert.pem     CA bundle, shipped into the tree as cert.pem

	Nothing outside -BuildRoot is written, so this is safe to run on a developer
	box as well as in CI.  It is not incremental beyond gvsbuild's own caching:
	CI restores the whole tree from actions/cache instead.
#>

[CmdletBinding()]
param (
	[string] $BuildRoot = 'C:\gtk-build',
	[ValidateSet('x64')]
	[string] $Platform = 'x64',
	# Pinned rather than floating: $SeededArchives below names an exact pixman
	# tarball, and that has to match the version this gvsbuild asks for.  Bump
	# them together.
	[string] $GvsbuildVersion = '2026.8.0',
	[string] $JanssonTag = 'v2.15.1',
	[string] $LibWebSocketsTag = 'v4.5.8',
	[string] $WinSparkleVersion = '0.9.4',
	[string] $CMakeGenerator = 'Visual Studio 17 2022'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'	# Invoke-WebRequest crawls with the progress bar on

# gvsbuild project names, not pkg-config names.  --enable-gi plus lgi give us
# the typelibs and girepository dll the lua plugin and the installer expect.
$GvsbuildProjects = @(
	'gtk4',
	'openssl',
	'libxml2',
	'sqlite',
	'luajit',
	'libcurl',
	'enchant',
	'gettext',
	'lgi'
)

# gvsbuild pulls each project's tarball from that project's own upstream, and
# pixman's -- cairographics.org -- redirects to an HTTPS endpoint that is simply
# down: following the redirect times out from the runners and from everywhere
# else we tried.  No amount of retrying fixes an endpoint that is off, so seed
# the archive instead.  Debian's orig tarball for pixman is the upstream file
# byte for byte -- verified against the hash gvsbuild itself records -- and we
# check the digest here so a substituted file fails loudly rather than quietly
# building something nobody vetted.
$SeededArchives = @(
	@{
		Name = 'pixman-0.46.4.tar.gz'
		Url = 'http://deb.debian.org/debian/pool/main/p/pixman/pixman_0.46.4.orig.tar.gz'
		Sha256 = 'd09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c'
	}
)

$prefix = Join-Path $BuildRoot "gtk\$Platform\release"
$srcRoot = Join-Path $BuildRoot 'ci-src'

function Write-Step ([string] $Name) {
	Write-Host ''
	Write-Host "=== $Name ===" -ForegroundColor Cyan
}

function Invoke-Checked ([string] $What, [scriptblock] $Body) {
	& $Body
	if ($LASTEXITCODE -ne 0) {
		throw "$What failed with exit code $LASTEXITCODE"
	}
}

function Get-SourceTree ([string] $Url, [string] $Name) {
	New-Item -ItemType Directory -Force -Path $srcRoot | Out-Null
	$zip = Join-Path $srcRoot "$Name.zip"
	$out = Join-Path $srcRoot $Name

	if (-not (Test-Path $zip)) {
		Write-Host "downloading $Url"
		Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
	}
	if (-not (Test-Path $out)) {
		Expand-Archive -Path $zip -DestinationPath $out -Force
	}

	# GitHub source zips nest everything one directory deep.
	$children = @(Get-ChildItem -Path $out -Directory)
	if ($children.Count -eq 1) { return $children[0].FullName }
	return $out
}

Write-Step 'gvsbuild: GTK4 and friends'
$package = if ($GvsbuildVersion) { "gvsbuild==$GvsbuildVersion" } else { 'gvsbuild' }
Invoke-Checked "pip install $package" { python -m pip install --upgrade --disable-pip-version-check $package }
# --configuration release matters: gvsbuild defaults to debug-optimized, which
# would land the prefix in ...\gtk\x64\debug-optimized instead of ...\release.
$gvsSrc = Join-Path $BuildRoot 'src'
New-Item -ItemType Directory -Force -Path $gvsSrc | Out-Null
foreach ($archive in $SeededArchives) {
	$dest = Join-Path $gvsSrc $archive.Name
	if (Test-Path $dest) {
		Write-Host "$($archive.Name) already seeded"
		continue
	}
	Write-Host "seeding $($archive.Name) from $($archive.Url)"
	Invoke-WebRequest -Uri $archive.Url -OutFile $dest -UseBasicParsing
	$digest = (Get-FileHash -Path $dest -Algorithm SHA256).Hash
	if ($digest -ne $archive.Sha256.ToUpper()) {
		Remove-Item $dest -Force
		throw "$($archive.Name) from $($archive.Url) hashed $digest, expected $($archive.Sha256)"
	}
}

# The remaining hosts are healthy but numerous, and a build this long shouldn't
# die on one of them blinking.  --fast-build lets a retry resume rather than
# start the whole stack again; fetched archives are kept either way.
$gvsArgs = @(
	'build',
	'--build-dir', $BuildRoot,
	'--platform', $Platform,
	'--configuration', 'release',
	'--enable-gi'
)
$attempts = 3
for ($attempt = 1; $attempt -le $attempts; $attempt++) {
	$attemptArgs = @($gvsArgs)
	if ($attempt -gt 1) { $attemptArgs += '--fast-build' }
	$attemptArgs += $GvsbuildProjects

	& gvsbuild @attemptArgs
	if ($LASTEXITCODE -eq 0) { break }
	if ($attempt -eq $attempts) {
		throw "gvsbuild build failed with exit code $LASTEXITCODE after $attempts attempts"
	}
	Write-Warning "gvsbuild attempt $attempt failed with exit code $LASTEXITCODE; retrying"
	Start-Sleep -Seconds 30
}

Write-Step 'jansson (static)'
$janssonSrc = Get-SourceTree "https://github.com/akheron/jansson/archive/refs/tags/$JanssonTag.zip" "jansson-$JanssonTag"
$janssonBuild = Join-Path $janssonSrc 'build-ci'
$janssonPrefix = Join-Path $BuildRoot 'jansson'
Invoke-Checked 'jansson configure' {
	cmake -S $janssonSrc -B $janssonBuild -G $CMakeGenerator -A $Platform `
		-DCMAKE_INSTALL_PREFIX="$janssonPrefix" `
		-DJANSSON_BUILD_SHARED_LIBS=OFF `
		-DJANSSON_BUILD_DOCS=OFF `
		-DJANSSON_EXAMPLES=OFF `
		-DJANSSON_WITHOUT_TESTS=ON
}
Invoke-Checked 'jansson build' { cmake --build $janssonBuild --config Release --target install }

Write-Step 'libwebsockets (static, against gvsbuild OpenSSL)'
$lwsSrc = Get-SourceTree "https://github.com/warmcat/libwebsockets/archive/refs/tags/$LibWebSocketsTag.zip" "libwebsockets-$LibWebSocketsTag"
$lwsBuild = Join-Path $lwsSrc 'build-ci'
$lwsPrefix = Join-Path $BuildRoot 'libwebsockets'
Invoke-Checked 'libwebsockets configure' {
	cmake -S $lwsSrc -B $lwsBuild -G $CMakeGenerator -A $Platform `
		-DCMAKE_INSTALL_PREFIX="$lwsPrefix" `
		-DCMAKE_PREFIX_PATH="$prefix" `
		-DLWS_WITH_STATIC=ON `
		-DLWS_WITH_SHARED=OFF `
		-DLWS_WITH_SSL=ON `
		-DLWS_OPENSSL_INCLUDE_DIRS="$prefix\include" `
		-DLWS_OPENSSL_LIBRARIES="$prefix\lib\libssl.lib;$prefix\lib\libcrypto.lib" `
		-DLWS_WITHOUT_TESTAPPS=ON `
		-DLWS_WITHOUT_TEST_SERVER=ON `
		-DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON `
		-DLWS_WITHOUT_TEST_PING=ON `
		-DLWS_WITHOUT_TEST_CLIENT=ON `
		-DLWS_WITH_MINIMAL_EXAMPLES=OFF
}
Invoke-Checked 'libwebsockets build' { cmake --build $lwsBuild --config Release --target install }

# The static build installs websockets_static.lib; DepLibs asks for
# websockets.lib, which is what a shared build would have produced.
$lwsStatic = Join-Path $lwsPrefix 'lib\websockets_static.lib'
if (Test-Path $lwsStatic) {
	Copy-Item $lwsStatic (Join-Path $lwsPrefix 'lib\websockets.lib') -Force
}

Write-Step 'WinSparkle'
$wsPrefix = Join-Path $BuildRoot "WinSparkle\$Platform"
$wsSrc = Get-SourceTree "https://github.com/vslavik/winsparkle/releases/download/v$WinSparkleVersion/WinSparkle-$WinSparkleVersion.zip" "WinSparkle-$WinSparkleVersion"
New-Item -ItemType Directory -Force -Path $wsPrefix | Out-Null

# The release zip carries every architecture; pick ours by path rather than by
# guessing at the layout, which has moved between releases.
$archPattern = if ($Platform -eq 'x64') { '(?i)(x64|amd64)' } else { '(?i)win32|x86' }
foreach ($file in 'WinSparkle.dll', 'WinSparkle.lib') {
	$found = Get-ChildItem -Path $wsSrc -Recurse -Filter $file |
		Where-Object { $_.FullName -match $archPattern } |
		Select-Object -First 1
	if (-not $found) {
		throw "no $Platform $file in the WinSparkle $WinSparkleVersion zip"
	}
	Copy-Item $found.FullName $wsPrefix -Force
}
foreach ($file in 'winsparkle.h', 'COPYING') {
	$found = Get-ChildItem -Path $wsSrc -Recurse -Filter $file | Select-Object -First 1
	if (-not $found) {
		throw "no $file in the WinSparkle $WinSparkleVersion zip"
	}
	Copy-Item $found.FullName $wsPrefix -Force
}

Write-Step 'CA bundle'
$certDir = Join-Path $BuildRoot 'cert'
New-Item -ItemType Directory -Force -Path $certDir | Out-Null
Invoke-WebRequest -Uri 'https://curl.se/ca/cacert.pem' -OutFile (Join-Path $certDir 'cacert.pem') -UseBasicParsing

Write-Step 'done'
Write-Host "gvsbuild prefix:  $prefix"
Write-Host "jansson:          $janssonPrefix"
Write-Host "libwebsockets:    $lwsPrefix"
Write-Host "WinSparkle:       $wsPrefix"
