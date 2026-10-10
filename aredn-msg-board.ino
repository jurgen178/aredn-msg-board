#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <sys/time.h>
#include <esp_system.h>
#include "BoardPage.h"
#include "FfatMessageBoard.h"
#include "WifiSetupPage.h"

namespace
{
// Time bounds reject uninitialized, implausible, or far-future clock values.
constexpr char WIFI_AP_SSID[] = "AREDN-Message-Board-Setup";
constexpr char WIFI_AP_PASSWORD[] = "arednsetup";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t WIFI_RECOVERY_DELAY_MS = 5000;
constexpr int64_t MIN_VALID_EPOCH = 1577836800LL;
constexpr int64_t CLIENT_TIME_BOOTSTRAP_EPOCH = 1700000000LL;
constexpr int64_t MAX_VALID_EPOCH = 4102444800LL;
constexpr uint8_t TIME_SOURCE_UNSET = 0;
constexpr uint8_t TIME_SOURCE_NETWORK = 1;
constexpr uint8_t TIME_SOURCE_MANUAL = 2;
constexpr uint8_t TIME_SOURCE_CLIENT = 3;
constexpr uint8_t CLIENT_TIME_AGREEMENT_COUNT = 3;
constexpr int64_t CLIENT_TIME_MAX_SAMPLE_SPREAD = 10;
constexpr char ADMIN_PASSWORD[] = "aredn-admin";
constexpr uint32_t SERIAL_STATUS_INTERVAL_MS = 15000;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr int8_t OLED_RESET = -1;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr uint32_t OLED_PAGE_INTERVAL_MS = 5000;

// These objects are shared by asynchronous HTTP callbacks and the main Arduino loop.
AsyncWebServer server(80);
Preferences wifiPreferences;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
FfatMessageBoard messageBoard;
// Atomic fields are read or updated from both the server task and the main loop.
std::atomic<uint8_t> deviceTimeSource{TIME_SOURCE_UNSET};
std::atomic<uint32_t> adminSessionToken{0};
std::atomic<uint32_t> boardRevision{1};
std::atomic<uint32_t> fullRefreshRevision{0};
std::atomic<bool> importUploadActive{false};
std::atomic<bool> wifiCredentialsSaved{false};
std::atomic<bool> wifiConfigRequestPending{false};
std::atomic<uint8_t> wifiProvisionState{0};
std::atomic<bool> wifiScanActive{false};
// Client samples are protected because HTTP handlers may run concurrently.
SemaphoreHandle_t clientTimeMutex = nullptr;
SemaphoreHandle_t wifiConfigMutex = nullptr;
SemaphoreHandle_t wifiScanMutex = nullptr;
char requestedWifiSsid[33]{};
char requestedWifiPassword[65]{};
char activeWifiSsid[33]{};
char activeWifiPassword[65]{};
String savedWifiSsid;
String savedWifiPassword;
int64_t clientTimeSamples[CLIENT_TIME_AGREEMENT_COUNT]{};
uint32_t clientTimeSampleClients[CLIENT_TIME_AGREEMENT_COUNT]{};
uint8_t clientTimeSampleCount = 0;
uint32_t lastReconnectAttempt = 0;
uint32_t wifiConnectAttemptStart = 0;
uint32_t wifiRecoveryStart = 0;
bool wifiConnectAttemptActive = false;
bool wifiSavingCandidate = false;
bool wifiCandidateSawDisconnect = false;
bool wifiRecoveryPending = false;
bool wifiPreferencesAvailable = false;
bool wasConnected = false;
uint32_t lastSerialStatus = 0;
uint32_t lastDisplayUpdate = 0;
uint32_t displayedBoardRevision = 0;
uint8_t displayedPage = 0;
bool displayAvailable = false;
bool latestMessageAvailable = false;
FfatMessageBoard::Message latestMessage{};

enum WifiProvisionState : uint8_t
{
  WIFI_SETUP_REQUIRED,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_FAILED
};

struct StoredWifiCredentials
{
  char ssid[33];
  char password[65];
};

struct ImportUploadState
{
  // The upload callback records failure until the final request handler sends its response.
  bool failed = false;
  bool started = false;
};

void drawDisplayLine(uint8_t row, const char* label, const char* value)
{
  display.setCursor(0, row * 8);
  display.print(label);
  display.print(value);
}

void printDisplayUtf8(const char* value)
{
  // The OLED font cannot render multibyte UTF-8; replace each complete code point with one '?'.
  const uint8_t* current = reinterpret_cast<const uint8_t*>(value);
  while (*current != '\0')
  {
    if (*current < 0x80)
    {
      display.write(*current++);
      continue;
    }

    uint8_t sequenceLength = 0;
    if (*current >= 0xC2 && *current <= 0xDF)
    {
      sequenceLength = 2;
    }
    else if (*current >= 0xE0 && *current <= 0xEF)
    {
      sequenceLength = 3;
    }
    else if (*current >= 0xF0 && *current <= 0xF4)
    {
      sequenceLength = 4;
    }

    bool validSequence = sequenceLength != 0;
    for (uint8_t index = 1; validSequence && index < sequenceLength; ++index)
    {
      validSequence = (current[index] & 0xC0) == 0x80;
    }
    display.write('?');
    current += validSequence ? sequenceLength : 1;
  }
}

void printDisplayUtf8Ellipsis(const char* value, uint8_t maxCharacters)
{
  // Count UTF-8 code points rather than bytes so truncation never splits a character.
  const uint8_t* current = reinterpret_cast<const uint8_t*>(value);
  uint8_t characterCount = 0;
  while (*current != '\0')
  {
    uint8_t sequenceLength = 1;
    if (*current >= 0xC2 && *current <= 0xDF)
    {
      sequenceLength = 2;
    }
    else if (*current >= 0xE0 && *current <= 0xEF)
    {
      sequenceLength = 3;
    }
    else if (*current >= 0xF0 && *current <= 0xF4)
    {
      sequenceLength = 4;
    }
    current += sequenceLength;
    ++characterCount;
  }

  if (characterCount <= maxCharacters)
  {
    printDisplayUtf8(value);
    return;
  }

  const uint8_t visibleCharacters = maxCharacters > 3 ? maxCharacters - 3 : 0;
  current = reinterpret_cast<const uint8_t*>(value);
  for (uint8_t index = 0; index < visibleCharacters && *current != '\0'; ++index)
  {
    if (*current < 0x80)
    {
      display.write(*current++);
      continue;
    }

    uint8_t sequenceLength = 1;
    if (*current >= 0xC2 && *current <= 0xDF)
    {
      sequenceLength = 2;
    }
    else if (*current >= 0xE0 && *current <= 0xEF)
    {
      sequenceLength = 3;
    }
    else if (*current >= 0xF0 && *current <= 0xF4)
    {
      sequenceLength = 4;
    }
    display.write('?');
    current += sequenceLength;
  }
  display.print("...");
}

void formatSerialClock(char* output, size_t outputSize);
const char* messagePriorityName(uint8_t priority);

void refreshLatestDisplayMessage()
{
  // Cache the last post so display refreshes do not reread FFat on every page update.
  latestMessageAvailable = messageBoard.readLatest(latestMessage);
}

void drawBoardDisplay()
{
  if (!displayAvailable)
  {
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  if (!wifiCredentialsSaved.load(std::memory_order_acquire))
  {
    display.println("WiFi setup required");
    drawDisplayLine(1, "AP: ", WIFI_AP_SSID);
    drawDisplayLine(2, "Password: ", WIFI_AP_PASSWORD);
    display.setCursor(0, 32);
    display.println("Open in browser:");
    drawDisplayLine(5, "", WiFi.softAPIP().toString().c_str());
    display.println("/wifi");
    display.println("Waiting for WiFi");
    display.display();
    return;
  }

  if (displayedPage == 0)
  {
    display.println("AREDN MESSAGE BOARD");
    drawDisplayLine(1, "IP: ", WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString().c_str()
        : "DISCONNECTED");
    char metrics[22];
    formatSerialClock(metrics, sizeof(metrics));
    drawDisplayLine(2, "Time: ", metrics);
    snprintf(metrics, sizeof(metrics), "%lu", static_cast<unsigned long>(messageBoard.messageCount()));
    drawDisplayLine(3, "Messages: ", metrics);
    snprintf(metrics, sizeof(metrics), "%d dBm",
             WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    drawDisplayLine(4, "WiFi: ", metrics);
    snprintf(metrics, sizeof(metrics), "%lu KB",
             static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
    drawDisplayLine(5, "Heap: ", metrics);
    const uint32_t storageTotal = messageBoard.totalBytes();
    const uint32_t storagePercent = storageTotal == 0
      ? 0
      : static_cast<uint32_t>(static_cast<uint64_t>(messageBoard.usedBytes()) * 100 / storageTotal);
    snprintf(metrics, sizeof(metrics), "%lu%%",
         static_cast<unsigned long>(storagePercent));
    drawDisplayLine(6, "Storage: ", metrics);
    const uint32_t uptimeSeconds = millis() / 1000;
        snprintf(metrics, sizeof(metrics), "%lud %02luh %02lum",
         static_cast<unsigned long>(uptimeSeconds / 86400),
         static_cast<unsigned long>((uptimeSeconds / 3600) % 24),
          static_cast<unsigned long>((uptimeSeconds / 60) % 60));
        drawDisplayLine(7, "Uptime: ", metrics);
  }
  else
  {
    // The second page shows the newest cached message; the author is truncated to fit the OLED.
    if (!latestMessageAvailable)
    {
      display.println("NO MESSAGES");
      display.setCursor(0, 24);
      display.println("Message board");
      display.setCursor(0, 32);
      display.println("is empty.");
    }
    else
    {
      display.print("LAST MESSAGE");
      if (latestMessage.priority != static_cast<uint8_t>(FfatMessageBoard::Priority::Green))
      {
        display.print(" [");
        display.print(messagePriorityName(latestMessage.priority));
        display.print("]");
      }
      display.println();
      display.setCursor(0, 16);
      printDisplayUtf8Ellipsis(latestMessage.name, 21);
      display.println();
      display.setCursor(0, 28);
      printDisplayUtf8(latestMessage.text);
      display.println();
    }
  }

  display.display();
}

void initializeDisplay()
{
  // Display failure is non-fatal: the network service can operate without the OLED.
  Wire.begin();
  Serial.println("I2C initialized");

  Serial.println("Scanning I2C bus...");
  Wire.beginTransmission(OLED_ADDRESS);
  const uint8_t error = Wire.endTransmission();

  if (error != 0)
  {
    displayAvailable = false;
    Serial.printf("No I2C device at address 0x%02X (error: %u)\n", OLED_ADDRESS, error);
    Serial.println("Service continues without display.");
    return;
  }

  Serial.printf("I2C device found at address 0x%02X\n", OLED_ADDRESS);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
  {
    displayAvailable = false;
    Serial.println("Display found via I2C but initialization failed.");
    Serial.println("Service continues without display.");
    return;
  }

  displayAvailable = true;
  Serial.println("Display successfully initialized");
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("AREDN MESSAGE BOARD");
  display.println();
  display.println("STARTING SERVICE ...");
  display.display();
}

char foldSearchCharacter(char value)
{
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool containsSearchTerm(const char* value, const String& searchTerm)
{
  // Search is deliberately ASCII case-insensitive; stored UTF-8 bytes otherwise match exactly.
  if (searchTerm.isEmpty())
  {
    return true;
  }
  for (size_t start = 0; value[start] != '\0'; ++start)
  {
    size_t offset = 0;
    while (value[start + offset] != '\0' && offset < searchTerm.length() &&
           foldSearchCharacter(value[start + offset]) == foldSearchCharacter(searchTerm[offset]))
    {
      ++offset;
    }
    if (offset == searchTerm.length())
    {
      return true;
    }
  }
  return false;
}

bool isDeviceTimeValid()
{
  // The platform clock's boot-time default is earlier than this accepted epoch.
  return static_cast<int64_t>(time(nullptr)) >= MIN_VALID_EPOCH;
}

const char* deviceTimeSourceName()
{
  // A stored source is meaningful only while the clock itself passes validation.
  if (!isDeviceTimeValid())
  {
    return "unset";
  }

  switch (deviceTimeSource.load(std::memory_order_acquire))
  {
    case TIME_SOURCE_MANUAL: return "manual";
    case TIME_SOURCE_CLIENT: return "client";
    default: return "unset";
  }
}

void resetClientTimeSamples()
{
  // Discard stale votes whenever a new clock value is accepted.
  clientTimeSampleCount = 0;
}

bool addClientTimeSample(int64_t epoch, uint32_t clientAddress)
{
  // Do not let one client satisfy the multi-client agreement threshold repeatedly.
  for (uint8_t index = 0; index < clientTimeSampleCount; ++index)
  {
    if (clientTimeSampleClients[index] == clientAddress)
    {
      return false;
    }
  }

  if (clientTimeSampleCount < CLIENT_TIME_AGREEMENT_COUNT)
  {
    clientTimeSamples[clientTimeSampleCount] = epoch;
    clientTimeSampleClients[clientTimeSampleCount++] = clientAddress;
  }
  else
  {
    for (uint8_t index = 1; index < CLIENT_TIME_AGREEMENT_COUNT; ++index)
    {
      clientTimeSamples[index - 1] = clientTimeSamples[index];
      clientTimeSampleClients[index - 1] = clientTimeSampleClients[index];
    }
    clientTimeSamples[CLIENT_TIME_AGREEMENT_COUNT - 1] = epoch;
    clientTimeSampleClients[CLIENT_TIME_AGREEMENT_COUNT - 1] = clientAddress;
  }
  return true;
}

void synchronizeFromClient(int64_t clientEpoch, uint32_t clientAddress)
{
  // Client time is accepted immediately only for an unset clock or a small correction.
  if (clientTimeMutex == nullptr || clientEpoch < CLIENT_TIME_BOOTSTRAP_EPOCH ||
      clientEpoch > MAX_VALID_EPOCH ||
      xSemaphoreTake(clientTimeMutex, portMAX_DELAY) != pdTRUE)
  {
    return;
  }

  const bool manualTime = deviceTimeSource.load(std::memory_order_acquire) == TIME_SOURCE_MANUAL;
  const int64_t deviceEpoch = static_cast<int64_t>(time(nullptr));
  const struct timeval clientTime = {static_cast<time_t>(clientEpoch), 0};
  if (deviceEpoch < CLIENT_TIME_BOOTSTRAP_EPOCH ||
      deviceTimeSource.load(std::memory_order_acquire) == TIME_SOURCE_UNSET)
  {
    // With no trustworthy clock yet, the first plausible browser timestamp bootstraps it.
    if (settimeofday(&clientTime, nullptr) == 0)
    {
      deviceTimeSource.store(TIME_SOURCE_CLIENT, std::memory_order_release);
      resetClientTimeSamples();
    }
    xSemaphoreGive(clientTimeMutex);
    return;
  }

  const int64_t difference = clientEpoch > deviceEpoch
      ? clientEpoch - deviceEpoch
      : deviceEpoch - clientEpoch;
  if (!manualTime && difference >= 1 && difference <= CLIENT_TIME_MAX_SAMPLE_SPREAD)
  {
    if (settimeofday(&clientTime, nullptr) == 0)
    {
      deviceTimeSource.store(TIME_SOURCE_CLIENT, std::memory_order_release);
      resetClientTimeSamples();
    }
    xSemaphoreGive(clientTimeMutex);
    return;
  }

  if (manualTime || difference > CLIENT_TIME_MAX_SAMPLE_SPREAD)
  {
    // A large correction, or any correction after manual setup, needs distinct-client agreement.
    if (!addClientTimeSample(clientEpoch, clientAddress))
    {
      xSemaphoreGive(clientTimeMutex);
      return;
    }
    if (clientTimeSampleCount == CLIENT_TIME_AGREEMENT_COUNT)
    {
      int64_t lowest = clientTimeSamples[0];
      int64_t highest = clientTimeSamples[0];
      for (uint8_t index = 1; index < CLIENT_TIME_AGREEMENT_COUNT; ++index)
      {
        lowest = min(lowest, clientTimeSamples[index]);
        highest = max(highest, clientTimeSamples[index]);
      }
      if (highest - lowest <= CLIENT_TIME_MAX_SAMPLE_SPREAD)
      {
        // The median limits the effect of a single outlying client timestamp.
        int64_t median = clientTimeSamples[0];
        for (uint8_t index = 0; index < CLIENT_TIME_AGREEMENT_COUNT; ++index)
        {
          uint8_t valuesBelow = 0;
          for (uint8_t candidate = 0; candidate < CLIENT_TIME_AGREEMENT_COUNT; ++candidate)
          {
            valuesBelow += clientTimeSamples[candidate] < clientTimeSamples[index] ? 1 : 0;
          }
          if (valuesBelow == CLIENT_TIME_AGREEMENT_COUNT / 2)
          {
            median = clientTimeSamples[index];
            break;
          }
        }
        const struct timeval consensusTime = {static_cast<time_t>(median), 0};
        if (settimeofday(&consensusTime, nullptr) == 0)
        {
          deviceTimeSource.store(TIME_SOURCE_CLIENT, std::memory_order_release);
        }
        resetClientTimeSamples();
      }
    }
  }
  xSemaphoreGive(clientTimeMutex);
}

void appendJsonString(Print& output, const char* value)
{
  // Stream escaped JSON directly to the response instead of allocating a second copy.
  output.write('"');
  for (const unsigned char* character = reinterpret_cast<const unsigned char*>(value);
       *character != '\0';
       ++character)
  {
    switch (*character)
    {
      case '"': output.print("\\\""); break;
      case '\\': output.print("\\\\"); break;
      case '\b': output.print("\\b"); break;
      case '\f': output.print("\\f"); break;
      case '\n': output.print("\\n"); break;
      case '\r': output.print("\\r"); break;
      case '\t': output.print("\\t"); break;
      default:
        if (*character < 0x20)
        {
          char escaped[7];
          snprintf(escaped, sizeof(escaped), "\\u%04x", *character);
          output.print(escaped);
        }
        else
        {
          output.write(*character);
        }
        break;
    }
  }
  output.write('"');
}

void appendIsoTimestamp(Print& output, int64_t epoch)
{
  // Exports use UTC ISO-8601 timestamps, independent of the device's local timezone.
  const time_t timestamp = static_cast<time_t>(epoch);
  struct tm utcTime{};
  char formatted[21]{};
  if (gmtime_r(&timestamp, &utcTime) == nullptr ||
      strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utcTime) == 0)
  {
    appendJsonString(output, "");
    return;
  }

  appendJsonString(output, formatted);
}

const char* messagePriorityName(uint8_t priority)
{
  // Green is also the compatibility fallback for older records without a priority field.
  switch (static_cast<FfatMessageBoard::Priority>(priority))
  {
    case FfatMessageBoard::Priority::Orange: return "orange";
    case FfatMessageBoard::Priority::Red: return "red";
    default: return "green";
  }
}

bool parseMessagePriority(const String& value, FfatMessageBoard::Priority& priority)
{
  // Accept only the exact priority tokens shared by the browser and import/export formats.
  if (value == "green")
  {
    priority = FfatMessageBoard::Priority::Green;
    return true;
  }
  if (value == "orange")
  {
    priority = FfatMessageBoard::Priority::Orange;
    return true;
  }
  if (value == "red")
  {
    priority = FfatMessageBoard::Priority::Red;
    return true;
  }
  return false;
}

void formatSerialClock(char* output, size_t outputSize)
{
  if (!isDeviceTimeValid())
  {
    snprintf(output, outputSize, "unset");
    return;
  }

  const time_t timestamp = time(nullptr);
  struct tm utcTime{};
  if (gmtime_r(&timestamp, &utcTime) == nullptr)
  {
    snprintf(output, outputSize, "unset");
    return;
  }

  strftime(output, outputSize, "%H:%M:%S UTC", &utcTime);
}

bool isAdminAuthenticated(AsyncWebServerRequest* request)
{
  // Only the current in-memory token is accepted; restarting the device invalidates sessions.
  const uint32_t token = adminSessionToken.load(std::memory_order_acquire);
  if (token == 0 || !request->hasHeader("Cookie"))
  {
    return false;
  }

  const String expectedCookie = "aredn_admin=" + String(token, HEX);
  return request->getHeader("Cookie")->value().indexOf(expectedCookie) >= 0;
}

bool isSetupApClient(AsyncWebServerRequest* request)
{
  if (request == nullptr || request->client() == nullptr)
  {
    return false;
  }

  const IPAddress apAddress = WiFi.softAPIP();
  const IPAddress clientAddress = request->client()->remoteIP();
  return clientAddress[0] == apAddress[0] &&
         clientAddress[1] == apAddress[1] &&
         clientAddress[2] == apAddress[2];
}

void sendSetupApRequired(AsyncWebServerRequest* request)
{
  request->send(403, "application/json", "{\"error\":\"setup_ap_required\"}");
}

void sendAdminRequired(AsyncWebServerRequest* request)
{
  // Keep authorization failures consistent for every protected route.
  request->send(401, "application/json", "{\"error\":\"admin_required\"}");
}

void writeMessageItems(AsyncResponseStream& response,
                       File& file,
                       uint32_t& count,
                       bool pretty,
                       bool newestFirst,
                       uint32_t limit = 0,
                       uint32_t beforeId = 0,
                       uint32_t afterId = 0,
                       bool* hasMore = nullptr,
                       const String* searchTerm = nullptr,
                       int priorityFilter = -1)
{
  // The ID-sorted index lets incremental polling skip records already seen by the client.
  if (hasMore != nullptr)
  {
    *hasMore = false;
  }
  const uint32_t storedCount = messageBoard.messageCount();
  if (storedCount == 0)
  {
    return;
  }

  // Search without an ID boundary needs a full scan; ordinary polling can use the sorted index.
  const bool sequentialSearch = searchTerm != nullptr && beforeId == 0 && afterId == 0;
  const bool incrementalLatest = afterId != 0 && beforeId == 0 && searchTerm == nullptr;
  // Narrow incremental reads to the first newer ID; filtered and paged requests scan normally.
  const uint32_t firstIndexAfter = incrementalLatest
      ? messageBoard.firstIndexAfter(afterId)
      : 0;
  for (uint32_t position = 0; position < storedCount; ++position)
  {
    const uint32_t index = newestFirst ? storedCount - 1 - position : position;
    if (incrementalLatest && index < firstIndexAfter)
    {
      break;
    }
    FfatMessageBoard::Message message{};
    const bool readable = messageBoard.readAtExclusive(file, index, message);
    if (!readable ||
        (beforeId != 0 && message.id >= beforeId) ||
        (afterId != 0 && message.id <= afterId) ||
        (priorityFilter >= 0 && message.priority != static_cast<uint8_t>(priorityFilter)) ||
        (searchTerm != nullptr &&
         !containsSearchTerm(message.name, *searchTerm) &&
         !containsSearchTerm(message.text, *searchTerm)))
    {
      continue;
    }
    if ((position & 0x1f) == 0)
    {
      esp_task_wdt_reset();
      delay(0);
    }
    if (limit != 0 && count >= limit)
    {
      if (hasMore != nullptr)
      {
        *hasMore = true;
      }
      break;
    }
    if (count > 0)
    {
      response.print(pretty ? ",\n" : ",");
    }
    if (pretty)
    {
      response.printf("    {\n      \"id\": %lu,\n      \"created_at\": ",
                      static_cast<unsigned long>(message.id));
      appendIsoTimestamp(response, message.createdAtEpoch);
      response.print(",\n      \"name\": ");
      appendJsonString(response, message.name);
      response.print(",\n      \"text\": ");
      appendJsonString(response, message.text);
      if (message.priority != static_cast<uint8_t>(FfatMessageBoard::Priority::Green))
      {
        response.print(",\n      \"priority\": ");
        appendJsonString(response, messagePriorityName(message.priority));
      }
      response.print("\n    }");
    }
    else
    {
      response.printf("{\"i\":%lu,\"t\":%lld,\"u\":",
                      static_cast<unsigned long>(message.id),
                      static_cast<long long>(message.createdAtEpoch));
      appendJsonString(response, message.name);
      response.print(",\"m\":");
      appendJsonString(response, message.text);
      response.print(",\"p\":");
      appendJsonString(response, messagePriorityName(message.priority));
      response.write('}');
    }
    ++count;
  }
}

void handleSubmitMessage(AsyncWebServerRequest* request)
{
  if (request->hasParam("c_time", true))
  {
    // Message submissions also carry client time so a browser can initialize the board clock.
    const String clientTimeValue = request->getParam("c_time", true)->value();
    char* parseEnd = nullptr;
    const long long clientEpoch = strtoll(clientTimeValue.c_str(), &parseEnd, 10);
    if (parseEnd != clientTimeValue.c_str() && *parseEnd == '\0')
    {
      const uint32_t clientAddress = request->client() == nullptr
          ? 0
          : static_cast<uint32_t>(request->client()->remoteIP());
      synchronizeFromClient(static_cast<int64_t>(clientEpoch), clientAddress);
    }
  }

  if (!request->hasParam("name", true) || !request->hasParam("text", true))
  {
    request->send(400, "application/json", "{\"error\":\"missing_fields\"}");
    return;
  }

  String author = request->getParam("name", true)->value();
  String text = request->getParam("text", true)->value();
  FfatMessageBoard::Priority priority = FfatMessageBoard::Priority::Green;
  if (request->hasParam("priority", true) &&
      !parseMessagePriority(request->getParam("priority", true)->value(), priority))
  {
    request->send(400, "application/json", "{\"error\":\"invalid_priority\"}");
    return;
  }
  author.trim();
  text.trim();

  if (author.isEmpty() || text.isEmpty())
  {
    request->send(400, "application/json", "{\"error\":\"empty_fields\"}");
    return;
  }

  if (author.length() > FfatMessageBoard::MAX_NAME_LENGTH ||
      text.length() > FfatMessageBoard::MAX_MESSAGE_LENGTH)
  {
    request->send(400, "application/json", "{\"error\":\"too_long\"}");
    return;
  }

  if (!isDeviceTimeValid())
  {
    request->send(409, "application/json", "{\"error\":\"clock_unset\"}");
    return;
  }

  uint32_t messageId = 0;
  // Persist only after validation and a usable timestamp are available.
  const FfatMessageBoard::SubmitResult result = messageBoard.enqueue(
      author.c_str(), text.c_str(), static_cast<int64_t>(time(nullptr)), priority, messageId);
  if (result == FfatMessageBoard::SubmitResult::Invalid)
  {
    request->send(400, "application/json", "{\"error\":\"invalid_message\"}");
    return;
  }
  if (result == FfatMessageBoard::SubmitResult::Full)
  {
    request->send(409, "application/json", "{\"error\":\"board_full\"}");
    return;
  }
  if (result != FfatMessageBoard::SubmitResult::Accepted)
  {
    request->send(500, "application/json", "{\"error\":\"board_unavailable\"}");
    return;
  }

  boardRevision.fetch_add(1, std::memory_order_acq_rel);

  char responseBody[48];
  snprintf(responseBody, sizeof(responseBody), "{\"accepted\":true,\"id\":%lu}",
           static_cast<unsigned long>(messageId));
  request->send(202, "application/json", responseBody);
}

void registerRoutes()
{
  // Route handlers are asynchronous; long-running storage operations use the board's mutex.
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    if (!isSetupApClient(request))
    {
      sendSetupApRequired(request);
      return;
    }

    AsyncWebServerResponse* response = request->beginResponse_P(
        200, "text/html; charset=utf-8", WIFI_SETUP_PAGE);
    response->addHeader("Cache-Control", "no-cache, must-revalidate");
    request->send(response);
  });

  server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    if (!isSetupApClient(request))
    {
      sendSetupApRequired(request);
      return;
    }

    const uint8_t state = wifiProvisionState.load(std::memory_order_acquire);
    const char* stateName = "setup_required";
    if (state == WIFI_CONNECTING)
    {
      stateName = "connecting";
    }
    else if (state == WIFI_CONNECTED)
    {
      stateName = "connected";
    }
    else if (state == WIFI_FAILED)
    {
      stateName = "failed";
    }

    const bool connected = WiFi.status() == WL_CONNECTED;
    const String apAddress = WiFi.softAPIP().toString();
    const String stationAddress = connected ? WiFi.localIP().toString() : "0.0.0.0";
    char response[192];
    snprintf(response, sizeof(response),
             "{\"state\":\"%s\",\"wifi_connected\":%s,"
             "\"ap_ip\":\"%s\",\"sta_ip\":\"%s\"}",
             stateName,
             connected ? "true" : "false",
             apAddress.c_str(),
             stationAddress.c_str());
    request->send(200, "application/json", response);
  });

  server.on("/api/wifi/scan", HTTP_POST, [](AsyncWebServerRequest* request)
  {
    if (!isSetupApClient(request))
    {
      sendSetupApRequired(request);
      return;
    }
    if (wifiScanMutex == nullptr ||
        xSemaphoreTake(wifiScanMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
      request->send(503, "application/json", "{\"error\":\"wifi_scan_busy\"}");
      return;
    }

    if (wifiScanActive.load(std::memory_order_acquire) &&
        WiFi.scanComplete() == WIFI_SCAN_RUNNING)
    {
      xSemaphoreGive(wifiScanMutex);
      request->send(202, "application/json", "{\"state\":\"scanning\"}");
      return;
    }

    WiFi.scanDelete();
    wifiScanActive.store(false, std::memory_order_release);
    const int16_t scanResult = WiFi.scanNetworks(true, true, false, 1000);
    if (scanResult == WIFI_SCAN_FAILED)
    {
      Serial.printf("Wi-Fi scan failed to start (result=%d, status=%d)\n",
                    scanResult, static_cast<int>(WiFi.status()));
      xSemaphoreGive(wifiScanMutex);
      request->send(500, "application/json", "{\"error\":\"wifi_scan_failed\"}");
      return;
    }

    wifiScanActive.store(true, std::memory_order_release);
    Serial.printf("Wi-Fi scan started (result=%d; -1 means still running)\n", scanResult);
    xSemaphoreGive(wifiScanMutex);
    request->send(202, "application/json", "{\"state\":\"scanning\"}");
  });

  server.on("/api/wifi/networks", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    if (!isSetupApClient(request))
    {
      sendSetupApRequired(request);
      return;
    }
    if (wifiScanMutex == nullptr ||
        xSemaphoreTake(wifiScanMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
      request->send(503, "application/json", "{\"error\":\"wifi_scan_busy\"}");
      return;
    }
    if (!wifiScanActive.load(std::memory_order_acquire))
    {
      xSemaphoreGive(wifiScanMutex);
      request->send(200, "application/json",
                    "{\"state\":\"idle\",\"networks\":[]}");
      return;
    }

    const int16_t networkCount = WiFi.scanComplete();
    if (networkCount == WIFI_SCAN_RUNNING)
    {
      xSemaphoreGive(wifiScanMutex);
      request->send(202, "application/json", "{\"state\":\"scanning\"}");
      return;
    }
    if (networkCount == WIFI_SCAN_FAILED)
    {
      Serial.printf("Wi-Fi scan failed to complete (status=%d)\n", networkCount);
      wifiScanActive.store(false, std::memory_order_release);
      WiFi.scanDelete();
      xSemaphoreGive(wifiScanMutex);
      request->send(500, "application/json", "{\"error\":\"wifi_scan_failed\"}");
      return;
    }

    AsyncResponseStream* response =
        request->beginResponseStream("application/json; charset=utf-8");
    Serial.printf("Wi-Fi scan completed: %d networks found\n", networkCount);
    response->print("{\"state\":\"complete\",\"networks\":[");
    bool firstNetwork = true;
    for (int16_t index = 0; index < networkCount; ++index)
    {
      const String ssid = WiFi.SSID(index);
      if (ssid.isEmpty() || ssid == WIFI_AP_SSID)
      {
        continue;
      }
      if (!firstNetwork)
      {
        response->write(',');
      }
      firstNetwork = false;
      response->print("{\"ssid\":");
      appendJsonString(*response, ssid.c_str());
      response->printf(",\"rssi\":%d,\"channel\":%d,\"secure\":%s}",
                       WiFi.RSSI(index),
                       WiFi.channel(index),
                       WiFi.encryptionType(index) == WIFI_AUTH_OPEN ? "false" : "true");
    }
    response->print("]}");
    xSemaphoreGive(wifiScanMutex);
    request->send(response);
  });

  server.on("/api/wifi/config", HTTP_POST, [](AsyncWebServerRequest* request)
  {
    if (!isSetupApClient(request))
    {
      sendSetupApRequired(request);
      return;
    }
    if (!wifiPreferencesAvailable)
    {
      request->send(503, "application/json", "{\"error\":\"wifi_storage_unavailable\"}");
      return;
    }
    bool scanRunning = false;
    if (wifiScanMutex != nullptr)
    {
      if (xSemaphoreTake(wifiScanMutex, pdMS_TO_TICKS(100)) != pdTRUE)
      {
        request->send(503, "application/json", "{\"error\":\"wifi_scan_busy\"}");
        return;
      }
      scanRunning = wifiScanActive.load(std::memory_order_acquire) &&
                    WiFi.scanComplete() == WIFI_SCAN_RUNNING;
      if (wifiScanActive.load(std::memory_order_acquire) && !scanRunning)
      {
        WiFi.scanDelete();
        wifiScanActive.store(false, std::memory_order_release);
      }
      xSemaphoreGive(wifiScanMutex);
    }
    if (scanRunning)
    {
      request->send(409, "application/json", "{\"error\":\"wifi_scan_in_progress\"}");
      return;
    }
    if (!request->hasParam("ssid", true) || !request->hasParam("password", true))
    {
      request->send(400, "application/json", "{\"error\":\"missing_credentials\"}");
      return;
    }

    const String ssid = request->getParam("ssid", true)->value();
    const String password = request->getParam("password", true)->value();
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 64)
    {
      request->send(400, "application/json", "{\"error\":\"invalid_credentials\"}");
      return;
    }
    if (wifiConfigMutex == nullptr ||
        xSemaphoreTake(wifiConfigMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
      request->send(503, "application/json", "{\"error\":\"wifi_config_busy\"}");
      return;
    }

    ssid.toCharArray(requestedWifiSsid, sizeof(requestedWifiSsid));
    password.toCharArray(requestedWifiPassword, sizeof(requestedWifiPassword));
    xSemaphoreGive(wifiConfigMutex);
    wifiConfigRequestPending.store(true, std::memory_order_release);
    wifiProvisionState.store(WIFI_CONNECTING, std::memory_order_release);
    request->send(202, "application/json", "{\"accepted\":true}");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    AsyncWebServerResponse* response = request->beginResponse_P(
        200, "text/html; charset=utf-8", BOARD_PAGE);
    response->addHeader("Cache-Control", "no-cache, must-revalidate");
    response->addHeader("Connection", "close");
    request->send(response);
  });

  server.on("/api/messages/export.json", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    File file;
    if (!messageBoard.openReaderExclusive(file))
    {
      request->send(500, "application/json", "{\"error\":\"board_unavailable\"}");
      return;
    }

    // Hold the exclusive reader lock while streaming so records cannot be rewritten mid-export.
    AsyncResponseStream* response = request->beginResponseStream("application/json; charset=utf-8");
    response->addHeader("Content-Disposition", "attachment; filename=aredn-messages.json");
    response->printf("{\n  \"format\": \"aredn-message-board\",\n  \"message_count\": %lu,\n  \"messages\": [\n",
                     static_cast<unsigned long>(messageBoard.messageCount()));
    uint32_t count = 0;
    writeMessageItems(*response, file, count, true, true);
    response->printf("\n  ]\n}\n");
    messageBoard.closeReaderExclusive(file);
    request->send(response);
  });

  server.on("/api/admin/login", HTTP_POST, [](AsyncWebServerRequest* request)
  {
    if (!request->hasParam("password", true) ||
        request->getParam("password", true)->value() != ADMIN_PASSWORD)
    {
      request->send(403, "application/json", "{\"error\":\"invalid_password\"}");
      return;
    }

    // There is one active administrator session; a new login replaces its token.
    uint32_t token = esp_random();
    if (token == 0)
    {
      token = 1;
    }
    adminSessionToken.store(token, std::memory_order_release);
    AsyncWebServerResponse* response = request->beginResponse(
        200, "application/json", "{\"authenticated\":true}");
    response->addHeader("Set-Cookie", "aredn_admin=" + String(token, HEX) +
      "; Path=/; Max-Age=3600; HttpOnly; SameSite=Strict");
    request->send(response);
  });

  server.on("/api/admin/logout", HTTP_POST, [](AsyncWebServerRequest* request)
  {
    adminSessionToken.store(0, std::memory_order_release);
    AsyncWebServerResponse* response = request->beginResponse(
        200, "application/json", "{\"authenticated\":false}");
    response->addHeader("Set-Cookie", "aredn_admin=; Max-Age=0; HttpOnly; SameSite=Strict");
    request->send(response);
  });

  server.on("/api/admin/status", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    request->send(200, "application/json",
                  isAdminAuthenticated(request)
                      ? "{\"authenticated\":true}"
                      : "{\"authenticated\":false}");
  });

  server.on("/api/admin/import", HTTP_POST, [](AsyncWebServerRequest* request)
  {
    ImportUploadState* state = static_cast<ImportUploadState*>(request->_tempObject);
    if (!isAdminAuthenticated(request))
    {
      if (state != nullptr && state->started)
      {
        messageBoard.abortImport();
        importUploadActive.store(false, std::memory_order_release);
      }
      request->_tempObject = nullptr;
      delete state;
      sendAdminRequired(request);
      return;
    }
    const bool failed = state == nullptr || state->failed;
    const char* importError = messageBoard.importError();
    request->_tempObject = nullptr;
    delete state;
    if (failed)
    {
      String response = "{\"error\":\"import_failed\",\"reason\":\"";
      response += importError;
      response += "\"}";
      request->send(400, "application/json", response);
    }
    else
    {
      request->send(200, "application/json", "{\"imported\":true}");
    }
  }, nullptr, [](AsyncWebServerRequest* request, uint8_t* data,
                 size_t length, size_t index, size_t total)
  {
    // AsyncWebServer delivers uploads in chunks; keep state on the request until the final chunk.
    if (index == 0)
    {
      request->_tempObject = new ImportUploadState();
      ImportUploadState* state = static_cast<ImportUploadState*>(request->_tempObject);
      const bool claimed = !importUploadActive.exchange(true, std::memory_order_acq_rel);
      if (state == nullptr || !claimed || !isAdminAuthenticated(request) ||
          !messageBoard.beginImport(total))
      {
        if (claimed)
        {
          importUploadActive.store(false, std::memory_order_release);
        }
        if (state != nullptr)
        {
          state->failed = true;
        }
      }
      else
      {
        state->started = true;
      }
    }
    ImportUploadState* state = static_cast<ImportUploadState*>(request->_tempObject);
    if (state != nullptr && !state->failed &&
        !messageBoard.appendImportData(data, length))
    {
      state->failed = true;
    }
    if (index + length == total)
    {
      if (state != nullptr && state->started && state->failed)
      {
        messageBoard.abortImport();
      }
      else if (state != nullptr && state->started && !messageBoard.finishImport())
      {
        state->failed = true;
      }
      if (state != nullptr && state->started)
      {
        importUploadActive.store(false, std::memory_order_release);
        if (!state->failed)
        {
          const uint32_t revision = boardRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
          fullRefreshRevision.store(revision, std::memory_order_release);
        }
      }
    }
  });

  server.on("/api/admin/messages", HTTP_DELETE, [](AsyncWebServerRequest* request)
  {
    if (!isAdminAuthenticated(request))
    {
      sendAdminRequired(request);
      return;
    }
    const bool reset = messageBoard.reset();
    if (reset)
    {
      const uint32_t revision = boardRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
      fullRefreshRevision.store(revision, std::memory_order_release);
    }
    request->send(reset ? 200 : 500,
                  "application/json",
            reset ? "{\"reset\":true}" : "{\"error\":\"reset_failed\"}");
  });

  server.on("/api/admin/message", HTTP_DELETE, [](AsyncWebServerRequest* request)
  {
    if (!isAdminAuthenticated(request))
    {
      sendAdminRequired(request);
      return;
    }
    if (!request->hasParam("id"))
    {
      request->send(400, "application/json", "{\"error\":\"missing_id\"}");
      return;
    }
    const uint32_t id = request->getParam("id")->value().toInt();
    const bool deleted = messageBoard.remove(id);
    if (deleted)
    {
      const uint32_t revision = boardRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
      fullRefreshRevision.store(revision, std::memory_order_release);
    }
    request->send(deleted ? 200 : 404,
                  "application/json",
            deleted ? "{\"deleted\":true}" : "{\"error\":\"message_not_found\"}");
  });

  server.on("/api/admin/message", HTTP_POST, [](AsyncWebServerRequest* request)
  {
    if (!isAdminAuthenticated(request))
    {
      sendAdminRequired(request);
      return;
    }
    if (!request->hasParam("id"))
    {
      request->send(400, "application/json", "{\"error\":\"missing_id\"}");
      return;
    }
    if (!request->hasParam("name", true) || !request->hasParam("text", true))
    {
      request->send(400, "application/json", "{\"error\":\"missing_fields\"}");
      return;
    }
    String name = request->getParam("name", true)->value();
    String text = request->getParam("text", true)->value();
    FfatMessageBoard::Priority priority = FfatMessageBoard::Priority::Green;
    if (request->hasParam("priority", true) &&
        !parseMessagePriority(request->getParam("priority", true)->value(), priority))
    {
      request->send(400, "application/json", "{\"error\":\"invalid_priority\"}");
      return;
    }
    name.trim();
    text.trim();
    const uint32_t id = request->getParam("id")->value().toInt();
    if (name.isEmpty() || text.isEmpty() ||
        name.length() > FfatMessageBoard::MAX_NAME_LENGTH ||
        text.length() > FfatMessageBoard::MAX_MESSAGE_LENGTH)
    {
      request->send(400, "application/json", "{\"error\":\"invalid_message\"}");
      return;
    }
    const bool updated = messageBoard.update(id, name.c_str(), text.c_str(), priority);
    if (updated)
    {
      const uint32_t revision = boardRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
      fullRefreshRevision.store(revision, std::memory_order_release);
    }
    request->send(updated ? 200 : 404,
                  "application/json",
            updated
                      ? "{\"updated\":true}"
                      : "{\"error\":\"message_not_found\"}");
  });

  server.on("/api/messages", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    if (request->hasParam("c_time"))
    {
      const String clientTimeValue = request->getParam("c_time")->value();
      char* parseEnd = nullptr;
      const long long clientEpoch = strtoll(clientTimeValue.c_str(), &parseEnd, 10);
      if (parseEnd != clientTimeValue.c_str() && *parseEnd == '\0')
      {
        const uint32_t clientAddress = request->client() == nullptr
            ? 0
            : static_cast<uint32_t>(request->client()->remoteIP());
        synchronizeFromClient(static_cast<int64_t>(clientEpoch), clientAddress);
      }
    }

    File file;
    if (!messageBoard.openReaderExclusive(file))
    {
      request->send(500, "application/json", "{\"error\":\"board_unavailable\"}");
      return;
    }

    const bool clockValid = isDeviceTimeValid();
    const time_t currentEpoch = clockValid ? time(nullptr) : 0;
    // Keep interactive pages bounded by default; callers must opt in to a larger page.
    uint32_t limit = 20;
    if (request->hasParam("all"))
    {
      limit = 0;
    }
    else if (request->hasParam("limit"))
    {
      const long requestedLimit = request->getParam("limit")->value().toInt();
      limit = requestedLimit > 0 ? static_cast<uint32_t>(requestedLimit) : 20;
      if (limit > 100)
      {
        limit = 100;
      }
    }
    const uint32_t beforeId = request->hasParam("before")
        ? request->getParam("before")->value().toInt()
        : 0;
    const uint32_t afterId = request->hasParam("after")
        ? request->getParam("after")->value().toInt()
        : 0;
    String searchTerm;
    if (request->hasParam("search"))
    {
      searchTerm = request->getParam("search")->value();
      searchTerm.trim();
      if (searchTerm.length() > 64)
      {
        messageBoard.closeReaderExclusive(file);
        request->send(400, "application/json", "{\"error\":\"search_too_long\"}");
        return;
      }
    }
    int priorityFilter = -1;
    if (request->hasParam("priority"))
    {
      const String priorityValue = request->getParam("priority")->value();
      if (priorityValue != "all" &&
          (priorityValue == "green" || priorityValue == "orange" || priorityValue == "red"))
      {
        FfatMessageBoard::Priority parsedPriority;
        parseMessagePriority(priorityValue, parsedPriority);
        priorityFilter = static_cast<int>(parsedPriority);
      }
      else if (priorityValue != "all")
      {
        messageBoard.closeReaderExclusive(file);
        request->send(400, "application/json", "{\"error\":\"invalid_priority\"}");
        return;
      }
    }
    bool hasMore = false;
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    const uint32_t currentRevision = boardRevision.load(std::memory_order_acquire);
    // Clients use these compact fields to decide whether polling can stay incremental.
    const bool requiresFullRefresh = fullRefreshRevision.load(std::memory_order_acquire) == currentRevision;
    response->printf("{\"r\":%lu,\"f\":%s,\"v\":%s,\"s\":\"%s\",\"e\":%lld,\"p\":[",
             static_cast<unsigned long>(currentRevision),
             requiresFullRefresh ? "true" : "false",
                     clockValid ? "true" : "false",
                     deviceTimeSourceName(),
                     static_cast<long long>(currentEpoch));
    uint32_t count = 0;
    writeMessageItems(*response, file, count, false, true, limit, beforeId, afterId, &hasMore,
              searchTerm.isEmpty() ? nullptr : &searchTerm, priorityFilter);
    response->printf("],\"h\":%s,\"c\":%lu,\"b\":%lu,\"z\":%lu}",
             hasMore ? "true" : "false",
                     static_cast<unsigned long>(messageBoard.messageCount()),
                     static_cast<unsigned long>(messageBoard.usedBytes()),
                     static_cast<unsigned long>(messageBoard.totalBytes()));
    messageBoard.closeReaderExclusive(file);
    request->send(response);
  });

  server.on("/api/time", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    char response[128];
    const bool valid = isDeviceTimeValid();
    snprintf(response, sizeof(response),
             "{\"valid\":%s,\"source\":\"%s\",\"epoch\":%lld}",
             valid ? "true" : "false",
             deviceTimeSourceName(),
             static_cast<long long>(valid ? time(nullptr) : 0));
    request->send(200, "application/json", response);
  });

  server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest* request)
  {
    if (!isAdminAuthenticated(request))
    {
      sendAdminRequired(request);
      return;
    }
    if (!request->hasParam("epoch", true))
    {
      request->send(400, "application/json", "{\"error\":\"missing_epoch\"}");
      return;
    }

    const String epochValue = request->getParam("epoch", true)->value();
    char* parseEnd = nullptr;
    const long long parsedEpoch = strtoll(epochValue.c_str(), &parseEnd, 10);
    if (parseEnd == epochValue.c_str() || *parseEnd != '\0' ||
        parsedEpoch < MIN_VALID_EPOCH || parsedEpoch > MAX_VALID_EPOCH)
    {
      request->send(400, "application/json", "{\"error\":\"invalid_epoch\"}");
      return;
    }

    if (clientTimeMutex == nullptr ||
        xSemaphoreTake(clientTimeMutex, portMAX_DELAY) != pdTRUE)
    {
      request->send(503, "application/json", "{\"error\":\"clock_busy\"}");
      return;
    }

    const struct timeval manualTime = {static_cast<time_t>(parsedEpoch), 0};
    if (settimeofday(&manualTime, nullptr) != 0)
    {
      xSemaphoreGive(clientTimeMutex);
      request->send(500, "application/json", "{\"error\":\"clock_update_failed\"}");
      return;
    }

    resetClientTimeSamples();
    deviceTimeSource.store(TIME_SOURCE_MANUAL, std::memory_order_release);
    xSemaphoreGive(clientTimeMutex);
    request->send(200, "application/json", "{\"valid\":true,\"source\":\"manual\"}");
  });

  server.on("/api/messages", HTTP_POST, handleSubmitMessage);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    const bool connected = WiFi.status() == WL_CONNECTED;
    const String ipAddress = connected ? WiFi.localIP().toString() : "0.0.0.0";
    const int rssi = connected ? WiFi.RSSI() : 0;
    char response[480];

    snprintf(
        response,
        sizeof(response),
      "{\"service\":\"aredn-service\",\"status\":\"ok\","
        "\"board_ready\":%s,"
        "\"message_count\":%lu,"
        "\"storage_used_bytes\":%lu,\"storage_total_bytes\":%lu,"
        "\"storage_free_bytes\":%lu,\"record_size_bytes\":%lu,"
        "\"index_capacity\":%lu,\"index_bytes\":%lu,"
        "\"uptime_ms\":%lu,\"free_heap\":%lu,\"wifi_connected\":%s,"
        "\"ip\":\"%s\",\"rssi_dbm\":%d}",
      messageBoard.ready() ? "true" : "false",
      static_cast<unsigned long>(messageBoard.messageCount()),
      static_cast<unsigned long>(messageBoard.usedBytes()),
      static_cast<unsigned long>(messageBoard.totalBytes()),
      static_cast<unsigned long>(messageBoard.freeBytes()),
      static_cast<unsigned long>(messageBoard.recordSize()),
      static_cast<unsigned long>(messageBoard.indexCapacity()),
      static_cast<unsigned long>(messageBoard.indexBytes()),
        static_cast<unsigned long>(millis()),
        static_cast<unsigned long>(ESP.getFreeHeap()),
        connected ? "true" : "false",
        ipAddress.c_str(),
        rssi);
    request->send(200, "application/json", response);
  });

  server.onNotFound([](AsyncWebServerRequest* request)
  {
    request->send(404, "text/plain; charset=utf-8", "Not found");
  });
}
}

void setup()
{
  // Bring up diagnostics and the optional display before initializing storage and networking.
  Serial.begin(115200);

  unsigned long startZeit = millis();
  while (!Serial && (millis() - startZeit < 1500))
  {
    delay(10);
  }
  unsigned long dauer = millis() - startZeit;
  
  delay(200); 
  Serial.print("[Serial connection ready. Connection time: ");
  Serial.print(dauer);
  Serial.println(" ms.]");

  Serial.println("\nAREDN service starting");

  Serial.printf("ESP reset reason: ");
  esp_reset_reason_t r = esp_reset_reason();
  switch(r) {
    case ESP_RST_UNKNOWN:   Serial.printf("UNKNOWN (Reset reason can not be determined)"); break;
    case ESP_RST_POWERON:   Serial.printf("Power-On"); break;
    case ESP_RST_EXT:       Serial.printf("EXT_PIN (Reset by external pin)"); break;
    case ESP_RST_SW:        Serial.printf("Software Reset"); break;
    case ESP_RST_PANIC:     Serial.printf("❌ PANIC"); break;
    case ESP_RST_INT_WDT:   Serial.printf("❌ Interrupt WDT"); break;
    case ESP_RST_TASK_WDT:  Serial.printf("❌ Task WDT"); break;
    case ESP_RST_WDT:       Serial.printf("❌ Other WDT"); break;
    case ESP_RST_DEEPSLEEP: Serial.printf("Deep Sleep"); break;
    case ESP_RST_BROWNOUT:  Serial.printf("❌ Brownout"); break;
    case ESP_RST_SDIO:      Serial.printf("SDIO (Reset over SDIO)"); break;
  }
  Serial.printf(" (Code %d)\n", static_cast<int>(r));
  Serial.flush();
  initializeDisplay();
  Serial.printf("Hardware: %s | cores: %d | PSRAM: %lu KB | flash: %lu MB | free heap: %lu KB\n",
                ESP.getChipModel(),
                ESP.getChipCores(),
                static_cast<unsigned long>(ESP.getPsramSize() / 1024),
                static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)),
                static_cast<unsigned long>(ESP.getFreeHeap() / 1024));

  if (!messageBoard.begin())
  {
    Serial.println("ERROR: Could not initialize the FFat message board");
  }
  clientTimeMutex = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  const IPAddress setupIp(192, 168, 4, 1);
  const IPAddress setupGateway(192, 168, 4, 1);
  const IPAddress setupSubnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(setupIp, setupGateway, setupSubnet))
  {
    Serial.println("ERROR: Could not configure the Wi-Fi setup AP address");
  }
  if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD))
  {
    Serial.println("ERROR: Could not start the Wi-Fi setup AP");
  }
  else
  {
    Serial.printf("Wi-Fi setup AP started: SSID=\"%s\" | IP=%s\n",
                  WIFI_AP_SSID,
                  WiFi.softAPIP().toString().c_str());
  }
  wifiConfigMutex = xSemaphoreCreateMutex();
  if (wifiConfigMutex == nullptr)
  {
    Serial.println("ERROR: Could not create the Wi-Fi configuration mutex");
  }
  wifiScanMutex = xSemaphoreCreateMutex();
  if (wifiScanMutex == nullptr)
  {
    Serial.println("ERROR: Could not create the Wi-Fi scan mutex");
  }

  wifiPreferencesAvailable = wifiPreferences.begin("aredn-wifi", false);
  if (!wifiPreferencesAvailable)
  {
    Serial.println("ERROR: Could not open Wi-Fi credential storage");
    wifiProvisionState.store(WIFI_SETUP_REQUIRED, std::memory_order_release);
  }
  else
  {
    StoredWifiCredentials storedCredentials{};
    const size_t storedSize = wifiPreferences.getBytesLength("credentials");
    if (storedSize == sizeof(storedCredentials) &&
        wifiPreferences.getBytes("credentials", &storedCredentials,
                                 sizeof(storedCredentials)) == sizeof(storedCredentials) &&
        storedCredentials.ssid[sizeof(storedCredentials.ssid) - 1] == '\0' &&
        storedCredentials.password[sizeof(storedCredentials.password) - 1] == '\0' &&
        storedCredentials.ssid[0] != '\0')
    {
      savedWifiSsid = storedCredentials.ssid;
      savedWifiPassword = storedCredentials.password;
      wifiCredentialsSaved.store(true, std::memory_order_release);
      wifiProvisionState.store(WIFI_CONNECTING, std::memory_order_release);
      WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
      wifiConnectAttemptStart = millis();
      wifiConnectAttemptActive = true;
      lastReconnectAttempt = wifiConnectAttemptStart;
      Serial.printf("Connecting to saved Wi-Fi network \"%s\"\n", savedWifiSsid.c_str());
    }
    else
    {
      wifiProvisionState.store(WIFI_SETUP_REQUIRED, std::memory_order_release);
      Serial.println("No saved Wi-Fi credentials; waiting for setup through the access point");
    }
  }
  Serial.printf("Setup page: http://%s/wifi\n", WiFi.softAPIP().toString().c_str());
  Serial.println("Client time synchronization enabled");
  Serial.printf("Wi-Fi connection started (status %d)\n", static_cast<int>(WiFi.status()));

  registerRoutes();
  DefaultHeaders::Instance().addHeader("Connection", "close");
  server.begin();
  Serial.println("Async HTTP server started on port 80");
}

void loop()
{
  const uint32_t now = millis();
  bool connected = WiFi.status() == WL_CONNECTED;

  if (wifiConfigRequestPending.exchange(false, std::memory_order_acq_rel))
  {
    char candidateSsid[sizeof(requestedWifiSsid)]{};
    char candidatePassword[sizeof(requestedWifiPassword)]{};
    if (wifiConfigMutex != nullptr &&
        xSemaphoreTake(wifiConfigMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      memcpy(candidateSsid, requestedWifiSsid, sizeof(candidateSsid));
      memcpy(candidatePassword, requestedWifiPassword, sizeof(candidatePassword));
      xSemaphoreGive(wifiConfigMutex);

      memcpy(activeWifiSsid, candidateSsid, sizeof(activeWifiSsid));
      memcpy(activeWifiPassword, candidatePassword, sizeof(activeWifiPassword));
      wifiCandidateSawDisconnect = !connected;
      WiFi.disconnect(false, false);
      WiFi.begin(activeWifiSsid, activeWifiPassword);
      wifiConnectAttemptStart = now;
      wifiConnectAttemptActive = true;
      wifiSavingCandidate = true;
      wifiRecoveryPending = false;
      lastReconnectAttempt = now;
      wifiProvisionState.store(WIFI_CONNECTING, std::memory_order_release);
      Serial.printf("Testing Wi-Fi credentials for \"%s\"\n", candidateSsid);
    }
    else
    {
      wifiProvisionState.store(WIFI_FAILED, std::memory_order_release);
      Serial.println("ERROR: Could not read the pending Wi-Fi configuration");
    }
  }
  connected = WiFi.status() == WL_CONNECTED;
  if (wifiSavingCandidate && !connected)
  {
    wifiCandidateSawDisconnect = true;
  }
  const bool connectedForAttempt =
      connected && (!wifiSavingCandidate || wifiCandidateSawDisconnect);

  if (connectedForAttempt && wifiConnectAttemptActive)
  {
    if (wifiSavingCandidate)
    {
      StoredWifiCredentials candidateCredentials{};
      strncpy(candidateCredentials.ssid, activeWifiSsid,
              sizeof(candidateCredentials.ssid) - 1);
      strncpy(candidateCredentials.password, activeWifiPassword,
              sizeof(candidateCredentials.password) - 1);
      if (wifiPreferences.putBytes("credentials", &candidateCredentials,
                                  sizeof(candidateCredentials)) != sizeof(candidateCredentials))
      {
        wifiProvisionState.store(WIFI_FAILED, std::memory_order_release);
        Serial.println("ERROR: Could not save the Wi-Fi credentials");
        WiFi.disconnect(false, false);
        wifiSavingCandidate = false;
        wifiCandidateSawDisconnect = false;
        wifiConnectAttemptActive = false;
        wifiRecoveryPending = !savedWifiSsid.isEmpty();
        wifiRecoveryStart = now;
      }
      else
      {
        savedWifiSsid = candidateCredentials.ssid;
        savedWifiPassword = candidateCredentials.password;
        wifiCredentialsSaved.store(true, std::memory_order_release);
        wifiProvisionState.store(WIFI_CONNECTED, std::memory_order_release);
        wifiSavingCandidate = false;
        wifiCandidateSawDisconnect = false;
        wifiConnectAttemptActive = false;
        Serial.printf("Wi-Fi credentials saved; connected to \"%s\"\n",
                      savedWifiSsid.c_str());
      }
    }
    else
    {
      wifiProvisionState.store(WIFI_CONNECTED, std::memory_order_release);
      wifiConnectAttemptActive = false;
    }
  }
  else if (wifiConnectAttemptActive &&
           now - wifiConnectAttemptStart >= WIFI_CONNECT_TIMEOUT_MS)
  {
    wifiConnectAttemptActive = false;
    wifiProvisionState.store(WIFI_FAILED, std::memory_order_release);
    Serial.println("ERROR: Wi-Fi connection attempt timed out; setup AP remains available");
    if (wifiSavingCandidate)
    {
      wifiSavingCandidate = false;
      wifiCandidateSawDisconnect = false;
      wifiRecoveryPending = !savedWifiSsid.isEmpty();
      wifiRecoveryStart = now;
    }
    lastReconnectAttempt = now;
  }

  if (wifiRecoveryPending && now - wifiRecoveryStart >= WIFI_RECOVERY_DELAY_MS)
  {
    wifiRecoveryPending = false;
    WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
    wifiConnectAttemptStart = now;
    wifiConnectAttemptActive = true;
    lastReconnectAttempt = now;
    wifiProvisionState.store(WIFI_CONNECTING, std::memory_order_release);
    Serial.printf("Retrying previously saved Wi-Fi network \"%s\"\n", savedWifiSsid.c_str());
  }

  if (displayAvailable)
  {
    // Refresh the cached latest post only when a write changes the board revision.
    const uint32_t currentRevision = boardRevision.load(std::memory_order_acquire);
    const bool boardChanged = currentRevision != displayedBoardRevision;
    if (boardChanged)
    {
      displayedBoardRevision = currentRevision;
      refreshLatestDisplayMessage();
    }
    if (boardChanged || now - lastDisplayUpdate >= OLED_PAGE_INTERVAL_MS)
    {
      // Show changed content immediately; otherwise alternate between status and message pages.
      if (!boardChanged && lastDisplayUpdate != 0)
      {
        displayedPage = static_cast<uint8_t>(displayedPage == 0 ? 1 : 0);
      }
      lastDisplayUpdate = now;
      drawBoardDisplay();
    }
  }

/*   if (now - lastSerialStatus >= SERIAL_STATUS_INTERVAL_MS)
  {
    lastSerialStatus = now;
    char clockTime[14]{};
    formatSerialClock(clockTime, sizeof(clockTime));
    Serial.printf("Status: time %s | heap %lu KB | Wi-Fi %s | RSSI %d dBm | messages %lu\n",
                  clockTime,
                  static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
                  connected ? "connected" : "disconnected",
                  connected ? WiFi.RSSI() : 0,
            static_cast<unsigned long>(messageBoard.messageCount()));
  }
 */
  if (connected && !wasConnected)
  {
    const String ipAddress = WiFi.localIP().toString();
    Serial.printf("Connected to AREDN WLAN; IP: %s\n", ipAddress.c_str());
    Serial.printf("Message board: http://%s/\n", ipAddress.c_str());
    Serial.printf("Service status: http://%s/api/status\n", ipAddress.c_str());
    wasConnected = true;
  }
  else if (!connected && wasConnected)
  {
    Serial.println("AREDN WLAN disconnected");
    wasConnected = false;
  }

  // Retry saved credentials without interrupting the setup access point.
  if (!connected && !wifiConnectAttemptActive && !wifiRecoveryPending &&
      wifiCredentialsSaved.load(std::memory_order_acquire) &&
      now - lastReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS)
  {
    lastReconnectAttempt = now;
    wifiConnectAttemptStart = now;
    wifiConnectAttemptActive = true;
    wifiProvisionState.store(WIFI_CONNECTING, std::memory_order_release);
    Serial.printf("Retrying saved Wi-Fi connection (status %d)\n",
                  static_cast<int>(WiFi.status()));
    WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  }

  delay(10);
}