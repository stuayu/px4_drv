
# カレントディレクトリに移動
if ($MyInvocation.MyCommand.Path -ne $null) {
    $CurrentPath = (Split-Path $MyInvocation.MyCommand.Path -Parent)
} else {
    $CurrentPath = $PSScriptRoot
}
Set-Location $CurrentPath

# コンソールウインドウのサイズを変更
$ErrorActionPreference = 'Stop'  # エラーを確実に catch する
try { (Get-Host).UI.RawUI.BufferSize = New-Object System.Management.Automation.Host.Size(120,25) } catch {}
try { (Get-Host).UI.RawUI.WindowSize = New-Object System.Management.Automation.Host.Size(120,25) } catch {}

# MSBuild のパスを環境変数 PATH に追加
# GitHub Actions では microsoft/setup-msbuild が PATH を設定するため、その設定を優先する
# ローカル環境では Visual Studio 2022 のエディション差を吸収するため、vswhere で MSBuild を探索する
if ((Get-Command msbuild -ErrorAction SilentlyContinue) -eq $null) {
    $vswhere_path = Join-Path -Path ${env:ProgramFiles(x86)} -ChildPath 'Microsoft Visual Studio\Installer\vswhere.exe'

    if ((Test-Path $vswhere_path) -eq $True) {
        $msbuild_file_path = & $vswhere_path -latest -version '[17.0,18.0)' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1

        if ($msbuild_file_path -ne $null) {
            $msbuild_dir_path = Split-Path $msbuild_file_path -Parent
            $env:PATH = "$env:PATH;$msbuild_dir_path"
        }
    }
}

if ((Get-Command msbuild -ErrorAction SilentlyContinue) -eq $null) {
    throw 'MSBuild was not found. Install Visual Studio 2022 with MSBuild.'
}

# カタログファイルと INF の不一致をビルド前に検出し、署名対象と異なるドライバーパッケージの生成を防ぐ
& 'pkg/signing-tools/verify.ps1' -DriverPath 'pkg/inf'

# MSBuild を使用してソリューションをビルド
$build_platforms = @('x86', 'x64')
foreach ($build_platform in $build_platforms) {
    msbuild px4_winusb.sln /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"

    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed. platform: $build_platform"
    }

    # Linux 版と WinUSB 版が共有する TS 同期判定を実際の入力値で検証
    msbuild tests/ts_sync_condition_test.vcxproj /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) {
        throw "TS sync condition test build failed. platform: $build_platform"
    }
    & 'tests/ts_sync_condition_test.ps1' -Platform $build_platform

    # 実機に依存しないカード抜去・再挿入・接触不良の状態遷移を毎回検証
    & "build/$build_platform/Release-static/smart_card_state_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Smart card state test failed. platform: $build_platform"
    }

    # 選局時の TS バッファ初期化が読み書きと競合しても止まらないことを毎回検証
    & "build/$build_platform/Release-static/ringbuffer_purge_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Ring buffer purge test failed. platform: $build_platform"
    }

    # 模擬 I2C で ISDB-C・J.83B の設定と失敗時の復帰を検証
    & "build/$build_platform/Release-static/j83b_driver_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "J83B driver test failed. platform: $build_platform"
    }

    # 作った TS で、TSMF の TS 番号による選択・ヘッダーのない搬送波の素通し・分割 TLV の自動判定を検証
    & "build/$build_platform/Release-static/tsmf_filter_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "TSMF filter test failed. platform: $build_platform"
    }

    # Host 強制終了後の T230C USB pipe 回収順を検証
    msbuild tests/t230c_usb_recovery_test.vcxproj /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) {
        throw "T230C USB recovery test build failed. platform: $build_platform"
    }
    & "build/$build_platform/Release-static/t230c_usb_recovery_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "T230C USB recovery test failed. platform: $build_platform"
    }

    # ISDB-C と J.83B の ChSet の列の解釈と不正な行の扱い、配布の ChSet がすべて読めることを検証
    & "build/$build_platform/Release-static/chset_isdbc_test.exe" "pkg/BonDriver_PX4/BonDriver_PX4-C.ChSet.txt" "pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt"
    if ($LASTEXITCODE -ne 0) {
        throw "ISDB-C ChSet test failed. platform: $build_platform"
    }
    # T230C 用の ISDB-C ChSet も同じ列の規則で読めることを検証
    & "build/$build_platform/Release-static/chset_isdbc_test.exe" "pkg/BonDriver_PX4/BonDriver_T230C-C.ChSet.txt" "pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt"
    if ($LASTEXITCODE -ne 0) {
        throw "T230C ISDB-C ChSet test failed. platform: $build_platform"
    }

    msbuild tests/siano_protocol_test.vcxproj /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) {
        throw "Siano protocol test build failed. platform: $build_platform"
    }
    & "build/$build_platform/Release-static/siano_protocol_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Siano protocol test failed. platform: $build_platform"
    }

    # Shared diagnostics, TS continuity, USB history, and fixed-point C/N conversion.
    msbuild tests/diagnostics_test.vcxproj /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) {
        throw "Diagnostics test build failed. platform: $build_platform"
    }
    & "build/$build_platform/Release-static/diagnostics_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Diagnostics test failed. platform: $build_platform"
    }

    msbuild tests/signal_stat_test.vcxproj /t:"Rebuild" /p:"Configuration=Release-static;Platform=$build_platform;PlatformToolset=v143"
    if ($LASTEXITCODE -ne 0) {
        throw "Signal statistics test build failed. platform: $build_platform"
    }
    & "build/$build_platform/Release-static/signal_stat_test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "Signal statistics test failed. platform: $build_platform"
    }
}

# dist/ フォルダにビルドされたファイルをコピー
# フォルダの作成
Remove-Item -Recurse -Force dist/ -ErrorAction SilentlyContinue
New-Item -ItemType Directory dist/
New-Item -ItemType Directory dist/BonDriver_PX4_32bit
New-Item -ItemType Directory dist/BonDriver_PX4_64bit
New-Item -ItemType Directory dist/BonDriver_PX-MLT_32bit
New-Item -ItemType Directory dist/BonDriver_PX-MLT_64bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056_32bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056_64bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056N_32bit
New-Item -ItemType Directory dist/BonDriver_ISDB2056N_64bit
New-Item -ItemType Directory dist/BonDriver_ISDBT2071_32bit
New-Item -ItemType Directory dist/BonDriver_ISDBT2071_64bit
New-Item -ItemType Directory dist/BonDriver_PX-M1UR_32bit
New-Item -ItemType Directory dist/BonDriver_PX-M1UR_64bit
New-Item -ItemType Directory dist/BonDriver_PX-S1UR_32bit
New-Item -ItemType Directory dist/BonDriver_PX-S1UR_64bit
New-Item -ItemType Directory dist/BonDriver_PX-S1UD_32bit
New-Item -ItemType Directory dist/BonDriver_PX-S1UD_64bit
New-Item -ItemType Directory dist/BonDriver_PX-Q1UD_32bit
New-Item -ItemType Directory dist/BonDriver_PX-Q1UD_64bit
New-Item -ItemType Directory dist/BonDriver_T230C_32bit
New-Item -ItemType Directory dist/BonDriver_T230C_64bit

# BonDriver_PX4_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_32bit/BonDriver_PX4-T.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ini dist/BonDriver_PX4_32bit/BonDriver_PX4-T.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX4_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_32bit/BonDriver_PX4-S.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ini dist/BonDriver_PX4_32bit/BonDriver_PX4-S.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX4_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX4_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX4_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX4_32bit/it930x-firmware.bin

# BonDriver_PX4_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_64bit/BonDriver_PX4-T.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ini dist/BonDriver_PX4_64bit/BonDriver_PX4-T.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX4_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX4_64bit/BonDriver_PX4-S.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ini dist/BonDriver_PX4_64bit/BonDriver_PX4-S.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX4_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX4_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX4_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX4_64bit/it930x-firmware.bin

# BonDriver_PX-MLT_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-MLT_32bit/BonDriver_PX-MLT.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-MLT.ini dist/BonDriver_PX-MLT_32bit/BonDriver_PX-MLT.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-C.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-C.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt dist/BonDriver_PX-MLT_32bit/BonDriver_PX4-J83B.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-MLT_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-MLT_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-MLT_32bit/it930x-firmware.bin

# BonDriver_PX-MLT_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-MLT_64bit/BonDriver_PX-MLT.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-MLT.ini dist/BonDriver_PX-MLT_64bit/BonDriver_PX-MLT.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-C.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-C.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt dist/BonDriver_PX-MLT_64bit/BonDriver_PX4-J83B.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-MLT_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-MLT_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-MLT_64bit/it930x-firmware.bin

# BonDriver_ISDB2056_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056_32bit/BonDriver_ISDB2056.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056.ini dist/BonDriver_ISDB2056_32bit/BonDriver_ISDB2056.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056_32bit/it930x-firmware.bin

# BonDriver_ISDB2056_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056_64bit/BonDriver_ISDB2056.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056.ini dist/BonDriver_ISDB2056_64bit/BonDriver_ISDB2056.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056_64bit/it930x-firmware.bin

# BonDriver_ISDB2056N_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056N_32bit/BonDriver_ISDB2056N.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056N.ini dist/BonDriver_ISDB2056N_32bit/BonDriver_ISDB2056N.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056N_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056N_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056N_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056N_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056N_32bit/it930x-firmware.bin

# BonDriver_ISDB2056N_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDB2056N_64bit/BonDriver_ISDB2056N.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDB2056N.ini dist/BonDriver_ISDB2056N_64bit/BonDriver_ISDB2056N.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDB2056N_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_ISDB2056N_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDB2056N_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDB2056N_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDB2056N_64bit/it930x-firmware.bin

# BonDriver_PX-M1UR_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-M1UR_32bit/BonDriver_PX-M1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-M1UR.ini dist/BonDriver_PX-M1UR_32bit/BonDriver_PX-M1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-M1UR_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-M1UR_32bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-M1UR_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-M1UR_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-M1UR_32bit/it930x-firmware.bin

# BonDriver_PX-M1UR_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-M1UR_64bit/BonDriver_PX-M1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-M1UR.ini dist/BonDriver_PX-M1UR_64bit/BonDriver_PX-M1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-M1UR_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-S.ChSet.txt dist/BonDriver_PX-M1UR_64bit/BonDriver_PX4-S.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-M1UR_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-M1UR_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-M1UR_64bit/it930x-firmware.bin

# BonDriver_ISDBT2071_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDBT2071_32bit/BonDriver_ISDBT2071.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDBT2071.ini dist/BonDriver_ISDBT2071_32bit/BonDriver_ISDBT2071.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDBT2071_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDBT2071_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDBT2071_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDBT2071_32bit/it930x-firmware.bin

# BonDriver_ISDBT2071_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_ISDBT2071_64bit/BonDriver_ISDBT2071.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_ISDBT2071.ini dist/BonDriver_ISDBT2071_64bit/BonDriver_ISDBT2071.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_ISDBT2071_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_ISDBT2071_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_ISDBT2071_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_ISDBT2071_64bit/it930x-firmware.bin

# BonDriver_PX-S1UR_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-S1UR_32bit/BonDriver_PX-S1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-S1UR.ini dist/BonDriver_PX-S1UR_32bit/BonDriver_PX-S1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-S1UR_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-S1UR_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-S1UR_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-S1UR_32bit/it930x-firmware.bin

# BonDriver_PX-S1UR_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-S1UR_64bit/BonDriver_PX-S1UR.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-S1UR.ini dist/BonDriver_PX-S1UR_64bit/BonDriver_PX-S1UR.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-S1UR_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-S1UR_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-S1UR_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-S1UR_64bit/it930x-firmware.bin

# BonDriver_PX-S1UD_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-S1UD_32bit/BonDriver_PX-S1UD.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-S1UD.ini dist/BonDriver_PX-S1UD_32bit/BonDriver_PX-S1UD.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-S1UD_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-S1UD_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-S1UD_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-S1UD_32bit/it930x-firmware.bin
Copy-Item pkg/DriverHost_PX4/isdbt_rio.inp dist/BonDriver_PX-S1UD_32bit/isdbt_rio.inp
Copy-Item pkg/DriverHost_PX4/LICENCE.siano dist/BonDriver_PX-S1UD_32bit/LICENCE.siano

# BonDriver_PX-S1UD_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-S1UD_64bit/BonDriver_PX-S1UD.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-S1UD.ini dist/BonDriver_PX-S1UD_64bit/BonDriver_PX-S1UD.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-S1UD_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-S1UD_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-S1UD_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-S1UD_64bit/it930x-firmware.bin
Copy-Item pkg/DriverHost_PX4/isdbt_rio.inp dist/BonDriver_PX-S1UD_64bit/isdbt_rio.inp
Copy-Item pkg/DriverHost_PX4/LICENCE.siano dist/BonDriver_PX-S1UD_64bit/LICENCE.siano

# BonDriver_PX-Q1UD_32bit のファイルをコピー
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-Q1UD_32bit/BonDriver_PX-Q1UD.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-Q1UD.ini dist/BonDriver_PX-Q1UD_32bit/BonDriver_PX-Q1UD.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-Q1UD_32bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-Q1UD_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-Q1UD_32bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-Q1UD_32bit/it930x-firmware.bin
Copy-Item pkg/DriverHost_PX4/isdbt_rio.inp dist/BonDriver_PX-Q1UD_32bit/isdbt_rio.inp
Copy-Item pkg/DriverHost_PX4/LICENCE.siano dist/BonDriver_PX-Q1UD_32bit/LICENCE.siano

# BonDriver_PX-Q1UD_64bit のファイルをコピー
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_PX-Q1UD_64bit/BonDriver_PX-Q1UD.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_PX-Q1UD.ini dist/BonDriver_PX-Q1UD_64bit/BonDriver_PX-Q1UD.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt dist/BonDriver_PX-Q1UD_64bit/BonDriver_PX4-T.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_PX-Q1UD_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_PX-Q1UD_64bit/DriverHost_PX4.ini
Copy-Item pkg/DriverHost_PX4/it930x-firmware.bin dist/BonDriver_PX-Q1UD_64bit/it930x-firmware.bin
Copy-Item pkg/DriverHost_PX4/isdbt_rio.inp dist/BonDriver_PX-Q1UD_64bit/isdbt_rio.inp
Copy-Item pkg/DriverHost_PX4/LICENCE.siano dist/BonDriver_PX-Q1UD_64bit/LICENCE.siano

# BonDriver_T230C_32bit のファイルをコピー
# Si2168 / Si2141 の firmware は再配布できないため、利用者が配置する
Copy-Item build/x86/Release-static/BonDriver_PX4.dll dist/BonDriver_T230C_32bit/BonDriver_T230C.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_T230C.ini dist/BonDriver_T230C_32bit/BonDriver_T230C.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_T230C-C.ChSet.txt dist/BonDriver_T230C_32bit/BonDriver_T230C-C.ChSet.txt
Copy-Item build/x86/Release-static/DriverHost_PX4.exe dist/BonDriver_T230C_32bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_T230C_32bit/DriverHost_PX4.ini

# BonDriver_T230C_64bit のファイルをコピー
# Si2168 / Si2141 の firmware は再配布できないため、利用者が配置する
Copy-Item build/x64/Release-static/BonDriver_PX4.dll dist/BonDriver_T230C_64bit/BonDriver_T230C.dll
Copy-Item pkg/BonDriver_PX4/BonDriver_T230C.ini dist/BonDriver_T230C_64bit/BonDriver_T230C.ini
Copy-Item pkg/BonDriver_PX4/BonDriver_T230C-C.ChSet.txt dist/BonDriver_T230C_64bit/BonDriver_T230C-C.ChSet.txt
Copy-Item build/x64/Release-static/DriverHost_PX4.exe dist/BonDriver_T230C_64bit/DriverHost_PX4.exe
Copy-Item pkg/DriverHost_PX4/DriverHost_PX4.ini dist/BonDriver_T230C_64bit/DriverHost_PX4.ini

# 各 BonDriver と同じビット数の WinSCard.dll を配置
# WinUSB 版の全対応機種で内蔵カードリーダーを利用できる
Get-ChildItem dist/ -Directory -Filter 'BonDriver_*' |
    Where-Object { $_.Name -notmatch 'PX-(S1UD|Q1UD)_' } | ForEach-Object {
    $win_scard_platform = if ($_.Name.EndsWith('_32bit')) { 'x86' } else { 'x64' }
    Copy-Item "build/$win_scard_platform/Release-static/WinSCard.dll" $_.FullName
}

# inf ファイルをコピー
Copy-Item -Recurse pkg/inf/ dist/Driver

# コピー後の最終配布物も再度検査
& 'pkg/signing-tools/verify.ps1' -DriverPath 'dist/Driver'

Write-Host '    '
Write-Host '    '
Write-Host '    BonDriver のビルドとパッケージングを完了しました。'
Write-Host '    ビルドしたファイルは dist/ フォルダに配置されています。'
Write-Host '    '
Start-Sleep -Seconds 5
