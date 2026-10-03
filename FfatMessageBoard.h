#pragma once

#include <Arduino.h>
#include <FFat.h>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

class FfatMessageBoard
{
public:
  // The live file is append-only; temporary paths support atomic-style repair and replacement.
  static constexpr const char* FILE_PATH = "/messages.dat";
  static constexpr const char* REPAIR_PATH = "/messages.repair";
  static constexpr const char* BACKUP_PATH = "/messages.backup";
  static constexpr const char* IMPORT_PATH = "/messages.import";
  static constexpr size_t MAX_NAME_LENGTH = 32;
  static constexpr size_t MAX_MESSAGE_LENGTH = 1024;
  static constexpr size_t MAX_INDEXED_MESSAGES = 4096;

  // Public, null-terminated view of one fixed-size record on the FFat volume.
  struct Message
  {
    uint32_t id;
    int64_t createdAtEpoch;
    uint8_t priority;
    char name[MAX_NAME_LENGTH + 1];
    char text[MAX_MESSAGE_LENGTH + 1];
  };

  enum class Priority : uint8_t
  {
    Green,
    Orange,
    Red
  };

  enum class SubmitResult
  {
    Accepted,
    Invalid,
    Full,
    Unavailable
  };

  // Mount FFat, allocate the bounded index, and repair valid data during startup.
  bool begin()
  {
    messageCount_ = 0;
    nextId_ = 1;
    if (mutex_ == nullptr)
    {
      mutex_ = xSemaphoreCreateMutex();
    }
    if (mutex_ == nullptr || !FFat.begin(false))
    {
      return false;
    }
    if (index_ == nullptr)
    {
      const size_t storageCapacity = FFat.totalBytes() / sizeof(DiskRecord);
      indexCapacity_ = storageCapacity < MAX_INDEXED_MESSAGES
          ? storageCapacity
          : MAX_INDEXED_MESSAGES;
      index_ = indexCapacity_ == 0 ? nullptr : new (std::nothrow) IndexEntry[indexCapacity_];
      if (index_ == nullptr)
      {
        return false;
      }
    }

    if (!FFat.exists(FILE_PATH) && FFat.exists(BACKUP_PATH))
    {
      if (!FFat.rename(BACKUP_PATH, FILE_PATH))
      {
        return false;
      }
    }
    else if (FFat.exists(FILE_PATH) && FFat.exists(BACKUP_PATH))
    {
      FFat.remove(BACKUP_PATH);
    }

    File file = FFat.open(FILE_PATH, FILE_READ);
    if (!file)
    {
      file = FFat.open(FILE_PATH, FILE_WRITE);
      const bool created = file && writeHeader(file);
      if (!created)
      {
        if (file) file.close();
        return false;
      }
      file.close();
      return true;
    }

    if (file.size() == 0)
    {
      file.close();
      file = FFat.open(FILE_PATH, FILE_WRITE);
      const bool initialized = file && writeHeader(file);
      if (file) file.close();
      return initialized;
    }

    StorageHeader header{};
    file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    if (!isCurrentHeader(header))
    {
      file.close();
      FFat.remove(FILE_PATH);
      file = FFat.open(FILE_PATH, FILE_WRITE);
      const bool initialized = file && writeHeader(file);
      if (file)
      {
        file.close();
      }
      if (!initialized)
      {
        FFat.remove(FILE_PATH);
        return false;
      }
      return true;
    }

    const size_t recordSize = sizeof(DiskRecord);
    const size_t dataSize = file.size() - header.headerSize;
    const size_t validSize = header.headerSize + (dataSize / recordSize) * recordSize;
    uint32_t highestId = 0;
    size_t offset = header.headerSize;
    file.seek(offset);
    DiskRecord record{};
    // Rebuild the in-memory index only from complete, checksum-valid records.
    while (offset + recordSize <= validSize && file.read(
               reinterpret_cast<uint8_t*>(&record), recordSize) == recordSize)
    {
      if (!isValid(record))
      {
        break;
      }
      if (!appendIndexEntry(record.id, offset))
      {
        file.close();
        return false;
      }
      highestId = record.id > highestId ? record.id : highestId;
      ++messageCount_;
      offset += recordSize;
    }
    const bool needsRepair = offset != file.size() || offset != validSize;
    file.close();

    if (needsRepair)
    {
      // Preserve the valid prefix and replace a torn/corrupt tail on the next boot.
      File source = FFat.open(FILE_PATH, FILE_READ);
      File repairFile = FFat.open(REPAIR_PATH, FILE_WRITE);
      bool repaired = source && repairFile && writeHeader(repairFile);
      size_t copied = header.headerSize;
      if (source)
      {
        source.seek(header.headerSize);
      }
      while (repaired && copied < offset)
      {
        const size_t chunkSize = offset - copied < sizeof(DiskRecord)
            ? offset - copied
            : sizeof(DiskRecord);
        uint8_t buffer[sizeof(DiskRecord)];
        repaired = source.read(buffer, chunkSize) == chunkSize &&
                   repairFile.write(buffer, chunkSize) == chunkSize;
        if (repaired)
        {
          copied += chunkSize;
        }
      }
      if (repaired)
      {
        repairFile.flush();
      }
      if (source)
      {
        source.close();
      }
      if (repairFile)
      {
        repairFile.close();
      }
      if (repaired && copied == offset)
      {
        repaired = installRepairFile();
      }
      else
      {
        FFat.remove(REPAIR_PATH);
      }
      if (!repaired)
      {
        return false;
      }
    }

    nextId_ = highestId == UINT32_MAX ? 1 : highestId + 1;
    return true;
  }

  bool ready() const
  {
    return mutex_ != nullptr && index_ != nullptr &&
           indexCapacity_ > 0 && FFat.totalBytes() > 0;
  }

  // Report storage/index exhaustion separately from invalid input and unavailable storage.
  // Append one validated record and update the sorted lookup index under the storage mutex.
  SubmitResult enqueue(const char* name,
                       const char* text,
                       int64_t createdAtEpoch,
                       Priority priority,
                       uint32_t& id)
  {
    if (!ready() || importInProgress_)
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

    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return SubmitResult::Unavailable;
    }
    if (messageCount_ >= indexCapacity_)
    {
      xSemaphoreGive(mutex_);
      return SubmitResult::Full;
    }

    DiskRecord record{};
    record.magic = RECORD_MAGIC;
    record.id = nextId_;
    record.createdAtEpoch = createdAtEpoch;
    record.priority = static_cast<uint8_t>(priority);
    record.nameLength = static_cast<uint8_t>(nameLength);
    record.textLength = static_cast<uint16_t>(textLength);
    memcpy(record.name, name, nameLength);
    memcpy(record.text, text, textLength);
    record.checksum = checksum(record);

    appendFile_.close();
    appendFile_ = FFat.open(FILE_PATH, FILE_APPEND);
    const size_t offset = appendFile_ ? appendFile_.size() : 0;
    const bool written = appendFile_ &&
        appendFile_.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
    if (written)
    {
      appendFile_.flush();
    }
    appendFile_.close();
    if (!written || !appendIndexEntry(record.id, offset))
    {
      xSemaphoreGive(mutex_);
      return SubmitResult::Unavailable;
    }

    id = record.id;
    nextId_ = nextId_ == UINT32_MAX ? 1 : nextId_ + 1;
    ++messageCount_;
    xSemaphoreGive(mutex_);
    return SubmitResult::Accepted;
  }

  // Replace the database with an empty, valid file rather than truncating the live file in place.
  bool reset()
  {
    if (!ready() || importInProgress_ || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }

    appendFile_.close();
    File replacement = FFat.open(REPAIR_PATH, FILE_WRITE);
    bool success = replacement && writeHeader(replacement);
    if (success)
    {
      replacement.flush();
    }
    if (replacement)
    {
      replacement.close();
    }
    if (success)
    {
      success = installRepairFile();
    }
    if (success)
    {
      clearIndex();
      nextId_ = 1;
    }
    xSemaphoreGive(mutex_);
    return success;
  }

  bool update(uint32_t id, const char* name, const char* text, Priority priority)
  {
    return rewrite(id, name, text, priority, false);
  }

  bool remove(uint32_t id)
  {
    return rewrite(id, nullptr, nullptr, Priority::Green, true);
  }

  bool beginImport(size_t expectedBytes)
  {
    // Upload into a separate file first; live data remains intact until validation succeeds.
    importError_ = "none";
    if (!ready() || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      importError_ = "board_unavailable";
      return false;
    }
    if (importInProgress_)
    {
      importError_ = "already_in_progress";
      xSemaphoreGive(mutex_);
      return false;
    }
    appendFile_.close();
    const size_t freeBytes = FFat.totalBytes() - FFat.usedBytes();
    if (expectedBytes > freeBytes)
    {
      importError_ = "upload_too_large";
      xSemaphoreGive(mutex_);
      return false;
    }
    FFat.remove(IMPORT_PATH);
    FFat.remove(REPAIR_PATH);
    importFile_ = FFat.open(IMPORT_PATH, FILE_WRITE);
    importInProgress_ = static_cast<bool>(importFile_);
    if (!importInProgress_)
    {
      importError_ = "import_file_open_failed";
      FFat.remove(IMPORT_PATH);
    }
    xSemaphoreGive(mutex_);
    return importInProgress_;
  }

  // Accept upload chunks only while an import has been started and its temporary file is open.
  bool appendImportData(const uint8_t* data, size_t length)
  {
    if (data == nullptr || length == 0 ||
        xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }
    if (!importInProgress_)
    {
      xSemaphoreGive(mutex_);
      return false;
    }
    const bool success = importFile_ && importFile_.write(data, length) == length;
    xSemaphoreGive(mutex_);
    return success;
  }

  // Validate and install the staged import; on failure the previous board remains installed.
  bool finishImport()
  {
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      importError_ = "mutex_unavailable";
      return false;
    }
    if (!importInProgress_)
    {
      importError_ = "not_in_progress";
      xSemaphoreGive(mutex_);
      return false;
    }

    importFile_.flush();
    importFile_.close();

    // Parse into a repair file, then swap it in so malformed JSON cannot replace live data.
    FFat.remove(BACKUP_PATH);
    File source = FFat.open(IMPORT_PATH, FILE_READ);
    File replacement = FFat.open(REPAIR_PATH, FILE_WRITE);
    const bool sourceOpened = static_cast<bool>(source);
    const bool replacementOpened = static_cast<bool>(replacement);
    uint32_t importedCount = 0;
    uint32_t highestId = 0;
    bool success = sourceOpened && replacementOpened && writeHeader(replacement) &&
        parseImport(source, replacement, importedCount, highestId);
    if (success)
    {
      replacement.flush();
    }
    if (source)
    {
      source.close();
    }
    if (replacement)
    {
      replacement.close();
    }
    if (success)
    {
      success = installRepairFile();
      if (success)
      {
        FFat.remove(BACKUP_PATH);
        nextId_ = highestId == UINT32_MAX ? 1 : highestId + 1;
        success = rebuildIndex();
        if (!success)
        {
          importError_ = "index_rebuild_failed";
          messageCount_ = importedCount;
        }
      }
    }
    else
    {
      importError_ = !sourceOpened ? "import_file_open_failed"
          : !replacementOpened ? "repair_file_open_failed"
          : "json_parse_failed";
      FFat.remove(REPAIR_PATH);
    }
    FFat.remove(IMPORT_PATH);
    importInProgress_ = false;
    if (success)
    {
      importError_ = "none";
    }
    xSemaphoreGive(mutex_);
    return success;
  }

  const char* importError() const
  {
    return importError_;
  }

  // Discard only the staged upload; the currently installed message file is left untouched.
  void abortImport()
  {
    if (mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE)
    {
      importFile_.close();
      FFat.remove(IMPORT_PATH);
      importInProgress_ = false;
      xSemaphoreGive(mutex_);
    }
  }

  // Counts and storage metrics are snapshots; mutation and file operations use the mutex.
  uint32_t messageCount() const
  {
    return messageCount_;
  }

  // Binary-search the ID-sorted index to resume incremental polling after the last seen message.
  uint32_t firstIndexAfter(uint32_t id) const
  {
    uint32_t lower = 0;
    uint32_t upper = messageCount_;
    while (lower < upper)
    {
      const uint32_t middle = lower + (upper - lower) / 2;
      if (index_[middle].id <= id)
      {
        lower = middle + 1;
      }
      else
      {
        upper = middle;
      }
    }
    return lower;
  }

  size_t usedBytes() const
  {
    return FFat.usedBytes();
  }

  size_t totalBytes() const
  {
    return FFat.totalBytes();
  }

  size_t freeBytes() const
  {
    return FFat.totalBytes() - FFat.usedBytes();
  }

  size_t recordSize() const
  {
    return sizeof(DiskRecord);
  }

  size_t indexCapacity() const
  {
    return indexCapacity_;
  }

  size_t indexBytes() const
  {
    return indexCapacity_ * sizeof(IndexEntry);
  }

  // Open a short-lived reader while protecting the file from concurrent replacement.
  bool openReader(File& file) const
  {
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }
    const bool success = openReaderUnlocked(file);
    if (!success && file)
    {
      file.close();
    }
    xSemaphoreGive(mutex_);
    return success;
  }

  bool openReaderExclusive(File& file) const
  {
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }
    if (!openReaderUnlocked(file))
    {
      if (file)
      {
        file.close();
      }
      xSemaphoreGive(mutex_);
      return false;
    }
    // The caller must close through closeReaderExclusive() to release this lock.
    return true;
  }

  // Pair this with openReaderExclusive(); the lock covers the caller's complete read/stream.
  void closeReaderExclusive(File& file) const
  {
    if (file)
    {
      file.close();
    }
    xSemaphoreGive(mutex_);
  }

  // Sequential reads are used for export; indexed reads support newest-first and paged queries.
  bool readNext(File& file, Message& message) const
  {
    DiskRecord record{};
    if (!file || file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record) ||
        !isValid(record))
    {
      return false;
    }

    message.id = record.id;
    message.createdAtEpoch = record.createdAtEpoch;
    message.priority = record.priority;
    memcpy(message.name, record.name, record.nameLength);
    message.name[record.nameLength] = '\0';
    memcpy(message.text, record.text, record.textLength);
    message.text[record.textLength] = '\0';
    return true;
  }

  // Lock around a single indexed read when the caller does not already hold the board lock.
  bool readAt(File& file, uint32_t index, Message& message) const
  {
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }
    const bool success = readAtUnlocked(file, index, message);
    xSemaphoreGive(mutex_);
    return success;
  }

  // Use only while holding the exclusive reader lock returned by openReaderExclusive().
  bool readAtExclusive(File& file, uint32_t index, Message& message) const
  {
    return readAtUnlocked(file, index, message);
  }

  // Fetch the newest message under one lock so its file and index position stay consistent.
  bool readLatest(Message& message) const
  {
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }

    File file;
    const bool found = messageCount_ != 0 && openReaderUnlocked(file) &&
        readAtUnlocked(file, messageCount_ - 1, message);
    if (file)
    {
      file.close();
    }
    xSemaphoreGive(mutex_);
    return found;
  }

private:
  // Packed structures make on-disk record size stable and permit direct fixed-size reads.
  static constexpr uint32_t RECORD_MAGIC = 0x41524D31;
  static constexpr uint32_t STORAGE_MAGIC = 0x41524631;
  static constexpr uint16_t CURRENT_FORMAT_VERSION = 2;
  static constexpr uint32_t MAX_IMPORTED_MESSAGES = MAX_INDEXED_MESSAGES - 1;

  struct __attribute__((packed)) StorageHeader
  {
    uint32_t magic;
    uint16_t version;
    uint16_t headerSize;
    uint32_t recordSize;
    uint32_t reserved;
  };

  struct __attribute__((packed)) DiskRecord
  {
    uint32_t magic;
    uint32_t id;
    int64_t createdAtEpoch;
    uint8_t priority;
    uint8_t nameLength;
    uint16_t textLength;
    char name[MAX_NAME_LENGTH];
    char text[MAX_MESSAGE_LENGTH];
    uint32_t checksum;
  };

  struct IndexEntry
  {
    uint32_t id;
    size_t offset;
  };

  static void copyRecordToMessage(const DiskRecord& record, Message& message)
  {
    message.id = record.id;
    message.createdAtEpoch = record.createdAtEpoch;
    message.priority = record.priority;
    memcpy(message.name, record.name, record.nameLength);
    message.name[record.nameLength] = '\0';
    memcpy(message.text, record.text, record.textLength);
    message.text[record.textLength] = '\0';
  }

  bool appendIndexEntry(uint32_t id, size_t offset)
  {
    if (index_ == nullptr || messageCount_ >= indexCapacity_)
    {
      return false;
    }

    // Keep entries ordered by ID for firstIndexAfter() binary searches.
    size_t position = messageCount_;
    while (position > 0 && index_[position - 1].id > id)
    {
      index_[position] = index_[position - 1];
      --position;
    }
    index_[position] = {id, offset};
    return true;
  }

  void clearIndex()
  {
    messageCount_ = 0;
  }

  bool rebuildIndex()
  {
    // Reconstruct offsets after replacing the data file; IDs remain sorted for efficient lookup.
    File file;
    if (index_ == nullptr || !openReaderUnlocked(file))
    {
      return false;
    }

    clearIndex();
    DiskRecord record{};
    while (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record))
    {
      const size_t offset = file.position() - sizeof(record);
      if (!isValid(record))
      {
        file.close();
        return false;
      }
      if (!appendIndexEntry(record.id, offset))
      {
        file.close();
        return false;
      }
      ++messageCount_;
    }
    const bool complete = file.position() == file.size();
    file.close();
    return complete;
  }

  static bool writeHeader(File& file)
  {
    // Readers reject files whose layout/version differs from this firmware's packed record format.
    StorageHeader header{
        STORAGE_MAGIC,
        CURRENT_FORMAT_VERSION,
        static_cast<uint16_t>(sizeof(StorageHeader)),
        static_cast<uint32_t>(sizeof(DiskRecord)),
        0};
    return file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);
  }

  bool openReaderUnlocked(File& file) const
  {
    // Caller owns mutex_; validate the header before positioning at the first record.
    file = FFat.open(FILE_PATH, FILE_READ);
    if (!file)
    {
      return false;
    }
    StorageHeader header{};
    return file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
        isCurrentHeader(header) && file.seek(header.headerSize);
  }

  bool readAtUnlocked(File& file, uint32_t index, Message& message) const
  {
    // Caller owns mutex_; offsets come from the validated in-memory index.
    if (!file || index >= messageCount_ || index_[index].id == 0 ||
      !file.seek(index_[index].offset))
      {
        return false;
      }
    DiskRecord record{};
    return file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
      isValid(record) && (copyRecordToMessage(record, message), true);
  }

  static bool isCurrentHeader(const StorageHeader& header)
  {
    return header.magic == STORAGE_MAGIC &&
           header.version == CURRENT_FORMAT_VERSION &&
           header.headerSize == sizeof(StorageHeader) &&
           header.recordSize == sizeof(DiskRecord);
  }

  static uint32_t checksum(const DiskRecord& record)
  {
    // FNV-1a covers every record field before checksum, detecting partial writes and corruption.
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t value = 2166136261UL;
    for (size_t index = 0; index < offsetof(DiskRecord, checksum); ++index)
    {
      value ^= bytes[index];
      value *= 16777619UL;
    }
    return value;
  }

  static bool isValid(const DiskRecord& record)
  {
    // Validate lengths before copying into public buffers, as well as integrity and enum bounds.
    return record.magic == RECORD_MAGIC && record.id != 0 &&
           record.priority <= static_cast<uint8_t>(Priority::Red) &&
           record.nameLength > 0 && record.nameLength <= MAX_NAME_LENGTH &&
           record.textLength > 0 && record.textLength <= MAX_MESSAGE_LENGTH &&
           record.checksum == checksum(record);
  }

  class BufferedReader
  {
  public:
    // A small read-ahead buffer reduces filesystem calls while parsing imported JSON.
    BufferedReader(File& file, uint8_t* buffer) : file_(file), buffer_(buffer) {}

    bool readByte(char& value)
    {
      if (bufferPosition_ == bufferSize_)
      {
        bufferSize_ = file_.read(buffer_, BUFFER_SIZE);
        bufferPosition_ = 0;
        if (bufferSize_ == 0)
        {
          return false;
        }
      }
      value = static_cast<char>(buffer_[bufferPosition_++]);
      return true;
    }

    bool seek(size_t position)
    {
      bufferPosition_ = 0;
      bufferSize_ = 0;
      return file_.seek(position);
    }

    size_t position() const
    {
      return file_.position() - (bufferSize_ - bufferPosition_);
    }

    static constexpr size_t BUFFER_SIZE = 512;

  private:
    File& file_;
    uint8_t* buffer_;
    size_t bufferPosition_ = 0;
    size_t bufferSize_ = 0;
  };

  class BufferedWriter
  {
  public:
    // Batch serialized records to reduce small writes during import, repair, and rewrite.
    BufferedWriter(File& file, uint8_t* buffer) : file_(file), buffer_(buffer) {}

    bool write(const uint8_t* data, size_t length)
    {
      while (length > 0)
      {
        const size_t available = BUFFER_SIZE - bufferSize_;
        const size_t chunkSize = length < available ? length : available;
        memcpy(buffer_ + bufferSize_, data, chunkSize);
        bufferSize_ += chunkSize;
        data += chunkSize;
        length -= chunkSize;
        if (bufferSize_ == BUFFER_SIZE && !flush())
        {
          return false;
        }
      }
      return true;
    }

    bool flush()
    {
      if (bufferSize_ == 0)
      {
        return true;
      }
      if (file_.write(buffer_, bufferSize_) != bufferSize_)
      {
        return false;
      }
      bufferSize_ = 0;
      return true;
    }

    static constexpr size_t BUFFER_SIZE = 2048;

  private:
    File& file_;
    uint8_t* buffer_;
    size_t bufferSize_ = 0;
  };

  static bool readByte(BufferedReader& reader, char& value)
  {
    return reader.readByte(value);
  }

  static bool nextNonWhitespace(BufferedReader& reader, char& value)
  {
    while (readByte(reader, value))
    {
      if (!isspace(static_cast<unsigned char>(value)))
      {
        return true;
      }
    }
    return false;
  }

  static bool expect(BufferedReader& reader, char expected)
  {
    char value = 0;
    return nextNonWhitespace(reader, value) && value == expected;
  }

  static bool readString(BufferedReader& reader, char* destination, size_t capacity)
  {
    // Decode JSON escapes into a bounded buffer; this importer accepts only ASCII \u escapes.
    if (!expect(reader, '"'))
    {
      return false;
    }
    size_t length = 0;
    char value = 0;
    while (readByte(reader, value))
    {
      if (value == '"')
      {
        if (length >= capacity)
        {
          return false;
        }
        destination[length] = '\0';
        return true;
      }
      if (value == '\\')
      {
        if (!readByte(reader, value))
        {
          return false;
        }
        switch (value)
        {
          case '"': case '\\': case '/': break;
          case 'b': value = '\b'; break;
          case 'f': value = '\f'; break;
          case 'n': value = '\n'; break;
          case 'r': value = '\r'; break;
          case 't': value = '\t'; break;
          case 'u':
          {
            uint16_t code = 0;
            for (uint8_t digit = 0; digit < 4; ++digit)
            {
              char hex = 0;
              if (!readByte(reader, hex)) return false;
              code = static_cast<uint16_t>(code << 4);
              if (hex >= '0' && hex <= '9') code = static_cast<uint16_t>(code + hex - '0');
              else if (hex >= 'a' && hex <= 'f') code = static_cast<uint16_t>(code + hex - 'a' + 10);
              else if (hex >= 'A' && hex <= 'F') code = static_cast<uint16_t>(code + hex - 'A' + 10);
              else return false;
            }
            if (code > 0x7f) return false;
            value = static_cast<char>(code);
            break;
          }
          default: return false;
        }
      }
      if (length + 1 >= capacity)
      {
        return false;
      }
      destination[length++] = value;
    }
    return false;
  }

  static bool readUnsigned(BufferedReader& reader, uint32_t& value)
  {
    char digit = 0;
    if (!nextNonWhitespace(reader, digit) || digit < '0' || digit > '9')
    {
      return false;
    }
    value = 0;
    do
    {
      const uint32_t next = value * 10UL + static_cast<uint32_t>(digit - '0');
      if (next < value)
      {
        return false;
      }
      value = next;
    } while (readByte(reader, digit) && digit >= '0' && digit <= '9');
    if (reader.position() > 0)
    {
      reader.seek(reader.position() - 1);
    }
    return true;
  }

  static bool parseTimestamp(const char* value, int64_t& epoch)
  {
    // Convert validated UTC calendar fields directly to epoch seconds without timezone state.
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day,
               &hour, &minute, &second) != 6)
    {
      return false;
    }
    if (year < 1970 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59)
    {
      return false;
    }
    const bool leapYear = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const uint8_t daysInMonth[] = {
        31, static_cast<uint8_t>(leapYear ? 29 : 28), 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31};
    if (day < 1 || day > daysInMonth[month - 1])
    {
      return false;
    }

    int adjustedYear = year - (month <= 2 ? 1 : 0);
    const int era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(adjustedYear - era * 400);
    const unsigned adjustedMonth = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 +
                               static_cast<unsigned>(day - 1);
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t daysSinceEpoch = static_cast<int64_t>(era) * 146097 +
                                   static_cast<int64_t>(dayOfEra) - 719468;
    epoch = ((daysSinceEpoch * 24 + hour) * 60 + minute) * 60 + second;
    return true;
  }

  static bool parsePriority(const char* value, uint8_t& priority)
  {
    if (strcmp(value, "green") == 0)
    {
      priority = static_cast<uint8_t>(Priority::Green);
      return true;
    }
    if (strcmp(value, "orange") == 0)
    {
      priority = static_cast<uint8_t>(Priority::Orange);
      return true;
    }
    if (strcmp(value, "red") == 0)
    {
      priority = static_cast<uint8_t>(Priority::Red);
      return true;
    }
    return false;
  }

  static bool parseImport(File& source, File& replacement,
                          uint32_t& importedCount, uint32_t& highestId)
  {
    // Stream the known export shape with fixed buffers instead of loading JSON into RAM.
    static uint8_t readerBuffer[BufferedReader::BUFFER_SIZE];
    static uint8_t writerBuffer[BufferedWriter::BUFFER_SIZE];
    BufferedReader reader(source, readerBuffer);
    BufferedWriter writer(replacement, writerBuffer);
    char key[32]{};
    char value = 0;
    bool foundMessages = false;
    // Locate the exported messages array while allowing metadata fields before it.
    while (readByte(reader, value))
    {
      if (value == '"')
      {
        reader.seek(reader.position() - 1);
        if (!readString(reader, key, sizeof(key)))
        {
          return false;
        }
        if (strcmp(key, "messages") == 0)
        {
          foundMessages = true;
          break;
        }
      }
    }
    if (!foundMessages || !expect(reader, ':') || !expect(reader, '['))
    {
      return false;
    }
    char next = 0;
    if (!nextNonWhitespace(reader, next))
    {
      return false;
    }
    if (next == ']')
    {
      return writer.flush();
    }
    reader.seek(reader.position() - 1);

    while (true)
    {
      if (!expect(reader, '{') || !expect(reader, '"')) return false;
      reader.seek(reader.position() - 1);
      if (!readString(reader, key, sizeof(key)) || strcmp(key, "id") != 0 ||
        !expect(reader, ':')) return false;
      uint32_t id = 0;
      if (!readUnsigned(reader, id) || id == 0 || !expect(reader, ',') ||
        !expect(reader, '"')) return false;
      reader.seek(reader.position() - 1);
      if (!readString(reader, key, sizeof(key)) || strcmp(key, "created_at") != 0 ||
        !expect(reader, ':')) return false;
      char timestamp[32]{};
      if (!readString(reader, timestamp, sizeof(timestamp)) || !expect(reader, ',') ||
        !expect(reader, '"')) return false;
      reader.seek(reader.position() - 1);
      char name[MAX_NAME_LENGTH + 1]{};
      if (!readString(reader, key, sizeof(key)) || strcmp(key, "name") != 0 ||
        !expect(reader, ':') || !readString(reader, name, sizeof(name)) ||
        !expect(reader, ',') || !expect(reader, '"')) return false;
      reader.seek(reader.position() - 1);
      char text[MAX_MESSAGE_LENGTH + 1]{};
      if (!readString(reader, key, sizeof(key)) || strcmp(key, "text") != 0 ||
        !expect(reader, ':') || !readString(reader, text, sizeof(text))) return false;

      uint8_t priority = static_cast<uint8_t>(Priority::Green);
      if (!nextNonWhitespace(reader, next)) return false;
      if (next != '}')
      {
        if (next != ',' || !expect(reader, '"')) return false;
        reader.seek(reader.position() - 1);
        char priorityValue[8]{};
        if (!readString(reader, key, sizeof(key)) || strcmp(key, "priority") != 0 ||
            !expect(reader, ':') || !readString(reader, priorityValue, sizeof(priorityValue)) ||
            !parsePriority(priorityValue, priority) || !expect(reader, '}')) return false;
      }

      int64_t epoch = 0;
      if (!parseTimestamp(timestamp, epoch)) return false;
      DiskRecord record{};
      record.magic = RECORD_MAGIC;
      record.id = id;
      record.createdAtEpoch = epoch;
      record.priority = priority;
      record.nameLength = static_cast<uint8_t>(strlen(name));
      record.textLength = static_cast<uint16_t>(strlen(text));
      if (record.nameLength == 0 || record.textLength == 0)
      {
        return false;
      }
      memcpy(record.name, name, record.nameLength);
      memcpy(record.text, text, record.textLength);
      record.checksum = checksum(record);
      const bool keepRecord = importedCount < MAX_IMPORTED_MESSAGES;
      if (keepRecord &&
          !writer.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)))
      {
        return false;
      }
      highestId = id > highestId ? id : highestId;
      if (keepRecord)
      {
        ++importedCount;
      }
      if ((importedCount & 0x1f) == 0)
      {
        esp_task_wdt_reset();
        delay(0);
      }

      if (!nextNonWhitespace(reader, next)) return false;
      if (next == ']') return writer.flush();
      if (next != ',') return false;
    }
  }

  bool rewrite(uint32_t targetId, const char* name, const char* text,
               Priority priority, bool removeMessage)
  {
    if (!ready() || targetId == 0 || (!removeMessage && (name == nullptr || text == nullptr)) ||
        xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE)
    {
      return false;
    }

    const size_t nameLength = removeMessage ? 0 : strlen(name);
    const size_t textLength = removeMessage ? 0 : strlen(text);
    const bool validReplacement = removeMessage ||
        (nameLength > 0 && nameLength <= MAX_NAME_LENGTH &&
         textLength > 0 && textLength <= MAX_MESSAGE_LENGTH);
    if (!validReplacement)
    {
      xSemaphoreGive(mutex_);
      return false;
    }

    // Edits and deletes rewrite through a temporary file because records have fixed size.
    appendFile_.close();
    File source = FFat.open(FILE_PATH, FILE_READ);
    File replacement = FFat.open(REPAIR_PATH, FILE_WRITE);
    StorageHeader header{};
    bool success = source && replacement && writeHeader(replacement) &&
      source.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
      isCurrentHeader(header);
    const size_t sourceSize = source ? source.size() : 0;
    const size_t dataSize = sourceSize >= sizeof(StorageHeader)
        ? sourceSize - sizeof(StorageHeader)
        : 0;
    const uint32_t expectedRecords = dataSize % sizeof(DiskRecord) == 0
        ? static_cast<uint32_t>(dataSize / sizeof(DiskRecord))
        : 0;
    if (success && (sourceSize < sizeof(StorageHeader) ||
                    dataSize % sizeof(DiskRecord) != 0))
    {
      success = false;
    }
    bool found = false;
    uint32_t recordsRead = 0;
    static uint8_t rewriteBuffer[BufferedWriter::BUFFER_SIZE];
    BufferedWriter writer(replacement, rewriteBuffer);
    DiskRecord record{};
    while (success && recordsRead < expectedRecords)
    {
      if (source.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record) ||
          !isValid(record))
      {
        success = false;
        break;
      }
      ++recordsRead;
      if (record.id == targetId)
      {
        found = true;
        if (!removeMessage)
        {
          record.priority = static_cast<uint8_t>(priority);
          record.nameLength = static_cast<uint8_t>(nameLength);
          record.textLength = static_cast<uint16_t>(textLength);
          memset(record.name, 0, sizeof(record.name));
          memset(record.text, 0, sizeof(record.text));
          memcpy(record.name, name, nameLength);
          memcpy(record.text, text, textLength);
          record.checksum = checksum(record);
            success = writer.write(
              reinterpret_cast<const uint8_t*>(&record), sizeof(record));
        }
      }
      else
      {
        success = writer.write(
          reinterpret_cast<const uint8_t*>(&record), sizeof(record));
      }
      if ((recordsRead & 0x1f) == 0)
      {
        esp_task_wdt_reset();
        delay(0);
      }
    }
      if (success && recordsRead != expectedRecords)
    {
      success = false;
    }
    if (success && !found)
    {
      success = false;
    }
    if (success)
    {
      success = writer.flush();
      if (success)
      {
        replacement.flush();
      }
    }
    if (source)
    {
      source.close();
    }
    if (replacement)
    {
      replacement.close();
    }

    if (success)
    {
      success = installRepairFile();
      if (success)
      {
        messageCount_ = removeMessage ? messageCount_ - 1 : messageCount_;
        success = rebuildIndex();
      }
    }
    else
    {
      FFat.remove(REPAIR_PATH);
    }

    xSemaphoreGive(mutex_);
    return success;
  }

  static bool installRepairFile()
  {
    // Keep a backup until the replacement is installed so a failed rename can be rolled back.
    if (!FFat.exists(FILE_PATH))
    {
      return FFat.rename(REPAIR_PATH, FILE_PATH);
    }
    FFat.remove(BACKUP_PATH);
    if (!FFat.rename(FILE_PATH, BACKUP_PATH))
    {
      FFat.remove(REPAIR_PATH);
      return false;
    }
    if (FFat.rename(REPAIR_PATH, FILE_PATH))
    {
      FFat.remove(BACKUP_PATH);
      return true;
    }
    FFat.remove(FILE_PATH);
    FFat.rename(BACKUP_PATH, FILE_PATH);
    FFat.remove(REPAIR_PATH);
    return false;
  }

  SemaphoreHandle_t mutex_ = nullptr;
  File importFile_;
  File appendFile_;
  IndexEntry* index_ = nullptr;
  size_t indexCapacity_ = 0;
  uint32_t nextId_ = 1;
  uint32_t messageCount_ = 0;
  bool importInProgress_ = false;
  const char* importError_ = "none";
};
