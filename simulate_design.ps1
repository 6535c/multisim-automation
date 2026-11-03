param(
    [Parameter(Mandatory=$true)][string]$InputFile,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [double]$Duration = 0.001,
    [ValidateSet('transient','op','ac')][string]$Analysis = 'op',
    [Parameter(Mandatory=$true)][string]$NetlistFile,
    [double]$StartFrequency = 1,
    [double]$StopFrequency = 1000000,
    [int]$PointsPerDecade = 30,
    [int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 4) {
    & "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -InputFile $InputFile -OutputDirectory $OutputDirectory -Duration $Duration -Analysis $Analysis -NetlistFile $NetlistFile -StartFrequency $StartFrequency -StopFrequency $StopFrequency -PointsPerDecade $PointsPerDecade -TimeoutSeconds $TimeoutSeconds
    exit $LASTEXITCODE
}
$source = (Resolve-Path -LiteralPath $InputFile).Path
$destination = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$scratch = Join-Path $env:TEMP ('multisim-solve-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$copy = Join-Path $scratch 'design.ms10'
Copy-Item -LiteralPath $source -Destination $copy
$app = $null
$circuit = $null
$status = [ordered]@{file=$source; process_bits=32; analysis=$Analysis; completed=$false}
try {
    $app = New-Object -ComObject MultisimInterface.MultisimApp
    $app.LogFile = Join-Path $scratch 'native.log'
    $app.Connect()
    # A fresh circuit isolates Nutmeg analysis from the live interactive
    # design's callbacks. The input is the editor's native exported netlist.
    $circuit = if ($NetlistFile) { $app.NewFile() } else { $app.OpenFile($copy) }
    $status.outputs = @($circuit.EnumOutputs(0) | Where-Object {$null -ne $_})
    $status.inputs = @($circuit.EnumInputs(0) | Where-Object {$null -ne $_})
    if (-not $NetlistFile) { $circuit.RunSimulation($Duration, $false) }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ([int]$circuit.SimulationState -ne 0) {
        Start-Sleep -Milliseconds 100
        if ($timer.Elapsed.TotalSeconds -gt $TimeoutSeconds) {
            $circuit.StopSimulation()
            throw 'Native circuit simulation timed out.'
        }
    }
    $raw = Join-Path $scratch ($Analysis + '.raw')
    $command = Join-Path $scratch 'commands.spice'
    $analysisCommand = if ($Analysis -eq 'op') { 'op' } else { '' }
    $sourceCommand = ''
    if ($NetlistFile) {
        $netlistPath = Join-Path $scratch 'circuit.cir'
        $netlist = Get-Content -LiteralPath $NetlistFile -Raw
        if ($netlist -match '[^\x00-\x7F]') { throw 'SPICE analysis currently requires ASCII identifiers in the exported netlist.' }
        ($netlist + "`r`n.end`r`n") | Set-Content -LiteralPath $netlistPath -Encoding ASCII
        $sourceCommand = "source $netlistPath"
        if ($Analysis -eq 'transient') {
            $step = ($Duration / 100).ToString('G17', [Globalization.CultureInfo]::InvariantCulture)
            $stop = $Duration.ToString('G17', [Globalization.CultureInfo]::InvariantCulture)
            $analysisCommand = "tran $step $stop"
        } elseif ($Analysis -eq 'ac') {
            $start = $StartFrequency.ToString('G17', [Globalization.CultureInfo]::InvariantCulture)
            $stop = $StopFrequency.ToString('G17', [Globalization.CultureInfo]::InvariantCulture)
            $analysisCommand = "ac dec $PointsPerDecade $start $stop"
        }
    }
    @"
$sourceCommand
set filetype=ascii
listing
show all
showmod all
$analysisCommand
write $raw
"@ | Set-Content -LiteralPath $command -Encoding ASCII
    $circuit.DoCommandLine($command, (Join-Path $scratch 'simulation.log'))
    $timer.Restart()
    while ([int]$circuit.SimulationState -ne 0 -or -not (Test-Path -LiteralPath $raw)) {
        Start-Sleep -Milliseconds 100
        if ($timer.Elapsed.TotalSeconds -gt $TimeoutSeconds) {
            $circuit.StopSimulation()
            throw 'Native OP export timed out.'
        }
    }
    # Some command-line analyses keep SimulationState at zero while the native
    # worker is still executing; wait for the actual artifact, then its writer.
    $priorLength = -1
    do {
        $length = (Get-Item -LiteralPath $raw).Length
        if ($length -gt 0 -and $length -eq $priorLength) { break }
        $priorLength = $length
        Start-Sleep -Milliseconds 200
        if ($timer.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw 'Native result writer timed out.' }
    } while ($true)
    if (-not (Test-Path -LiteralPath $raw)) { throw 'The native solver did not produce a raw result.' }
    $rawHeader = Get-Content -LiteralPath $raw -TotalCount 8
    $expectedPlot = if ($Analysis -eq 'op') { '^Plotname: (DC )?Operating Point' } elseif ($Analysis -eq 'ac') { '^Plotname: AC Analysis' } else { '^Plotname: Transient Analysis' }
    if (-not ($rawHeader -match $expectedPlot)) { throw 'The solver output does not match the requested analysis; inspect simulation.log.' }
    $solverLog = Get-Content -LiteralPath (Join-Path $scratch 'simulation.log') -Raw
    if ($solverLog -match '(?im)aborted|out of memory|^\s*Error:') { throw 'The native solver reported an analysis error; inspect simulation.log.' }
    $status.completed = $true
} catch {
    $status.error = $_.Exception.Message
    throw
} finally {
    if ($null -ne $circuit) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($circuit) }
    if ($null -ne $app) {
        $app.Disconnect()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($app)
    }
    Get-ChildItem -LiteralPath $scratch -File | Where-Object {$_.Extension -in @('.raw','.log','.spice','.cir','.net')} | ForEach-Object {
        $artifact = $_
        $copied = $false
        for ($attempt = 0; $attempt -lt 10 -and -not $copied; $attempt++) {
            try {
                Copy-Item -LiteralPath $artifact.FullName -Destination (Join-Path $destination $artifact.Name) -Force
                $copied = $true
            } catch { Start-Sleep -Milliseconds 200 }
        }
        if (-not $copied) { $status[('copy_warning_' + $artifact.Name)] = 'The application still holds the artifact.' }
    }
    $status | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'simulation.json') -Encoding UTF8
}
$status | ConvertTo-Json -Depth 5
