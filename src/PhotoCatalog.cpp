#include "PhotoCatalog.h"

#include <SD.h>

extern "C" {
#include <sqlite3.h>
}

#include "FrameConfig.h"

namespace odf {

namespace {

constexpr const char* kCatalogPath = "/sd/catalog.db";

bool bindNullableText(sqlite3_stmt* stmt, int index, const String& value) {
  if (value.isEmpty()) {
    return sqlite3_bind_null(stmt, index) == SQLITE_OK;
  }
  return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

bool hasColumn(sqlite3* db, const char* tableName, const char* columnName) {
  String sql = "PRAGMA table_info(" + String(tableName) + ");";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  bool found = false;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (name != nullptr && strcmp(name, columnName) == 0) {
      found = true;
      break;
    }
  }

  sqlite3_finalize(stmt);
  return found;
}

void logSqliteError(const char* context, sqlite3* db, int rc) {
  Serial.print("[Catalog] ");
  Serial.print(context);
  Serial.print(" failed (");
  Serial.print(rc);
  Serial.print("): ");
  if (db != nullptr) {
    Serial.println(sqlite3_errmsg(db));
  } else {
    Serial.println(sqlite3_errstr(rc));
  }
}

}  // namespace

bool PhotoCatalog::openDatabase() {
  Serial.print("[Catalog] Opening DB: ");
  Serial.println(kCatalogPath);
  sqlite3* db = nullptr;
  const int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    Serial.print("[Catalog] Failed opening DB: ");
    Serial.println(sqlite3_errstr(rc));
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }
  sqlite3_close(db);
  return true;
}

bool PhotoCatalog::execute(const char* sql) {
  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    Serial.print("[Catalog] sqlite3_open failed: ");
    Serial.println(sqlite3_errstr(rc));
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  char* errorMessage = nullptr;
  rc = sqlite3_exec(db, sql, nullptr, nullptr, &errorMessage);
  if (rc != SQLITE_OK) {
    Serial.print("[Catalog] sqlite3_exec failed: ");
    if (errorMessage != nullptr) {
      Serial.println(errorMessage);
      sqlite3_free(errorMessage);
    } else {
      Serial.println(sqlite3_errstr(rc));
    }
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  return true;
}

bool PhotoCatalog::createSchema() {
  Serial.println("[Catalog] Creating/verifying schema...");
  static constexpr const char* kSchemaSql =
      "CREATE TABLE IF NOT EXISTS photos ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "item_id TEXT UNIQUE,"
      "filename TEXT NOT NULL,"
      "local_path TEXT NOT NULL UNIQUE,"
      "size_bytes INTEGER NOT NULL DEFAULT 0,"
      "width INTEGER,"
      "height INTEGER,"
      "orientation TEXT,"
      "last_modified_utc TEXT,"
      "etag TEXT,"
      "mime_type TEXT,"
      "remote_present INTEGER NOT NULL DEFAULT 0,"
      "local_present INTEGER NOT NULL DEFAULT 0,"
      "downloaded_utc TEXT"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_photos_item_id ON photos(item_id);"
      "CREATE INDEX IF NOT EXISTS idx_photos_last_modified ON photos(last_modified_utc);"
      "CREATE INDEX IF NOT EXISTS idx_photos_local_present ON photos(local_present);"
      "CREATE INDEX IF NOT EXISTS idx_photos_size ON photos(size_bytes);";
  if (!execute(kSchemaSql)) {
    Serial.println("[Catalog] Base schema creation failed.");
    return false;
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    logSqliteError("open for schema migration", db, rc);
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  if (!hasColumn(db, "photos", "remote_present")) {
    Serial.println("[Catalog] Applying schema migration: add remote_present.");
    char* errorMessage = nullptr;
    rc = sqlite3_exec(db, "ALTER TABLE photos ADD COLUMN remote_present INTEGER NOT NULL DEFAULT 0;", nullptr, nullptr,
                      &errorMessage);
    if (rc != SQLITE_OK) {
      bool duplicateColumn = false;
      if (errorMessage != nullptr && strstr(errorMessage, "duplicate column name") != nullptr) {
        duplicateColumn = true;
      }
      if (!duplicateColumn) {
        logSqliteError("alter table add remote_present", db, rc);
        if (errorMessage != nullptr) {
          sqlite3_free(errorMessage);
        }
        sqlite3_close(db);
        return false;
      }
      if (errorMessage != nullptr) {
        sqlite3_free(errorMessage);
      }
    }
  }

  sqlite3_close(db);
  return true;
}

bool PhotoCatalog::begin() {
  if (initialized_) {
    return true;
  }

  Serial.println("[Catalog] begin() start");
  if (!openDatabase()) {
    Serial.println("[Catalog] begin() failed in openDatabase().");
    return false;
  }

  if (!createSchema()) {
    Serial.println("[Catalog] begin() failed in createSchema().");
    return false;
  }

  initialized_ = true;
  Serial.println("[Catalog] SQLite catalog ready.");
  return true;
}

bool PhotoCatalog::reconcileWithStorage() {
  if (!initialized_ && !begin()) {
    return false;
  }

  if (!execute("UPDATE photos SET local_present = 0;")) {
    return false;
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    Serial.print("[Catalog] sqlite3_open failed: ");
    Serial.println(sqlite3_errstr(rc));
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  sqlite3_stmt* updateStmt = nullptr;
  sqlite3_stmt* insertStmt = nullptr;

  const char* updateSql = "UPDATE photos SET local_present=1, size_bytes=? WHERE local_path=?;";
  rc = sqlite3_prepare_v2(db, updateSql, -1, &updateStmt, nullptr);
  if (rc != SQLITE_OK) {
    Serial.println("[Catalog] Failed to prepare update statement.");
    sqlite3_close(db);
    return false;
  }

  const char* insertSql =
      "INSERT INTO photos(item_id, filename, local_path, size_bytes, local_present, downloaded_utc) "
      "VALUES(NULL, ?, ?, ?, 1, datetime('now'));";
  rc = sqlite3_prepare_v2(db, insertSql, -1, &insertStmt, nullptr);
  if (rc != SQLITE_OK) {
    Serial.println("[Catalog] Failed to prepare insert statement.");
    sqlite3_finalize(updateStmt);
    sqlite3_close(db);
    return false;
  }

  File root = SD.open(kPhotoDirectory);
  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        const String filename = String(entry.name());
        const String localPath = String(kPhotoDirectory) + "/" + filename;
        if (!localPath.endsWith("/catalog.db")) {
          const int64_t sizeBytes = static_cast<int64_t>(entry.size());

          sqlite3_bind_int64(updateStmt, 1, sizeBytes);
          sqlite3_bind_text(updateStmt, 2, localPath.c_str(), -1, SQLITE_TRANSIENT);
          rc = sqlite3_step(updateStmt);
          sqlite3_reset(updateStmt);
          sqlite3_clear_bindings(updateStmt);

          if (rc != SQLITE_DONE) {
            Serial.println("[Catalog] Failed updating local_present during reconcile.");
          } else if (sqlite3_changes(db) == 0) {
            sqlite3_bind_text(insertStmt, 1, filename.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertStmt, 2, localPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(insertStmt, 3, sizeBytes);
            rc = sqlite3_step(insertStmt);
            sqlite3_reset(insertStmt);
            sqlite3_clear_bindings(insertStmt);
            if (rc != SQLITE_DONE) {
              Serial.println("[Catalog] Failed inserting local-only file into catalog.");
            }
          }
        }
      }
      entry.close();
      entry = root.openNextFile();
    }
    root.close();
  }

  sqlite3_finalize(updateStmt);
  sqlite3_finalize(insertStmt);
  sqlite3_close(db);
  Serial.println("[Catalog] Reconciled catalog with SD files.");
  return true;
}

bool PhotoCatalog::beginRemoteSync() {
  if (!initialized_ && !begin()) {
    return false;
  }
  return execute("UPDATE photos SET remote_present = 0 WHERE item_id IS NOT NULL;");
}

bool PhotoCatalog::upsertRemotePhoto(const RemotePhotoMetadata& metadata) {
  if (!initialized_ && !begin()) {
    return false;
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    Serial.print("[Catalog] sqlite3_open failed: ");
    Serial.println(sqlite3_errstr(rc));
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  const char* updateSql =
      "UPDATE photos SET filename=?, local_path=?, size_bytes=?, width=?, height=?, orientation=?, "
      "last_modified_utc=?, etag=?, mime_type=?, remote_present=1 WHERE item_id=?;";
  sqlite3_stmt* updateStmt = nullptr;
  rc = sqlite3_prepare_v2(db, updateSql, -1, &updateStmt, nullptr);
  if (rc != SQLITE_OK) {
    logSqliteError("prepare remote metadata update", db, rc);
    sqlite3_close(db);
    return false;
  }

  bindNullableText(updateStmt, 1, metadata.filename);
  bindNullableText(updateStmt, 2, metadata.localPath);
  sqlite3_bind_int64(updateStmt, 3, static_cast<int64_t>(metadata.sizeBytes));
  if (metadata.width >= 0) {
    sqlite3_bind_int(updateStmt, 4, metadata.width);
  } else {
    sqlite3_bind_null(updateStmt, 4);
  }
  if (metadata.height >= 0) {
    sqlite3_bind_int(updateStmt, 5, metadata.height);
  } else {
    sqlite3_bind_null(updateStmt, 5);
  }
  bindNullableText(updateStmt, 6, metadata.orientation);
  bindNullableText(updateStmt, 7, metadata.modifiedUtc);
  bindNullableText(updateStmt, 8, metadata.etag);
  bindNullableText(updateStmt, 9, metadata.mimeType);
  bindNullableText(updateStmt, 10, metadata.itemId);

  rc = sqlite3_step(updateStmt);
  sqlite3_finalize(updateStmt);
  if (rc != SQLITE_DONE) {
    logSqliteError("update remote metadata", db, rc);
    sqlite3_close(db);
    return false;
  }

  if (sqlite3_changes(db) == 0) {
    const char* adoptSql =
        "UPDATE photos SET item_id=?, filename=?, size_bytes=?, width=?, height=?, orientation=?, "
        "last_modified_utc=?, etag=?, mime_type=?, remote_present=1 WHERE local_path=?;";
    sqlite3_stmt* adoptStmt = nullptr;
    rc = sqlite3_prepare_v2(db, adoptSql, -1, &adoptStmt, nullptr);
    if (rc != SQLITE_OK) {
      logSqliteError("prepare remote metadata adopt", db, rc);
      sqlite3_close(db);
      return false;
    }

    bindNullableText(adoptStmt, 1, metadata.itemId);
    bindNullableText(adoptStmt, 2, metadata.filename);
    sqlite3_bind_int64(adoptStmt, 3, static_cast<int64_t>(metadata.sizeBytes));
    if (metadata.width >= 0) {
      sqlite3_bind_int(adoptStmt, 4, metadata.width);
    } else {
      sqlite3_bind_null(adoptStmt, 4);
    }
    if (metadata.height >= 0) {
      sqlite3_bind_int(adoptStmt, 5, metadata.height);
    } else {
      sqlite3_bind_null(adoptStmt, 5);
    }
    bindNullableText(adoptStmt, 6, metadata.orientation);
    bindNullableText(adoptStmt, 7, metadata.modifiedUtc);
    bindNullableText(adoptStmt, 8, metadata.etag);
    bindNullableText(adoptStmt, 9, metadata.mimeType);
    bindNullableText(adoptStmt, 10, metadata.localPath);

    rc = sqlite3_step(adoptStmt);
    sqlite3_finalize(adoptStmt);
    if (rc != SQLITE_DONE) {
      logSqliteError("adopt local row for remote metadata", db, rc);
      sqlite3_close(db);
      return false;
    }
  }

  if (sqlite3_changes(db) == 0) {
    const char* insertSql =
        "INSERT INTO photos(item_id, filename, local_path, size_bytes, width, height, orientation, "
        "last_modified_utc, etag, mime_type, remote_present, local_present) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, 0);";
    sqlite3_stmt* insertStmt = nullptr;
    rc = sqlite3_prepare_v2(db, insertSql, -1, &insertStmt, nullptr);
    if (rc != SQLITE_OK) {
      logSqliteError("prepare remote metadata insert", db, rc);
      sqlite3_close(db);
      return false;
    }

    bindNullableText(insertStmt, 1, metadata.itemId);
    bindNullableText(insertStmt, 2, metadata.filename);
    bindNullableText(insertStmt, 3, metadata.localPath);
    sqlite3_bind_int64(insertStmt, 4, static_cast<int64_t>(metadata.sizeBytes));
    if (metadata.width >= 0) {
      sqlite3_bind_int(insertStmt, 5, metadata.width);
    } else {
      sqlite3_bind_null(insertStmt, 5);
    }
    if (metadata.height >= 0) {
      sqlite3_bind_int(insertStmt, 6, metadata.height);
    } else {
      sqlite3_bind_null(insertStmt, 6);
    }
    bindNullableText(insertStmt, 7, metadata.orientation);
    bindNullableText(insertStmt, 8, metadata.modifiedUtc);
    bindNullableText(insertStmt, 9, metadata.etag);
    bindNullableText(insertStmt, 10, metadata.mimeType);

    rc = sqlite3_step(insertStmt);
    sqlite3_finalize(insertStmt);
    if (rc != SQLITE_DONE) {
      logSqliteError("insert remote metadata", db, rc);
      sqlite3_close(db);
      return false;
    }
  }

  sqlite3_close(db);
  return true;
}

bool PhotoCatalog::markDownloaded(const String& itemId, const String& filename, const String& localPath, uint32_t sizeBytes) {
  if (!initialized_ && !begin()) {
    return false;
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    Serial.print("[Catalog] sqlite3_open failed: ");
    Serial.println(sqlite3_errstr(rc));
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  const char* sql =
      "UPDATE photos SET local_present=1, remote_present=1, filename=?, local_path=?, size_bytes=?, downloaded_utc=datetime('now') "
      "WHERE item_id=?;";
  sqlite3_stmt* updateStmt = nullptr;
  rc = sqlite3_prepare_v2(db, sql, -1, &updateStmt, nullptr);
  if (rc != SQLITE_OK) {
    Serial.println("[Catalog] Failed to prepare markDownloaded update.");
    sqlite3_close(db);
    return false;
  }

  bindNullableText(updateStmt, 1, filename);
  bindNullableText(updateStmt, 2, localPath);
  sqlite3_bind_int64(updateStmt, 3, static_cast<int64_t>(sizeBytes));
  bindNullableText(updateStmt, 4, itemId);
  rc = sqlite3_step(updateStmt);
  sqlite3_finalize(updateStmt);
  if (rc != SQLITE_DONE) {
    sqlite3_close(db);
    Serial.println("[Catalog] Failed updating downloaded state.");
    return false;
  }

  if (sqlite3_changes(db) == 0) {
    const char* adoptSql =
        "UPDATE photos SET item_id=?, filename=?, size_bytes=?, local_present=1, remote_present=1, "
        "downloaded_utc=datetime('now') WHERE local_path=?;";
    sqlite3_stmt* adoptStmt = nullptr;
    rc = sqlite3_prepare_v2(db, adoptSql, -1, &adoptStmt, nullptr);
    if (rc == SQLITE_OK) {
      bindNullableText(adoptStmt, 1, itemId);
      bindNullableText(adoptStmt, 2, filename);
      sqlite3_bind_int64(adoptStmt, 3, static_cast<int64_t>(sizeBytes));
      bindNullableText(adoptStmt, 4, localPath);
      rc = sqlite3_step(adoptStmt);
      sqlite3_finalize(adoptStmt);
    }
    if (rc != SQLITE_DONE) {
      sqlite3_close(db);
      Serial.println("[Catalog] Failed adopting local row while marking download.");
      return false;
    }
  }

  if (sqlite3_changes(db) == 0) {
    const char* insertSql =
        "INSERT INTO photos(item_id, filename, local_path, size_bytes, local_present, remote_present, downloaded_utc) "
        "VALUES(?, ?, ?, ?, 1, 1, datetime('now'));";
    sqlite3_stmt* insertStmt = nullptr;
    rc = sqlite3_prepare_v2(db, insertSql, -1, &insertStmt, nullptr);
    if (rc == SQLITE_OK) {
      bindNullableText(insertStmt, 1, itemId);
      bindNullableText(insertStmt, 2, filename);
      bindNullableText(insertStmt, 3, localPath);
      sqlite3_bind_int64(insertStmt, 4, static_cast<int64_t>(sizeBytes));
      rc = sqlite3_step(insertStmt);
      sqlite3_finalize(insertStmt);
    }
    if (rc != SQLITE_DONE) {
      sqlite3_close(db);
      Serial.println("[Catalog] Failed inserting downloaded row.");
      return false;
    }
  }

  sqlite3_close(db);
  return true;
}

bool PhotoCatalog::removeFilesNotInRemoteCatalog(size_t& deletedCount, uint64_t& freedBytes) {
  deletedCount = 0;
  freedBytes = 0;
  if (!initialized_ && !begin()) {
    return false;
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  const char* selectSql =
      "SELECT id, local_path, size_bytes, item_id FROM photos "
      "WHERE local_present = 1 AND (item_id IS NULL OR remote_present = 0);";
  sqlite3_stmt* stmt = nullptr;
  rc = sqlite3_prepare_v2(db, selectSql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const int64_t rowId = sqlite3_column_int64(stmt, 0);
    const char* localPathText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const uint64_t sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    const bool hasItemId = sqlite3_column_type(stmt, 3) != SQLITE_NULL;
    if (localPathText == nullptr) {
      continue;
    }
    const String localPath(localPathText);
    if (localPath.endsWith("/catalog.db")) {
      continue;
    }

    if (SD.exists(localPath) && SD.remove(localPath)) {
      ++deletedCount;
      freedBytes += sizeBytes;
    }

    sqlite3_stmt* updateStmt = nullptr;
    rc = sqlite3_prepare_v2(db, "UPDATE photos SET local_present=0 WHERE id=?;", -1, &updateStmt, nullptr);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(updateStmt, 1, rowId);
      sqlite3_step(updateStmt);
      sqlite3_finalize(updateStmt);
    } else if (updateStmt != nullptr) {
      sqlite3_finalize(updateStmt);
    }

    if (!hasItemId) {
      sqlite3_stmt* deleteStmt = nullptr;
      rc = sqlite3_prepare_v2(db, "DELETE FROM photos WHERE id=?;", -1, &deleteStmt, nullptr);
      if (rc == SQLITE_OK) {
        sqlite3_bind_int64(deleteStmt, 1, rowId);
        sqlite3_step(deleteStmt);
        sqlite3_finalize(deleteStmt);
      } else if (deleteStmt != nullptr) {
        sqlite3_finalize(deleteStmt);
      }
    }
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

uint64_t PhotoCatalog::totalLocalBytes() {
  if (!initialized_ && !begin()) {
    return 0;
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return 0;
  }

  sqlite3_stmt* stmt = nullptr;
  rc = sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(size_bytes),0) FROM photos WHERE local_present=1;", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return 0;
  }

  uint64_t total = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    total = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return total;
}

bool PhotoCatalog::pruneOldestLocalFilesToQuota(uint64_t quotaBytes, size_t& deletedCount, uint64_t& deletedBytes) {
  deletedCount = 0;
  deletedBytes = 0;
  if (!initialized_ && !begin()) {
    return false;
  }

  uint64_t usage = totalLocalBytes();
  if (usage <= quotaBytes) {
    return true;
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(kCatalogPath, &db);
  if (rc != SQLITE_OK) {
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  const char* selectSql =
      "SELECT id, local_path, size_bytes FROM photos "
      "WHERE local_present=1 "
      "ORDER BY CASE WHEN last_modified_utc IS NULL THEN 1 ELSE 0 END, last_modified_utc ASC, id ASC;";
  sqlite3_stmt* stmt = nullptr;
  rc = sqlite3_prepare_v2(db, selectSql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }

  while (usage > quotaBytes && sqlite3_step(stmt) == SQLITE_ROW) {
    const int64_t rowId = sqlite3_column_int64(stmt, 0);
    const char* localPathText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const uint64_t sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    if (localPathText == nullptr) {
      continue;
    }
    const String localPath(localPathText);
    if (localPath.endsWith("/catalog.db")) {
      continue;
    }

    if (SD.exists(localPath) && SD.remove(localPath)) {
      ++deletedCount;
      deletedBytes += sizeBytes;
      usage = usage > sizeBytes ? usage - sizeBytes : 0;
    }

    sqlite3_stmt* updateStmt = nullptr;
    rc = sqlite3_prepare_v2(db, "UPDATE photos SET local_present=0 WHERE id=?;", -1, &updateStmt, nullptr);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(updateStmt, 1, rowId);
      sqlite3_step(updateStmt);
      sqlite3_finalize(updateStmt);
    } else if (updateStmt != nullptr) {
      sqlite3_finalize(updateStmt);
    }
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

}  // namespace odf
