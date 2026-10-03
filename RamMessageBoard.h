#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstring>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

class RamMessageBoard
{
public:
  static constexpr uint8_t HISTORY_CAPACITY = 32;
  static constexpr uint8_t PENDING_CAPACITY = 8;
  static constexpr size_t MAX_NAME_LENGTH = 32;
  static constexpr size_t MAX_MESSAGE_LENGTH = 192;

  enum class TimeSource : uint8_t
  {
    Network,
    Manual
  };

  struct Message
  {
    uint32_t id;
    int64_t createdAtEpoch;
    TimeSource timeSource;
    char name[MAX_NAME_LENGTH + 1];
    char text[MAX_MESSAGE_LENGTH + 1];
  };

  enum class SubmitResult
  {
    Accepted,
    Full,
    Invalid,
    Unavailable
  };

  bool begin()
  {
    if (queue_ == nullptr)
    {
      queue_ = xQueueCreate(PENDING_CAPACITY, sizeof(Message));
    }
    if (mutex_ == nullptr)
    {
      mutex_ = xSemaphoreCreateMutex();
    }
    return ready();
  }

  bool ready() const
  {
    return queue_ != nullptr && mutex_ != nullptr;
  }

  SubmitResult enqueue(const char* name,
                       const char* text,
                       int64_t createdAtEpoch,
                       TimeSource timeSource,
                       uint32_t& id)
  {
    if (!ready())
    {
      return SubmitResult::Unavailable;
    }
    if (name == nullptr || text == nullptr)
    {
      return SubmitResult::Invalid;
    }

    const size_t nameLength = strlen(name);
    const size_t textLength = strlen(text);
    if (nameLength == 0 || nameLength > MAX_NAME_LENGTH ||
        textLength == 0 || textLength > MAX_MESSAGE_LENGTH)
    {
      return SubmitResult::Invalid;
    }

    Message message{};
    message.id = nextId_.fetch_add(1, std::memory_order_relaxed);
    message.createdAtEpoch = createdAtEpoch;
    message.timeSource = timeSource;
    memcpy(message.name, name, nameLength + 1);
    memcpy(message.text, text, textLength + 1);

    if (xQueueSend(queue_, &message, 0) != pdPASS)
    {
      return SubmitResult::Full;
    }

    id = message.id;
    return SubmitResult::Accepted;
  }

  uint8_t processPending()
  {
    if (!ready())
    {
      return 0;
    }

    uint8_t processed = 0;
    for (;;)
    {
      if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
      {
        break;
      }

      Message message{};
      if (xQueueReceive(queue_, &message, 0) != pdPASS)
      {
        xSemaphoreGive(mutex_);
        break;
      }

      history_[nextHistorySlot_] = message;
      nextHistorySlot_ = static_cast<uint8_t>((nextHistorySlot_ + 1) % HISTORY_CAPACITY);
      if (historyCount_ < HISTORY_CAPACITY)
      {
        ++historyCount_;
      }
      xSemaphoreGive(mutex_);
      ++processed;
    }

    return processed;
  }

  bool copyLatest(Message* destination, uint8_t& count) const
  {
    if (!ready() || destination == nullptr ||
        xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }

    count = historyCount_;
    for (uint8_t index = 0; index < count; ++index)
    {
      const uint8_t slot = static_cast<uint8_t>(
          (nextHistorySlot_ + HISTORY_CAPACITY - 1 - index) % HISTORY_CAPACITY);
      destination[index] = history_[slot];
    }

    xSemaphoreGive(mutex_);
    return true;
  }

  uint8_t pending() const
  {
    return queue_ == nullptr ? 0 : static_cast<uint8_t>(uxQueueMessagesWaiting(queue_));
  }

private:
  QueueHandle_t queue_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  std::atomic<uint32_t> nextId_{1};
  Message history_[HISTORY_CAPACITY]{};
  uint8_t historyCount_ = 0;
  uint8_t nextHistorySlot_ = 0;
};