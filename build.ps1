# build.ps1 - C Project Build Script

# 1. Compiler and Output Executable Setup
function Resolve-ToolPath ($cmdName, $defaultPath) {
    $found = Get-Command $cmdName -ErrorAction SilentlyContinue
    if ($found) {
        return $found.Source
    }
    if (Test-Path $defaultPath) {
        return $defaultPath
    }
    return $null
}

$GCC_PATH = Resolve-ToolPath "gcc" "C:\msys64\mingw64\bin\gcc.exe"
$CLANG_PATH = Resolve-ToolPath "clang" "C:\Program Files\LLVM\bin\clang.exe"
$OBJCOPY_PATH = Resolve-ToolPath "objcopy" "C:\msys64\mingw64\bin\objcopy.exe"
$AR_PATH = Resolve-ToolPath "ar" "C:\msys64\mingw64\bin\ar.exe"

$GCC = if ($GCC_PATH) { $GCC_PATH } else { "gcc" }
$CLANG = if ($CLANG_PATH) { $CLANG_PATH } else { $GCC }
$AR = if ($AR_PATH) { $AR_PATH } else { "ar" }
$OUTPUT = "build\tiny_app.exe"

# 2. Compiler & Linker Flags
$COMMON_CFLAGS = @(
    "-std=c99",
    "-DENABLE_CUSTOM_MINI_ENGINE",
    "-DMINI_TLS",
    "-D_GNU_SOURCE",
    "-g",
    "-gcodeview",
    "-O2",
    "-finput-charset=UTF-8",
    "-fexec-charset=UTF-8"
)

$LDFLAGS = @(
    "-mwindows",
    "-fuse-ld=lld",
    "-Wl,--pdb=build\tiny_app.pdb"
)

# 3. Include Directories (-I)
$INCLUDES = @(
    "-Iinclude",
    "-Ilibs\quickjs",
    "-Ilibs\stb",
    "-Ilibs\mbedtls\include",
    "-ID:\glfw-3.4.bin.WIN64\include"
)

# 4. Source Files (Application Sources)
$SOURCES = @(
    "src\main.c",
    "src\mini_renderer.c",
    "src\mini_gradient.c",
    "src\mini_css.c",
    "src\mini_dom.c",
    "src\mini_events.c",
    "src\mini_js_bridge.c",
    "src\mini_vfs_decrypt.c",
    "src\mini_cdp_server.c",
    "src\mini_cdp_domains.c",
    "src\mini_png.c",
    "src\mini_html5.c",
    "src\mini_diag.c",
    "src\mini_net.c",
    "src\mini_log.c",
    "src\mini_crash.c",
    "src\mini_cookies.c",
    "src\mini_httpcache.c",
    "src\mini_policy.c",
    "src\mini_websocket.c",
    "src\mini_h2.c",
    "src\mini_h3.c",
    "src\mini_eventloop.c",
    "src\mini_devtools.c",
    "src\mini_webgl_ext.c",
    "src\mini_bidi.c",
    "src\mini_shaping.c",
    "src\mini_audio.c",
    "src\mini_native.c",
    "src\mini_worker.c"
)

# 5. Library Directories (-L) and Libraries (-l)
$LIBS = @(
    "build\quickjs.o",
    "build\libregexp.o",
    "build\libunicode.o",
    "build\dtoa.o",
    "-Llibs\mbedtls\library",
    "-lmbedtls",
    "-LD:\glfw-3.4.bin.WIN64\lib-mingw-w64",
    "-lglfw3dll",
    "-lopengl32",
    "-lgdi32",
    "-luser32",
    "-limm32",
    "-lws2_32",
    "-lbcrypt",
    "-ldbghelp",
    "-lwinmm",
    "-lm"
)

# 6. Ensure Output Directory Exists
$OutputDir = Split-Path $OUTPUT
$AppObjDir = Join-Path $OutputDir "app_objs"
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}
if (-not (Test-Path $AppObjDir)) {
    New-Item -ItemType Directory -Path $AppObjDir | Out-Null
}

# 6a. Compile QuickJS with Clang + CodeView (Full source-level PDB symbols)
$qjsSources = @(
    @{ Src = "libs\quickjs\quickjs.c"; Obj = "build\quickjs.o" },
    @{ Src = "libs\quickjs\libregexp.c"; Obj = "build\libregexp.o" },
    @{ Src = "libs\quickjs\libunicode.c"; Obj = "build\libunicode.o" },
    @{ Src = "libs\quickjs\dtoa.c"; Obj = "build\dtoa.o" }
)

$qjsCompiler = $CLANG

foreach ($item in $qjsSources) {
    $srcPath = $item.Src
    $objPath = $item.Obj
    $needCompile = $false

    if (-not (Test-Path $objPath)) {
        $needCompile = $true
    }
    elseif ((Get-Item $srcPath).LastWriteTime -gt (Get-Item $objPath).LastWriteTime) {
        $needCompile = $true
    }

    if ($needCompile) {
        Write-Host "Compiling $($item.Src)..." -ForegroundColor Cyan
        & $qjsCompiler -target x86_64-w64-windows-gnu -std=c99 -D_GNU_SOURCE -DCONFIG_VERSION="`"2024-01-13`"" -Ilibs\quickjs -O2 -g -gcodeview -c $srcPath -o $objPath
        if ($LASTEXITCODE -ne 0) {
            Write-Host "QuickJS compilation failed for $($item.Src)" -ForegroundColor Red
            exit 1
        }
    }
}

# 6b. Build vendored mbedtls (TLS) into a single static lib if absent or outdated
$mbedLib = "libs\mbedtls\library\libmbedtls.a"
$mbedSourceFiles = Get-ChildItem -Path "libs\mbedtls\library\*.c" -ErrorAction SilentlyContinue
$needMbedBuild = -not (Test-Path $mbedLib)

if (-not $needMbedBuild -and $mbedSourceFiles) {
    $libTime = (Get-Item $mbedLib).LastWriteTime
    foreach ($s in $mbedSourceFiles) {
        if ($s.LastWriteTime -gt $libTime) {
            $needMbedBuild = $true
            break
        }
    }
}

if ($needMbedBuild -and $mbedSourceFiles) {
    Write-Host "Building mbedtls (TLS) static lib..." -ForegroundColor Cyan
    $mbedInc = @("-Ilibs\mbedtls\include", "-Ilibs\mbedtls\library")
    $mbedObjs = @()
    foreach ($s in $mbedSourceFiles) {
        $o = "libs\mbedtls\library\" + $s.BaseName + ".o"
        & $GCC -c $s.FullName @mbedInc -Os -o $o
        if ($LASTEXITCODE -ne 0) {
            Write-Host "mbedtls compile failed: $($s.Name)" -ForegroundColor Red
            exit 1
        }
        $mbedObjs += $o
    }
    & $AR rcs $mbedLib @mbedObjs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "mbedtls ar failed" -ForegroundColor Red
        exit 1
    }
    Remove-Item $mbedObjs -ErrorAction SilentlyContinue
    Write-Host "mbedtls built: $mbedLib" -ForegroundColor Green
}

# 7. Incremental Compile Application Source Files
$appObjs = @()
$relinkNeeded = -not (Test-Path $OUTPUT)

foreach ($src in $SOURCES) {
    $srcItem = Get-Item $src
    $objName = $srcItem.BaseName + ".o"
    $objPath = Join-Path $AppObjDir $objName
    $appObjs += $objPath

    $compileThis = $false
    if (-not (Test-Path $objPath)) {
        $compileThis = $true
    }
    elseif ($srcItem.LastWriteTime -gt (Get-Item $objPath).LastWriteTime) {
        $compileThis = $true
    }

    if ($compileThis) {
        Write-Host "Compiling $src ..." -ForegroundColor Gray
        & $GCC -c $src @COMMON_CFLAGS @INCLUDES -o $objPath
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Compilation failed for $src" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        $relinkNeeded = $true
    }
}

$rcObj = "build\app_objs\app_icon.res.o"
if (Test-Path "src\app.rc") {
    $rcItem = Get-Item "src\app.rc"
    $rcNeeded = $false
    if (-not (Test-Path $rcObj)) {
        $rcNeeded = $true
    }
    elseif ($rcItem.LastWriteTime -gt (Get-Item $rcObj).LastWriteTime) {
        $rcNeeded = $true
    }
    if ($rcNeeded) {
        Write-Host "Compiling resources src\app.rc ..." -ForegroundColor Gray
        & windres "src\app.rc" -O coff -o $rcObj
        $relinkNeeded = $true
    }
    $appObjs += $rcObj
}

# 8. Link Executable
if ($relinkNeeded) {
    Write-Host "Linking $OUTPUT ..." -ForegroundColor Cyan
    & $GCC @COMMON_CFLAGS @LDFLAGS $appObjs -o $OUTPUT @LIBS

    if ($LASTEXITCODE -ne 0) {
        Write-Host "Linking failed with exit code: $LASTEXITCODE" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    Write-Host "Build succeeded! Output: $OUTPUT" -ForegroundColor Green
    if ($OBJCOPY_PATH) {
        & $OBJCOPY_PATH --only-keep-debug $OUTPUT "build\tiny_app.debug"
        Write-Host "Generated debug symbols: build\tiny_app.debug" -ForegroundColor Green
    }
}
else {
    Write-Host "Everything is up to date." -ForegroundColor Green
}