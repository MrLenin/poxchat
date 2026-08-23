#Requires -Version 5.1
<#
.SYNOPSIS
	Build the Windows dependency stack that win32/poxchat.props expects.

.DESCRIPTION
	Everything lands under -BuildRoot (default C:\gtk-build), in the layout the
	solution's UserMacros point at:

	  gtk\<plat>\release  gvsbuild prefix: GTK4, OpenSSL, libxml2, sqlite,
	                      libcurl, enchant, and gettext (win32\nls compiles
	                      the .po catalogues with its msgfmt.exe).  LuaJIT is
	                      built below and installed into this prefix, which is
	                      where poxchat.props looks for lua51.lib and
	                      include\luajit-2.1
	  jansson\            static jansson -- DepLibs wants jansson.lib and
	                      copy.vcxproj ships no jansson dll
	  libwebsockets\      static libwebsockets, same reasoning
	  WinSparkle\<plat>\  prebuilt release, for the upd plugin
	  python-embed\       CPython embeddable runtime, staged whole into the
	                      tree so the python plugin needs no system Python
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
	# LuaJIT's v2.1 branch is release-less by policy, so pin a commit rather
	# than the floating v2.1.ROLLING tag.
	[string] $LuaJITCommit = '1ee778a4e37122d8ca7d5733c590a47dafd6b15c',
	# The runtime the python plugin ships.  Its minor version is load-bearing
	# in three other places -- Python3Lib in win32\poxchat.props, setup-python
	# in the workflow, and the python313.* names in poxchat.iss.tt -- so bump
	# all four together, and the hash with the version.
	[string] $PythonEmbedVersion = '3.13.9',
	[string] $PythonEmbedSha256 = '91d828c2da3a029b41699e918674a0cb379c02cf20dab9c501306885f837402a',
	[string] $CMakeGenerator = 'Visual Studio 17 2022'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'	# Invoke-WebRequest crawls with the progress bar on

# gvsbuild project names, not pkg-config names.  gettext is here for msgfmt.exe,
# which win32\nls compiles the .po catalogues with.
#
# luajit is deliberately not asked of gvsbuild: it runs '.\msvcbuild' through
# CreateProcess, which cannot launch a .bat file and finds nothing without the
# extension -- it fails identically every time.  LuaJIT is built directly below
# instead.  lgi and --enable-gi (which exists to produce lgi's typelibs) stay
# out with it: the lua plugin itself needs neither, they were only ever for
# GObject bindings inside Lua scripts.
$GvsbuildProjects = @(
	'gtk4',
	'openssl',
	'libxml2',
	'sqlite',
	'libcurl',
	'enchant',
	'gettext'
)

# gvsbuild pulls each project's tarball from that project's own upstream, and
# two of those upstreams are simply down: cairographics.org (cairo, pixman) and
# icon-theme.freedesktop.org (hicolor-icon-theme) both time out on connect, from
# the runners and from every other network tried.  Retrying cannot revive a host
# that is off, so seed those archives instead.
#
# Debian's orig tarballs are the upstream files byte for byte: every hash below
# is the one gvsbuild itself records for that version, checked against what
# Debian serves.  We verify the digest after download too, so a substituted file
# fails loudly rather than quietly building something nobody vetted.
#
# These pin exact versions, which is why $GvsbuildVersion is pinned as well --
# bump them together, and drop an entry once its upstream comes back.
$SeededArchives = @(
	@{
		Name = 'pixman-0.46.4.tar.gz'
		Url = 'http://deb.debian.org/debian/pool/main/p/pixman/pixman_0.46.4.orig.tar.gz'
		Sha256 = 'd09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c'
	},
	@{
		Name = 'cairo-1.18.4.tar.xz'
		Url = 'http://deb.debian.org/debian/pool/main/c/cairo/cairo_1.18.4.orig.tar.xz'
		Sha256 = '445ed8208a6e4823de1226a74ca319d3600e83f6369f99b14265006599c32ccb'
	},
	@{
		Name = 'hicolor-icon-theme-0.18.tar.xz'
		Url = 'http://deb.debian.org/debian/pool/main/h/hicolor-icon-theme/hicolor-icon-theme_0.18.orig.tar.xz'
		Sha256 = 'db0e50a80aa3bf64bb45cbca5cf9f75efd9348cf2ac690b907435238c3cf81d7'
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
# --fast-build from the outset: it skips any project whose .wingtk-built marker
# is already in the build tree, so a restored cache resumes the stack instead of
# recompiling it.  Its documented caveat -- stale results if the patches or the
# build script change underneath it -- is covered by pinning $GvsbuildVersion.
$gvsArgs = @(
	'build',
	'--build-dir', $BuildRoot,
	'--platform', $Platform,
	'--configuration', 'release',
	'--fast-build'
)
$attempts = 3
for ($attempt = 1; $attempt -le $attempts; $attempt++) {
	$attemptArgs = @($gvsArgs)
	$attemptArgs += $GvsbuildProjects

	& gvsbuild @attemptArgs
	if ($LASTEXITCODE -eq 0) { break }
	if ($attempt -eq $attempts) {
		throw "gvsbuild build failed with exit code $LASTEXITCODE after $attempts attempts"
	}
	Write-Warning "gvsbuild attempt $attempt failed with exit code $LASTEXITCODE; retrying"
	Start-Sleep -Seconds 30
}

# gvsbuild builds libcurl with CMake, which names the import library
# libcurl_imp.lib -- gvsbuild patches libcurl.pc for exactly this reason.
# DepLibs in poxchat.props asks for libcurl.lib, the name curl's own Windows
# builds use, so give it that name; same fixup as websockets_static.lib below.
$curlImp = Join-Path $prefix 'lib\libcurl_imp.lib'
$curlLib = Join-Path $prefix 'lib\libcurl.lib'
if (-not (Test-Path $curlLib)) {
	if (-not (Test-Path $curlImp)) {
		throw "neither libcurl.lib nor libcurl_imp.lib in $prefix\lib"
	}
	Copy-Item $curlImp $curlLib -Force
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

Write-Step 'LuaJIT'
# msvcbuild.bat wants cl on PATH and to be run from src\.  Doing the vcvars
# call and the cd inside a generated .cmd sidesteps cmd /c's quote-stripping
# rules, which would otherwise eat the spaces in the Visual Studio path.
# msvcbuild.bat does not reliably set an exit code, so the proof of success is
# the artifacts themselves, checked below.  Always a full rebuild, but that is
# under a minute.
$ljSrc = Get-SourceTree "https://github.com/LuaJIT/LuaJIT/archive/$LuaJITCommit.zip" "LuaJIT-$($LuaJITCommit.Substring(0, 12))"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
	throw 'vswhere found no Visual Studio with the C++ toolset'
}
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$ljBuild = Join-Path $srcRoot 'build-luajit.cmd'
@(
	"call `"$vcvars`" || exit /b 1",
	"cd /d `"$ljSrc\src`" || exit /b 1",
	'call msvcbuild.bat'
) | Set-Content -Path $ljBuild -Encoding ascii
Invoke-Checked 'LuaJIT msvcbuild' { cmd /c $ljBuild }
foreach ($artifact in 'lua51.dll', 'lua51.lib') {
	if (-not (Test-Path (Join-Path $ljSrc "src\$artifact"))) {
		throw "LuaJIT build produced no $artifact"
	}
}
# Install into the gvsbuild prefix, in the layout poxchat.props expects:
# LuaInclude points at include\luajit-2.1, lua.vcxproj links lua51.lib out of
# the shared lib dir, and copy.vcxproj stages bin\lua51.dll when it exists.
$ljInclude = Join-Path $prefix 'include\luajit-2.1'
New-Item -ItemType Directory -Force -Path $ljInclude | Out-Null
Copy-Item (Join-Path $ljSrc 'src\lua51.dll') (Join-Path $prefix 'bin') -Force
Copy-Item (Join-Path $ljSrc 'src\lua51.lib') (Join-Path $prefix 'lib') -Force
foreach ($header in 'lua.h', 'luaconf.h', 'lualib.h', 'lauxlib.h', 'lua.hpp', 'luajit.h') {
	Copy-Item (Join-Path $ljSrc "src\$header") $ljInclude -Force
}

Write-Step "CPython $PythonEmbedVersion embeddable runtime"
# The python plugin is a cffi embedding: hcpython3.dll links python313.dll and
# needs a stdlib beside it at run time.  The embeddable package is CPython's
# designed-for-this layout -- python313.dll, python313.zip, python313._pth
# (whose '.' entry keeps the install dir on sys.path, where copy.vcxproj also
# drops _cffi_backend*.pyd), and the stdlib's extension modules.  The workflow
# stages this directory into the tree after the copy project runs.
$pyDir = Join-Path $BuildRoot 'python-embed'
$pyZip = Join-Path $srcRoot "python-$PythonEmbedVersion-embed-amd64.zip"
New-Item -ItemType Directory -Force -Path $srcRoot | Out-Null
if (-not (Test-Path $pyZip)) {
	$pyUrl = "https://www.python.org/ftp/python/$PythonEmbedVersion/python-$PythonEmbedVersion-embed-amd64.zip"
	Write-Host "downloading $pyUrl"
	Invoke-WebRequest -Uri $pyUrl -OutFile $pyZip -UseBasicParsing
}
$pyDigest = (Get-FileHash -Path $pyZip -Algorithm SHA256).Hash
if ($pyDigest -ne $PythonEmbedSha256.ToUpper()) {
	Remove-Item $pyZip -Force
	throw "python embeddable zip hashed $pyDigest, expected $PythonEmbedSha256"
}
if (Test-Path $pyDir) {
	Remove-Item $pyDir -Recurse -Force
}
Expand-Archive -Path $pyZip -DestinationPath $pyDir -Force

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
# winsparkle.h includes winsparkle-version.h, so take the whole include
# directory rather than naming the headers -- upd.vcxproj compiles against
# whatever this release ships, not a list written here.
$header = Get-ChildItem -Path $wsSrc -Recurse -Filter 'winsparkle.h' | Select-Object -First 1
if (-not $header) {
	throw "no winsparkle.h in the WinSparkle $WinSparkleVersion zip"
}
Copy-Item (Join-Path $header.Directory.FullName '*.h') $wsPrefix -Force

$copying = Get-ChildItem -Path $wsSrc -Recurse -Filter 'COPYING' | Select-Object -First 1
if (-not $copying) {
	throw "no COPYING in the WinSparkle $WinSparkleVersion zip"
}
Copy-Item $copying.FullName $wsPrefix -Force

Write-Step 'CA bundle'
$certDir = Join-Path $BuildRoot 'cert'
New-Item -ItemType Directory -Force -Path $certDir | Out-Null
Invoke-WebRequest -Uri 'https://curl.se/ca/cacert.pem' -OutFile (Join-Path $certDir 'cacert.pem') -UseBasicParsing

Write-Step 'done'
Write-Host "gvsbuild prefix:  $prefix (LuaJIT installed into it)"
Write-Host "jansson:          $janssonPrefix"
Write-Host "libwebsockets:    $lwsPrefix"
Write-Host "WinSparkle:       $wsPrefix"
Write-Host "python runtime:   $pyDir"
