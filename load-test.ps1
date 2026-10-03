$ErrorActionPreference = 'Stop'

# Edit these values only when the test setup itself should change.
$BaseUri = 'http://192.168.1.110'
$MaxClients = 300
$StageSeconds = 30
$ConnectTimeoutSeconds = 15
$StartupDelayMs = 100
$MessageIntervalSeconds = 10

$sseWorker = {
  param($Uri, $ClientId, $Seconds, $ConnectTimeout)

  Add-Type -AssemblyName System.Net.Http
  $client = [System.Net.Http.HttpClient]::new()
  $client.Timeout = [TimeSpan]::FromMilliseconds(-1)
  $connectSource = [System.Threading.CancellationTokenSource]::new()
  $connectSource.CancelAfter([TimeSpan]::FromSeconds($ConnectTimeout))
  $source = [System.Threading.CancellationTokenSource]::new()
  $source.CancelAfter([TimeSpan]::FromSeconds($Seconds))
  $request = $null
  $response = $null
  $reader = $null
  $connected = $false
  $events = 0
  $errorText = $null

  try {
    $request = [System.Net.Http.HttpRequestMessage]::new(
      [System.Net.Http.HttpMethod]::Get,
      "$Uri/api/events")
    $request.Headers.Accept.Add(
      [System.Net.Http.Headers.MediaTypeWithQualityHeaderValue]::new('text/event-stream'))
    $response = $client.SendAsync(
      $request,
      [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead,
      $connectSource.Token).GetAwaiter().GetResult()

    if (-not $response.IsSuccessStatusCode) {
      throw "HTTP $([int]$response.StatusCode)"
    }
    $connected = $true

    $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
    $reader = [System.IO.StreamReader]::new($stream)
    $readTask = $reader.ReadLineAsync()
    while (-not $source.IsCancellationRequested) {
      if (-not $readTask.Wait(1000)) {
        continue
      }
      $line = $readTask.GetAwaiter().GetResult()
      if ($null -eq $line) {
        break
      }
      if ($line.StartsWith('event:')) {
        $events++
      }
      $readTask = $reader.ReadLineAsync()
    }
  }
  catch {
    if ($connectSource.IsCancellationRequested -and -not $connected) {
      $errorText = "connection timeout after $ConnectTimeout seconds"
    } elseif (-not $source.IsCancellationRequested) {
      $errorText = $_.Exception.Message
    }
  }
  finally {
    if (-not $connected -and -not $errorText) {
      $errorText = 'connection failed'
    }
    if ($reader) { $reader.Dispose() }
    if ($response) { $response.Dispose() }
    if ($request) { $request.Dispose() }
    $connectSource.Dispose()
    $source.Dispose()
    $client.Dispose()
  }

  [pscustomobject]@{
    Type = 'SSE'
    Id = $ClientId
    Connected = $connected
    Events = $events
    Error = $errorText
  }
}

$postWorker = {
  param($Uri, $Count, $DelayMs)

  Add-Type -AssemblyName System.Net.Http
  $client = [System.Net.Http.HttpClient]::new()
  $posted = 0
  $failed = 0

  try {
    for ($index = 1; $index -le $Count; $index++) {
      $form = [System.Collections.Generic.Dictionary[string, string]]::new()
      $form['name'] = 'stress-test'
      $form['text'] = "Stress test message $index at $([DateTime]::UtcNow.ToString('o'))"
      $content = [System.Net.Http.FormUrlEncodedContent]::new($form)

      try {
        $response = $client.PostAsync("$Uri/api/messages", $content).GetAwaiter().GetResult()
        if ([int]$response.StatusCode -eq 202) {
          $posted++
        } else {
          $failed++
        }
        $response.Dispose()
      }
      catch {
        $failed++
      }
      finally {
        $content.Dispose()
      }

      if ($DelayMs -gt 0 -and $index -lt $Count) {
        Start-Sleep -Milliseconds $DelayMs
      }
    }
  }
  finally {
    $client.Dispose()
  }

  [pscustomobject]@{
    Type = 'POST'
    Posted = $posted
    Failed = $failed
  }
}

function Invoke-StressStage {
  param([int] $ClientCount)

  $jobs = [System.Collections.Generic.List[object]]::new()
  $startupSeconds = [Math]::Ceiling(($ClientCount * $StartupDelayMs) / 1000)
  $workerSeconds = $StageSeconds + $startupSeconds + 2
  $messageCount = [Math]::Floor($workerSeconds / $MessageIntervalSeconds) + 1
  $started = [DateTime]::UtcNow
  $results = @()

  try {
    for ($clientId = 1; $clientId -le $ClientCount; $clientId++) {
      $jobs.Add((Start-Job -ScriptBlock $sseWorker -ArgumentList $BaseUri, $clientId, $workerSeconds, $ConnectTimeoutSeconds))
      if ($StartupDelayMs -gt 0 -and $clientId -lt $ClientCount) {
        Start-Sleep -Milliseconds $StartupDelayMs
      }
    }
    $jobs.Add((Start-Job -ScriptBlock $postWorker -ArgumentList $BaseUri, $messageCount, ($MessageIntervalSeconds * 1000)))

    $lastCompleted = -1
    while (@($jobs | Where-Object State -notin @('Completed', 'Failed', 'Stopped')).Count -gt 0) {
      $completed = @($jobs | Where-Object State -in @('Completed', 'Failed', 'Stopped')).Count
      if ($completed -ne $lastCompleted) {
        $elapsed = [int](([DateTime]::UtcNow - $started).TotalSeconds)
        Write-Host ("  progress: {0}/{1} workers finished, {2}s" -f $completed, $jobs.Count, $elapsed)
        $lastCompleted = $completed
      }
      Start-Sleep -Milliseconds 500
    }

    $results = @($jobs | ForEach-Object { Receive-Job -Job $_ })
  }
  finally {
    foreach ($job in $jobs) {
      if ($job.State -notin @('Completed', 'Failed', 'Stopped')) {
        Stop-Job -Job $job -ErrorAction SilentlyContinue
      }
      Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
    }
  }

  $sse = @($results | Where-Object Type -eq 'SSE')
  $post = @($results | Where-Object Type -eq 'POST')
  $connected = @($sse | Where-Object Connected).Count
  $posted = [int](($post | Measure-Object -Property Posted -Sum).Sum)
  $failed = [int](($post | Measure-Object -Property Failed -Sum).Sum)

  [pscustomobject]@{
    Clients = $ClientCount
    Connected = $connected
    ConnectionFailures = $ClientCount - $connected
    Events = [int](($sse | Measure-Object -Property Events -Sum).Sum)
    Posted = $posted
    WriteFailures = $failed
    RequiredWrites = $messageCount
    DurationSeconds = [Math]::Round(([DateTime]::UtcNow - $started).TotalSeconds, 1)
  }
}

Write-Host '=== AREDN realistic browser stress test ===' -ForegroundColor Cyan
Write-Host "Target: $BaseUri"
Write-Host "Stages: 1, 2, 4 ... up to $MaxClients clients"
Write-Host "Each stage: $StageSeconds seconds, one message every $MessageIntervalSeconds seconds"
Write-Host 'Press Ctrl+C to stop; active test jobs are cleaned up.'
Write-Host ''

function Test-StressStage {
  param([int] $ClientCount)

  Write-Host "--- Testing $ClientCount browser connections ---" -ForegroundColor Yellow
  $result = Invoke-StressStage -ClientCount $ClientCount
  $result | Format-List

  [pscustomobject]@{
    Passed = $result.ConnectionFailures -eq 0 -and
      $result.Posted -ge $result.RequiredWrites -and
      $result.WriteFailures -eq 0
    Result = $result
  }
}

$lastPassing = 0
$firstFailing = 0
$stage = 1
while ($stage -le $MaxClients) {
  $stageTest = Test-StressStage -ClientCount $stage
  if (-not $stageTest.Passed) {
    $firstFailing = $stage
    break
  }
  $lastPassing = $stage

  if ($stage -eq $MaxClients) {
    break
  }
  $stage = [Math]::Min($stage * 2, $MaxClients)
}

while ($firstFailing -gt 0 -and $firstFailing - $lastPassing -gt 1) {
  $stage = [Math]::Floor(($lastPassing + $firstFailing) / 2)
  $stageTest = Test-StressStage -ClientCount $stage
  if ($stageTest.Passed) {
    $lastPassing = $stage
  } else {
    $firstFailing = $stage
  }
}

if ($firstFailing -eq 0) {
  Write-Host "Maximum confirmed capacity: $lastPassing browser connections" -ForegroundColor Green
} else {
  Write-Host "Maximum confirmed capacity: $lastPassing browser connections" -ForegroundColor Green
  Write-Host "First failed connection level: $firstFailing browser connections" -ForegroundColor Red
}