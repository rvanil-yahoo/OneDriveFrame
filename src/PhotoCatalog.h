#pragma once

#include <Arduino.h>

struct sqlite3;
struct sqlite3_stmt;

namespace odf {

class PhotoCatalog {
 public:
  struct RemotePhotoMetadata {
    String itemId;
    String filename;
    String localPath;
    String modifiedUtc;
    String etag;
    String mimeType;
    int32_t width = -1;
    int32_t height = -1;
    String orientation;
    uint32_t sizeBytes = 0;
  };

  struct DownloadPhoto {
    String itemId;
    String filename;
    String modifiedUtc;
    String etag;
    uint32_t sizeBytes = 0;
    int64_t rowId = 0;
  };

  PhotoCatalog() = default;
  ~PhotoCatalog();
  PhotoCatalog(const PhotoCatalog&) = delete;
  PhotoCatalog& operator=(const PhotoCatalog&) = delete;

  bool begin();
  void end();
  bool beginTransaction();
  bool commitTransaction();
  bool rollbackTransaction();
  bool reconcileWithStorage();
  bool beginRemoteSync();
  bool upsertRemotePhoto(const RemotePhotoMetadata& metadata);
  bool loadDownloadBatch(const String& afterModifiedUtc, int64_t afterRowId, DownloadPhoto* photos, size_t capacity,
                         size_t& count);
  bool nextSlideshowPhoto(const String& afterPath, String& path, size_t& count);
  bool markDownloaded(const String& itemId, const String& filename, const String& localPath, uint32_t sizeBytes);
  bool removeFilesNotInRemoteCatalog(size_t& deletedCount, uint64_t& freedBytes);
  bool pruneOldestLocalFilesToQuota(uint64_t quotaBytes, size_t& deletedCount, uint64_t& deletedBytes);
  uint64_t totalLocalBytes();

 private:
  bool openDatabase();
  bool createSchema();
  bool execute(const char* sql);
  bool prepareStatements();
  void finalizeStatements();
  void closeDatabase();
  bool recoverFromIoError(int rc);
  bool stepUpsertRemote(sqlite3_stmt* statement, const RemotePhotoMetadata& metadata, bool matchItemId);
  bool stepMarkDownloaded(sqlite3_stmt* statement, const String& itemId, const String& filename,
                          const String& localPath, uint32_t sizeBytes, bool matchItemId);

  bool initialized_ = false;
  bool transactionActive_ = false;
  sqlite3* database_ = nullptr;
  sqlite3_stmt* reconcileUpdate_ = nullptr;
  sqlite3_stmt* reconcileInsert_ = nullptr;
  sqlite3_stmt* remoteUpdate_ = nullptr;
  sqlite3_stmt* remoteAdopt_ = nullptr;
  sqlite3_stmt* remoteInsert_ = nullptr;
  sqlite3_stmt* downloadUpdate_ = nullptr;
  sqlite3_stmt* downloadAdopt_ = nullptr;
  sqlite3_stmt* downloadInsert_ = nullptr;
  sqlite3_stmt* markMissing_ = nullptr;
};

}  // namespace odf
