Invoke-WebRequest http://192.168.1.102/api/messages -Method Post -Body @{
  name = '11W6AM'
  text = '11Acknowledged.'
  priority = 'green'
  c_time = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}
