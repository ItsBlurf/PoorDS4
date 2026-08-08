$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

$main = [IO.File]::ReadAllText(
    (Join-Path $repo 'payload\game_pad_bridge_main.c'))
$bridge = [IO.File]::ReadAllText(
    (Join-Path $repo 'payload\wireless_ds4.c'))
$makefile = [IO.File]::ReadAllText(
    (Join-Path $repo 'payload\Makefile'))

Assert-True ($makefile.Contains('RC_VERSION := 38')) `
    'Makefile RC version is not 38.'
Assert-True (-not $makefile.Contains('-lScePad')) `
    'Build still links libScePad despite using runtime-resolved game exports.'
Assert-True (-not $makefile.Contains('-lpthread')) `
    'Build still links pthread despite using runtime-resolved game functions.'
Assert-True (-not $makefile.Contains('-ldl')) `
    'Build still links libdl without a direct import.'
Assert-True ($makefile.Contains(
    'RUNTIME_LIBS := -nodefaultlibs -lc -lkernel_web -lSceLibcInternal')) `
    'Build does not declare its minimal SDK runtime dependencies.'
Assert-True ($makefile.Contains(
    'BRIDGE_LIBS  := $(RUNTIME_LIBS) -lSceUserService')) `
    'Automatic payload is missing its direct user-service dependency.'
Assert-True ($makefile.Contains(
    '$(STRIP) --strip-debug $(TARGET) $(STATUS_TARGET) $(STOP_TARGET)')) `
    'Release target does not remove workstation paths from DWARF data.'
Assert-True (-not $makefile.Contains('-lSceNet')) `
    'Build still links the SDK networking library without a network import.'
Assert-True ($makefile.Contains('TARGET        := PoorDS4rc$(RC_VERSION).elf')) `
    'Automatic payload does not use the PoorDS4 release name.'
Assert-True ($makefile.Contains('STATUS_TARGET := PoorDS4-status.elf')) `
    'Status payload does not use the PoorDS4 release name.'
Assert-True ($makefile.Contains('STOP_TARGET   := PoorDS4-stop.elf')) `
    'Stop payload does not use the PoorDS4 release name.'
Assert-True ($main.Contains('#define POORDS4_DATA_DIR   "/data/poords4"')) `
    'Runtime data directory does not use the PoorDS4 name.'
Assert-True ($main.Contains('install_result == 0 ? 5u : 15u')) `
    'Bounded retry for a transient fail-closed game snapshot is missing.'
Assert-True ($main.Contains(
    'restoring wireless reader before game scan')) `
    'Missing reader-before-game-install lifecycle gate.'
Assert-True ($main.Contains(
    'wireless_ds4_remote_reader_status(')) `
    'Pre-game reader gate does not validate the live reader snapshot.'
Assert-True ($main.Contains(
    'reader_preflight.connected != 0')) `
    'Pre-game reader gate does not require a connected source.'
Assert-True ($main.Contains(
    'reader_restart_required || !reader_ready')) `
    'Explicit reader-restart state does not gate game admission.'
Assert-True ($main.Contains(
    '"waiting_reader_stop"')) `
    'Failed reader stop is not retained for retry.'
Assert-True ($main.Contains(
    'reader unavailable after clean bridge removal')) `
    'Clean reader-failure recovery still exits instead of rediscovering.'
Assert-True ($main.Contains(
    'game exited before bridge-ready;')) `
    'Game-exit race before bridge readiness still terminates auto-watch.'
Assert-True ($main.Contains(
    'game_session_end_reason_name(end_reason)')) `
    'Session diagnostics do not include a named termination reason.'
Assert-True ($main.Contains(
    'g_pad_source = (PoorDS4PadSource){-1, -1, -1, 0};')) `
    'Reader-loss gate does not invalidate the stale source identity.'

$readerGate = $main.IndexOf(
    'restoring wireless reader before game scan',
    [StringComparison]::Ordinal)
$gameInstall = $main.IndexOf(
    'int install_result = wireless_ds4_game_bridge_install(',
    [StringComparison]::Ordinal)
Assert-True ($readerGate -ge 0 -and $gameInstall -gt $readerGate) `
    'Game installation is not ordered after the reader validation gate.'
Assert-True ($bridge.Contains('source_runtime_abi_match')) `
    'Runtime controller-information ABI evidence is missing.'
Assert-True ($bridge.Contains('POORDS4_GAME_BRIDGE_FW_0860')) `
    'Firmware 8.60 exact-manifest support is missing.'
Assert-True ($bridge.Contains('source-user-index-inactive')) `
    'Multi-slot game routing lacks the source user/index fallback.'
Assert-True ($bridge.Contains(
    'user_id == source_user_id &&')) `
    'Multi-slot routing does not bind the candidate to the DS4 user.'
Assert-True ($bridge.Contains(
    'ds4_count == 0u && identity_count == 1u &&')) `
    'Ambiguous duplicate user/index identities do not fail closed.'
Assert-True ($bridge.Contains(
    'active_count == 1u && identity_count == 1u')) `
    'Sole active fallback is not bound to the source controller identity.'
Assert-True ($bridge.Contains('poords4_rc=%d\nreport_schema=5')) `
    'Firmware reports do not identify their RC and schema.'
Assert-True ($bridge.Contains('source_library_match')) `
    'Same-firmware source/game libScePad comparison is missing.'
Assert-True (-not $bridge.Contains('game_pad_bridge_receiver_stub')) `
    'Retired injected game receiver is present.'
Assert-True (-not $bridge.Contains('wireless_ds4_game_bridge_run(')) `
    'Retired game-ptrace installer is present.'

$installerStart = $bridge.IndexOf(
    'wireless_ds4_game_bridge_run_passive(',
    [StringComparison]::Ordinal)
$installerEnd = $bridge.IndexOf(
    'wireless_ds4_game_bridge_install(', $installerStart,
    [StringComparison]::Ordinal)
Assert-True ($installerStart -ge 0 -and $installerEnd -gt $installerStart) `
    'Could not isolate the active game installer.'
$installer = $bridge.Substring(
    $installerStart, $installerEnd - $installerStart)
Assert-True (-not $installer.Contains('sys_ptrace(')) `
    'Active game installer contains ptrace.'
Assert-True (-not $installer.Contains('pt_call(')) `
    'Active game installer contains a borrowed game call.'
Assert-True (-not $installer.Contains('PT_ATTACH')) `
    'Active game installer contains a game attach operation.'

$tracked = & git -C $repo ls-files
if ($LASTEXITCODE -ne 0) {
    throw 'git ls-files failed.'
}
$expectedTracked = @(
    '.gitattributes',
    '.gitignore',
    'CHANGELOG.md',
    'LICENSE',
    'NOTICE.md',
    'README.md',
    'docs/ARCHITECTURE.md',
    'docs/FIRMWARE_SUPPORT.md',
    'payload/Makefile',
    'payload/game_pad_bridge_main.c',
    'payload/game_pad_bridge_status_main.c',
    'payload/pad_types.h',
    'payload/stop_game_pad_bridge_main.c',
    'payload/wireless_ds4.c',
    'payload/wireless_ds4.h',
    'tests/static-audit.ps1'
)
Assert-True ($tracked.Count -eq $expectedTracked.Count) `
    'Tracked file count does not match the focused public tree.'
foreach ($path in $expectedTracked) {
    Assert-True ($tracked -contains $path) `
        "Required public-tree file is missing: $path"
}

Write-Output 'PoorDS4 static audit passed.'
