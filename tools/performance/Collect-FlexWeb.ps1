param([string]$ProcessName = 'wfweb', [int]$DurationSeconds = 600, [string]$OutputPath = 'flexweb-process.csv')
$ErrorActionPreference = 'Stop'
if ($DurationSeconds -lt 2) { throw 'Duration must be at least two seconds' }
$clock = [Diagnostics.Stopwatch]::StartNew()
$previous = @{}
while ($clock.Elapsed.TotalSeconds -lt $DurationSeconds) {
    $now = $clock.Elapsed.TotalSeconds
    foreach ($process in @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)) {
        $idKey = [string]$process.Id
        $cpu = $process.TotalProcessorTime.TotalSeconds
        $pct = $null
        if ($previous.ContainsKey($idKey)) {
            $delta = $now - $previous[$idKey].Time
            if ($delta -gt 0) { $pct = 100 * ($cpu - $previous[$idKey].Cpu) / $delta }
        }
        [pscustomobject]@{Timestamp=[DateTimeOffset]::Now.ToString('o'); PID=$process.Id;
            CpuOneCorePercent=$pct; WorkingSetBytes=$process.WorkingSet64;
            PrivateBytes=$process.PrivateMemorySize64; Threads=$process.Threads.Count} |
            Export-Csv -Path $OutputPath -Append -NoTypeInformation
        $previous[$idKey] = @{Time=$now;Cpu=$cpu}
    }
    Start-Sleep -Seconds 2
}
