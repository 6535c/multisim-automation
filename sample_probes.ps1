param([Parameter(Mandatory=$true)][string]$RequestFile)
$ErrorActionPreference='Stop'
if ([IntPtr]::Size -ne 4) {
    & "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -RequestFile $RequestFile
    exit $LASTEXITCODE
}
$request=[IO.File]::ReadAllText($RequestFile,[Text.Encoding]::UTF8) | ConvertFrom-Json
$destination=[IO.Path]::GetFullPath([string]$request.output_dir)
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$scratch=Join-Path $env:TEMP ('multisim-probes-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$copy=Join-Path $scratch 'design.ms10'
Copy-Item -LiteralPath ([string]$request.source) -Destination $copy
$rate=[double]$request.sample_rate
$points=[int]$request.points
$duration=[double]$request.duration
$timeout=[double]$request.timeout
if ($rate -le 0 -or $points -lt 1 -or $points -gt 1000000 -or $duration -le 0 -or $timeout -le 0) { throw 'Invalid probe sampling request.' }
$status=[ordered]@{process_bits=32;completed=$false;run_id=[string]$request.run_id;source=[string]$request.source;scratch=$scratch;channels=@();owned_pids=@()}
$app=$null
$circuit=$null
$owned=@()
try {
    $app=New-Object -ComObject MultisimInterface.MultisimApp
    $app.LogFile=Join-Path $scratch 'native.log'
    $app.Connect()
    $owned=@(Get-CimInstance Win32_Process -Filter "Name='multisim.exe' AND ParentProcessId=$PID" | Select-Object -ExpandProperty ProcessId)
    $status.owned_pids=$owned
    $circuit=$app.OpenFile($copy)
    $status.available_outputs=@($circuit.EnumOutputs(0) | Where-Object {$null -ne $_})
    # EnumOutputs rebuilds native circuit state in this version. Never call it
    # again after SetOutputRequest/RunSimulation: it clears queued sample data.
    $outputs=if ($request.outputs -and @($request.outputs).Count) { @($request.outputs) } else { @($status.available_outputs) }
    if (-not $outputs.Count) { throw 'The native circuit exposes no probe outputs.' }
    foreach ($output in $outputs) {
        if ($status.available_outputs -notcontains [string]$output) { throw "Unknown native output: $output" }
        $status.stage='set_output_request: '+[string]$output
        $circuit.SetOutputRequest([string]$output,1,$rate,$points,$false)
    }
    $status.stage='run_simulation'
    $circuit.RunSimulation($duration,$false)
    $status.run_error=[string]$circuit.LastErrorMessage
    $timer=[Diagnostics.Stopwatch]::StartNew()
    do {
        Start-Sleep -Milliseconds 100
        if ($timer.Elapsed.TotalSeconds -gt $timeout) {
            $circuit.StopSimulation()
            throw 'Native probe sampling timed out.'
        }
    } while ([int]$circuit.SimulationState -ne 0)
    $status.simulation_error=[string]$circuit.LastErrorMessage
    $status.final_simulation_state=[int]$circuit.SimulationState
    foreach ($output in $outputs) {
      try {
        $status.stage='get_output_data: '+[string]$output
        $readyTimer=[Diagnostics.Stopwatch]::StartNew()
        do {
            $ready=[bool]$circuit.GetType().InvokeMember('OutputReady',[Reflection.BindingFlags]::GetProperty,$null,$circuit,@([string]$output))
            if ($ready) { break }
            if ($readyTimer.Elapsed.TotalSeconds -gt $timeout) { throw "Native output never became ready: $output" }
            Start-Sleep -Milliseconds 100
        } while ($true)
        $data=$null
        $interpolation=0
        $circuit.GetOutputData([string]$output,[ref]$data,[ref]$interpolation)
        if ($null -eq $data -or $data.Rank -ne 2 -or $data.GetLength(0) -lt 2) { throw "No native sample array for $output" }
        $count=$data.GetLength(1)
        if ($count -ne $points) { throw "Expected $points native samples for $output, received $count" }
        $row=$data.GetLowerBound(0)
        $column=$data.GetLowerBound(1)
        $times=New-Object 'System.Collections.Generic.List[double]'
        $values=New-Object 'System.Collections.Generic.List[double]'
        for ($i=0;$i -lt $count;$i++) {
            $time=[double]$data[$row,($column+$i)]
            $value=[double]$data[($row+1),($column+$i)]
            if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or [double]::IsNaN($time) -or [double]::IsInfinity($time)) { throw "Nonfinite native data for $output" }
            $times.Add($time)
            $values.Add($value)
        }
        $status.channels+=@([ordered]@{name=[string]$output;shape=@($data.GetLength(0),$count);interpolation=$interpolation;time=@($times);values=@($values)})
      } catch {
        $status.channels+=@([ordered]@{name=[string]$output;error=$_.Exception.Message;native_error=[string]$circuit.LastErrorMessage})
      }
    }
    if (@($status.channels | Where-Object {$_.error}).Count) { throw 'One or more native probe channels failed; see channels.' }
    $status.completed=$true
} catch {
    $status.error=$_.Exception.Message
    $status.error_position=$_.InvocationInfo.PositionMessage
    if ($null -ne $circuit) { try { $status.circuit_error=[string]$circuit.LastErrorMessage } catch {} }
    if ($null -ne $circuit -and $request.diagnostics) {
        try {
            $commands=Join-Path $scratch 'sampling-inspect.spice'
            $raw=Join-Path $scratch 'sampling.raw'
            @"
listing
set filetype=ascii
write $raw
"@ | Set-Content -LiteralPath $commands -Encoding ASCII
            $circuit.DoCommandLine($commands,(Join-Path $scratch 'simulation-listing.log'))
            Start-Sleep -Milliseconds 1000
        } catch { $status.diagnostic_error=$_.Exception.Message }
    }
} finally {
    if ($null -ne $circuit) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($circuit) }
    if ($null -ne $app) {
        try {$app.Disconnect()} catch {}
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($app)
    }
    foreach ($processId in $owned) {
        $live=Get-CimInstance Win32_Process -Filter "ProcessId=$processId AND ParentProcessId=$PID" -ErrorAction SilentlyContinue
        if ($null -ne $live -and $live.Name -eq 'multisim.exe') { Stop-Process -Id $processId -ErrorAction SilentlyContinue }
    }
    $status | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $destination 'probe-sampling.json') -Encoding UTF8
}
if (-not $status.completed) { exit 1 }
