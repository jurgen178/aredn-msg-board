# Replace 192.168.1.102 with the current IP address of the message-board device.
Invoke-WebRequest http://192.168.1.102/api/messages -Method Post -Body @{
  name = 'W6AM'
  text = 'Acknowledged.'
  priority = 'green'
  c_time = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}
