#pragma once

#include <Arduino.h>

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

  bool begin();
  bool reconcileWithStorage();
  bool beginRemoteSync();
  bool upsertRemotePhoto(const RemotePhotoMetadata& metadata);
  bool markDownloaded(const String& itemId, const String& filename, const String& localPath, uint32_t sizeBytes);
  bool removeFilesNotInRemoteCatalog(size_t& deletedCount, uint64_t& freedBytes);
  bool pruneOldestLocalFilesToQuota(uint64_t quotaBytes, size_t& deletedCount, uint64_t& deletedBytes);
  uint64_t totalLocalBytes();

 private:
  bool openDatabase();
  bool createSchema();
  bool execute(const char* sql);

  bool initialized_ = false;
};

}  // namespace odf
