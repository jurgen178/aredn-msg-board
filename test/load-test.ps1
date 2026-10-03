$ErrorActionPreference = 'Stop'

# Edit these values only when the test setup itself should change.
$BaseUri = 'http://192.168.1.102'
$MaxClients = 300
$StageSeconds = 120
$ConnectTimeoutSeconds = 15
$StartupDelayMs = 100
$MessageIntervalSeconds = 10
$PollIntervalSeconds = 20
$PollJitterSeconds = 3

$pollWorker = {
  param($Uri, $ClientId, $Seconds, $ConnectTimeout, $PollInterval, $PollJitter)

  Add-Type -AssemblyName System.Net.Http
  $client = [System.Net.Http.HttpClient]::new()
  $client.Timeout = [TimeSpan]::FromMilliseconds(-1)
  $polls = 0
  $messages = 0
  $failures = 0
  $errorText = $null
  $afterId = 0
  $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)

  try {
    while ([DateTime]::UtcNow -lt $deadline) {
      $endpoint = if ($afterId -eq 0) {
        "$Uri/api/messages?limit=50"
      } else {
        "$Uri/api/messages?after=$afterId&limit=50"
      }
      $request = [System.Net.Http.HttpRequestMessage]::new(
        [System.Net.Http.HttpMethod]::Get, $endpoint)
      $request.Headers.ConnectionClose = $true
      $connectSource = [System.Threading.CancellationTokenSource]::new()
      $connectSource.CancelAfter([TimeSpan]::FromSeconds($ConnectTimeout))
      try {
        $response = $client.SendAsync($request, $connectSource.Token).GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) {
          throw "HTTP $([int]$response.StatusCode)"
        }
        $data = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
        $polls++
        $messages += @($data.p).Count
        if (@($data.p).Count -gt 0) {
          $afterId = [uint32](@($data.p)[0].i)
        }
      }
      catch {
        $failures++
        $errorText = $_.Exception.Message
      }
      finally {
        if ($response) { $response.Dispose(); $response = $null }
        $request.Dispose()
        $connectSource.Dispose()
      }
      $jitterMs = Get-Random -Minimum (-$PollJitter * 1000) -Maximum (($PollJitter * 1000) + 1)
      $delayMs = [Math]::Max(1000, ($PollInterval * 1000) + $jitterMs)
      Start-Sleep -Milliseconds $delayMs
    }
  }
  finally {
    $client.Dispose()
  }

  [pscustomobject]@{
    Type = 'POLL'
    Id = $ClientId
    Polls = $polls
    Messages = $messages
    Failures = $failures
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
      $jobs.Add((Start-Job -ScriptBlock $pollWorker -ArgumentList $BaseUri, $clientId,
        $workerSeconds, $ConnectTimeoutSeconds, $PollIntervalSeconds, $PollJitterSeconds))
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

  $poll = @($results | Where-Object Type -eq 'POLL')
  $post = @($results | Where-Object Type -eq 'POST')
  $polls = [int](($poll | Measure-Object -Property Polls -Sum).Sum)
  $pollFailures = [int](($poll | Measure-Object -Property Failures -Sum).Sum)
  $posted = [int](($post | Measure-Object -Property Posted -Sum).Sum)
  $failed = [int](($post | Measure-Object -Property Failed -Sum).Sum)

  [pscustomobject]@{
    Clients = $ClientCount
    Polls = $polls
    PollFailures = $pollFailures
    MessagesReceived = [int](($poll | Measure-Object -Property Messages -Sum).Sum)
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
    Passed = $result.PollFailures -eq 0 -and
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
  Write-Host "Maximum confirmed capacity: $lastPassing polling clients" -ForegroundColor Green
} else {
  Write-Host "Maximum confirmed capacity: $lastPassing polling clients" -ForegroundColor Green
  Write-Host "First failed polling level: $firstFailing clients" -ForegroundColor Red
}