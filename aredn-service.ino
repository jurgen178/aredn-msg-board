#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <atomic>
#include <ctime>
#include <cstdlib>
#include <sys/time.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include "BoardPage.h"
#include "FfatMessageBoard.h"
#include "arduino_secrets.h"


namespace
{
constexpr char WIFI_SSID[] = SECRET_SSID;
constexpr char WIFI_PASSWORD[] = SECRET_PASS;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t NTP_SYNC_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr int64_t MIN_VALID_EPOCH = 1577836800LL;
constexpr int64_t MAX_VALID_EPOCH = 4102444800LL;
constexpr uint8_t TIME_SOURCE_UNSET = 0;
constexpr uint8_t TIME_SOURCE_NETWORK = 1;
constexpr uint8_t TIME_SOURCE_MANUAL = 2;
constexpr char ADMIN_PASSWORD[] = "aredn-admin";
constexpr uint32_t SERIAL_STATUS_INTERVAL_MS = 15000;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr int8_t OLED_RESET = -1;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr uint32_t OLED_PAGE_INTERVAL_MS = 5000;

AsyncWebServer server(80);
AsyncEventSource events("/api/events");
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
FfatMessageBoard messageBoard;
std::atomic<uint8_t> deviceTimeSource{TIME_SOURCE_UNSET};
std::atomic<uint32_t> adminSessionToken{0};
std::atomic<uint32_t> boardRevision{1};
std::atomic<bool> importUploadActive{false};
uint32_t lastReconnectAttempt = 0;
bool wasConnected = false;
uint32_t lastEventHeartbeat = 0;
uint32_t lastSerialStatus = 0;
uint32_t lastDisplayUpdate = 0;
uint32_t displayedBoardRevision = 0;
uint8_t displayedPage = 0;
bool displayAvailable = false;
bool latestMessageAvailable = false;
FfatMessageBoard::Message latestMessage{};

struct ImportUploadState
{
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

void refreshLatestDisplayMessage()
{
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

  if (displayedPage == 0)
  {
    display.println("AREDN MESSAGE BOARD");
    drawDisplayLine(1, "IP: ", WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString().c_str()
        : "DISCONNECTED");
    char metrics[22];
    snprintf(metrics, sizeof(metrics), "%lu", static_cast<unsigned long>(messageBoard.messageCount()));
    drawDisplayLine(3, "Messages: ", metrics);
    snprintf(metrics, sizeof(metrics), "%lu", static_cast<unsigned long>(messageBoard.pending()));
    drawDisplayLine(4, "Pending: ", metrics);
    snprintf(metrics, sizeof(metrics), "%d dBm",
             WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    drawDisplayLine(5, "WiFi: ", metrics);
    snprintf(metrics, sizeof(metrics), "%lu KB",
             static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
    drawDisplayLine(6, "Heap: ", metrics);
  }
  else
  {
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
      display.println("LAST MESSAGE");
      display.setCursor(0, 16);
      printDisplayUtf8(latestMessage.name);
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
  Wire.begin();
  Wire.beginTransmission(OLED_ADDRESS);
  if (Wire.endTransmission() != 0 ||
      !display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
  {
    Serial.printf("OLED unavailable at address 0x%02X\n", OLED_ADDRESS);
    return;
  }

  displayAvailable = true;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("AREDN MESSAGE BOARD");
  display.println();
  display.println("STARTING SERVICE...");
  display.display();
}

char foldSearchCharacter(char value)
{
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool containsSearchTerm(const char* value, const String& searchTerm)
{
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

void sendBoardEvent(const char* eventName, uint32_t messageId = 0)
{
  char payload[96];
  snprintf(payload, sizeof(payload), "{\"revision\":%lu,\"message_id\":%lu}",
           static_cast<unsigned long>(boardRevision.load(std::memory_order_acquire)),
           static_cast<unsigned long>(messageId));
  events.send(payload, eventName, millis());
}

bool isDeviceTimeValid()
{
  return static_cast<int64_t>(time(nullptr)) >= MIN_VALID_EPOCH;
}

const char* deviceTimeSourceName()
{
  if (!isDeviceTimeValid())
  {
    return "unset";
  }

  return deviceTimeSource.load(std::memory_order_acquire) == TIME_SOURCE_MANUAL
      ? "manual"
      : "network";
}

void onNetworkTimeSync(struct timeval* syncedTime)
{
  if (syncedTime != nullptr && static_cast<int64_t>(syncedTime->tv_sec) >= MIN_VALID_EPOCH)
  {
    deviceTimeSource.store(TIME_SOURCE_NETWORK, std::memory_order_release);
  }
}

void appendJsonString(Print& output, const char* value)
{
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
  const uint32_t token = adminSessionToken.load(std::memory_order_acquire);
  if (token == 0 || !request->hasHeader("Cookie"))
  {
    return false;
  }

  const String expectedCookie = "aredn_admin=" + String(token, HEX);
  return request->getHeader("Cookie")->value().indexOf(expectedCookie) >= 0;
}

void sendAdminRequired(AsyncWebServerRequest* request)
{
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
                       const String* searchTerm = nullptr)
{
  if (hasMore != nullptr)
  {
    *hasMore = false;
  }
  const uint32_t storedCount = messageBoard.messageCount();
  if (storedCount == 0)
  {
    return;
  }

  const bool sequentialSearch = searchTerm != nullptr && beforeId == 0 && afterId == 0;
  const bool incrementalLatest = afterId != 0 && beforeId == 0 && searchTerm == nullptr;
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
    const bool readable = sequentialSearch
        ? messageBoard.readNext(file, message)
        : messageBoard.readAtExclusive(file, index, message);
    if (!readable ||
        (beforeId != 0 && message.id >= beforeId) ||
        (afterId != 0 && message.id <= afterId) ||
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
      response.print("\n    }");
    }
    else
    {
      response.printf("{\"id\":%lu,\"created_at\":",
                      static_cast<unsigned long>(message.id));
      appendIsoTimestamp(response, message.createdAtEpoch);
      response.print(",\"name\":");
      appendJsonString(response, message.name);
      response.print(",\"text\":");
      appendJsonString(response, message.text);
      response.write('}');
    }
    ++count;
  }
}

void handleSubmitMessage(AsyncWebServerRequest* request)
{
  if (!request->hasParam("name", true) || !request->hasParam("text", true))
  {
    request->send(400, "application/json", "{\"error\":\"missing_fields\"}");
    return;
  }

  String author = request->getParam("name", true)->value();
  String text = request->getParam("text", true)->value();
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
  const FfatMessageBoard::SubmitResult result = messageBoard.enqueue(
      author.c_str(), text.c_str(), static_cast<int64_t>(time(nullptr)), messageId);
  if (result == FfatMessageBoard::SubmitResult::Invalid)
  {
    request->send(400, "application/json", "{\"error\":\"invalid_message\"}");
    return;
  }
  if (result != FfatMessageBoard::SubmitResult::Accepted)
  {
    request->send(500, "application/json", "{\"error\":\"board_unavailable\"}");
    return;
  }

  boardRevision.fetch_add(1, std::memory_order_acq_rel);
  sendBoardEvent("message_changed", messageId);

  char responseBody[48];
  snprintf(responseBody, sizeof(responseBody), "{\"accepted\":true,\"id\":%lu}",
           static_cast<unsigned long>(messageId));
  request->send(202, "application/json", responseBody);
}

void registerRoutes()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    request->send_P(200, "text/html; charset=utf-8", BOARD_PAGE);
  });

  server.on("/api/messages/export.json", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    File file;
    if (!messageBoard.openReaderExclusive(file))
    {
      request->send(500, "application/json", "{\"error\":\"board_unavailable\"}");
      return;
    }

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

    uint32_t token = esp_random();
    if (token == 0)
    {
      token = 1;
    }
    adminSessionToken.store(token, std::memory_order_release);
    AsyncWebServerResponse* response = request->beginResponse(
        200, "application/json", "{\"authenticated\":true}");
    response->addHeader("Set-Cookie", "aredn_admin=" + String(token, HEX) +
        "; Max-Age=3600; HttpOnly; SameSite=Strict");
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
    request->_tempObject = nullptr;
    delete state;
    request->send(failed ? 400 : 200,
                  "application/json",
                    failed
                      ? "{\"error\":\"import_failed\"}"
                      : "{\"imported\":true}");
  }, nullptr, [](AsyncWebServerRequest* request, uint8_t* data,
                 size_t length, size_t index, size_t total)
  {
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
          boardRevision.fetch_add(1, std::memory_order_acq_rel);
          sendBoardEvent("board_changed");
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
      boardRevision.fetch_add(1, std::memory_order_acq_rel);
      sendBoardEvent("board_changed");
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
      boardRevision.fetch_add(1, std::memory_order_acq_rel);
      sendBoardEvent("board_changed");
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
    const bool updated = messageBoard.update(id, name.c_str(), text.c_str());
    if (updated)
    {
      boardRevision.fetch_add(1, std::memory_order_acq_rel);
      sendBoardEvent("board_changed");
    }
    request->send(updated ? 200 : 404,
                  "application/json",
            updated
                      ? "{\"updated\":true}"
                      : "{\"error\":\"message_not_found\"}");
  });

  server.on("/api/messages", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    File file;
    if (!messageBoard.openReaderExclusive(file))
    {
      request->send(500, "application/json", "{\"error\":\"board_unavailable\"}");
      return;
    }

    const bool clockValid = isDeviceTimeValid();
    const time_t currentEpoch = clockValid ? time(nullptr) : 0;
    uint32_t limit = 50;
    if (request->hasParam("all"))
    {
      limit = 0;
    }
    else if (request->hasParam("limit"))
    {
      const long requestedLimit = request->getParam("limit")->value().toInt();
      limit = requestedLimit > 0 ? static_cast<uint32_t>(requestedLimit) : 50;
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
    bool hasMore = false;
    AsyncResponseStream* response = request->beginResponseStream("application/json");
    response->printf("{\"revision\":%lu,\"clock_valid\":%s,\"clock_source\":\"%s\",\"clock_epoch\":%lld,\"messages\":[",
                     static_cast<unsigned long>(boardRevision.load(std::memory_order_acquire)),
                     clockValid ? "true" : "false",
                     deviceTimeSourceName(),
                     static_cast<long long>(currentEpoch));
    uint32_t count = 0;
    writeMessageItems(*response, file, count, false, true, limit, beforeId, afterId, &hasMore,
              searchTerm.isEmpty() ? nullptr : &searchTerm);
    response->printf("],\"has_more\":%s,\"pending\":%lu,\"message_count\":%lu,\"used_bytes\":%lu,\"total_bytes\":%lu}",
             hasMore ? "true" : "false",
                     static_cast<unsigned long>(messageBoard.pending()),
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

    const struct timeval manualTime = {static_cast<time_t>(parsedEpoch), 0};
    if (settimeofday(&manualTime, nullptr) != 0)
    {
      request->send(500, "application/json", "{\"error\":\"clock_update_failed\"}");
      return;
    }

    deviceTimeSource.store(TIME_SOURCE_MANUAL, std::memory_order_release);
    request->send(200, "application/json", "{\"valid\":true,\"source\":\"manual\"}");
  });

  server.on("/api/messages", HTTP_POST, handleSubmitMessage);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request)
  {
    const bool connected = WiFi.status() == WL_CONNECTED;
    const String ipAddress = connected ? WiFi.localIP().toString() : "0.0.0.0";
    const int rssi = connected ? WiFi.RSSI() : 0;
    char response[320];

    snprintf(
        response,
        sizeof(response),
      "{\"service\":\"aredn-service\",\"status\":\"ok\","
      "\"queue_mode\":\"ffat\",\"queue_persistent\":true,"
        "\"board_ready\":%s,"
        "\"queue_pending\":%lu,\"message_count\":%lu,"
        "\"storage_used_bytes\":%lu,\"storage_total_bytes\":%lu,"
        "\"uptime_ms\":%lu,\"free_heap\":%lu,\"wifi_connected\":%s,"
        "\"ip\":\"%s\",\"rssi_dbm\":%d}",
      messageBoard.ready() ? "true" : "false",
      static_cast<unsigned long>(messageBoard.pending()),
      static_cast<unsigned long>(messageBoard.messageCount()),
      static_cast<unsigned long>(messageBoard.usedBytes()),
      static_cast<unsigned long>(messageBoard.totalBytes()),
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
  events.onConnect([](AsyncEventSourceClient* client)
  {
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"revision\":%lu}",
             static_cast<unsigned long>(boardRevision.load(std::memory_order_acquire)));
    client->send(payload, "sync", millis());
  });
  server.addHandler(&events);
}
}

void setup()
{
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
  Serial.printf("ESP reset reason: %d\n", static_cast<int>(esp_reset_reason()));
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

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  Serial.printf("Wi-Fi setup: mode=STA | SSID=\"%s\" | sleep=off | auto-reconnect=on\n",
                WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  sntp_set_time_sync_notification_cb(onNetworkTimeSync);
  sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  Serial.println("SNTP time synchronization started; checking every 15 minutes");
  Serial.printf("Wi-Fi connection started (status %d)\n", static_cast<int>(WiFi.status()));

  Serial.println("Waiting for AREDN WLAN connection...");
  const uint32_t wifiWaitStart = millis();
  constexpr uint32_t WIFI_STARTUP_TIMEOUT_MS = 30000;
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiWaitStart < WIFI_STARTUP_TIMEOUT_MS)
  {
    delay(250);
  }

  registerRoutes();
  server.begin();
  Serial.println("Async HTTP server started on port 80");
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Wi-Fi not connected during startup; continuing with reconnect attempts");
    return;
  }
  const String ipAddress = WiFi.localIP().toString();
  const String gatewayAddress = WiFi.gatewayIP().toString();
  const String subnetAddress = WiFi.subnetMask().toString();
  const String dnsAddress = WiFi.dnsIP().toString();
  Serial.printf("Wi-Fi connected: SSID=\"%s\" | IP=%s | gateway=%s | subnet=%s | DNS=%s | RSSI=%d dBm\n",
                WIFI_SSID,
                ipAddress.c_str(),
                gatewayAddress.c_str(),
                subnetAddress.c_str(),
                dnsAddress.c_str(),
                WiFi.RSSI());
  Serial.printf("Web server: http://%s/\n", ipAddress.c_str());
  wasConnected = true;
}

void loop()
{
  messageBoard.processPending();

  const uint32_t now = millis();
  if (now - lastEventHeartbeat >= 15000)
  {
    lastEventHeartbeat = now;
    events.send("", "heartbeat", now);
  }
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (displayAvailable)
  {
    const uint32_t currentRevision = boardRevision.load(std::memory_order_acquire);
    const bool boardChanged = currentRevision != displayedBoardRevision;
    if (boardChanged)
    {
      displayedBoardRevision = currentRevision;
      refreshLatestDisplayMessage();
    }
    if (boardChanged || now - lastDisplayUpdate >= OLED_PAGE_INTERVAL_MS)
    {
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
    Serial.printf("Status: time %s | heap %lu KB | Wi-Fi %s | RSSI %d dBm | messages %lu | pending %u\n",
                  clockTime,
                  static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
                  connected ? "connected" : "disconnected",
                  connected ? WiFi.RSSI() : 0,
                  static_cast<unsigned long>(messageBoard.messageCount()),
                  static_cast<unsigned int>(messageBoard.pending()));
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

  if (!connected && now - lastReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS)
  {
    lastReconnectAttempt = now;
    Serial.printf("Retrying AREDN WLAN connection (status %d)\n",
                  static_cast<int>(WiFi.status()));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  delay(10);
}