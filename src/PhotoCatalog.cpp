#include "PhotoCatalog.h"

#include <SD.h>

extern "C" {
#include <sqlite3.h>
}

#include "FrameConfig.h"

namespace odf {

namespace {

constexpr const char* kCatalogPath = "/sd/photos/catalog.db";

bool bindNullableText(sqlite3_stmt* stmt, int index, const String& value) {
  if (value.isEmpty()) {
    return sqlite3_bind_null(stmt, index) == SQLITE_OK;
  }
  return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
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
      "local_present INTEGER NOT NULL DEFAULT 0,"
      "downloaded_utc TEXT"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_photos_item_id ON photos(item_id);"
      "CREATE INDEX IF NOT EXISTS idx_photos_last_modified ON photos(last_modified_utc);"
      "CREATE INDEX IF NOT EXISTS idx_photos_local_present ON photos(local_present);"
      "CREATE INDEX IF NOT EXISTS idx_photos_size ON photos(size_bytes);";
  return execute(kSchemaSql);
}

bool PhotoCatalog::begin() {
  if (initialized_) {
    return true;
  }

  if (!openDatabase()) {
    return false;
  }

  if (!createSchema()) {
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
      "last_modified_utc=?, etag=?, mime_type=? WHERE item_id=?;";
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
    const char* insertSql =
        "INSERT INTO photos(item_id, filename, local_path, size_bytes, width, height, orientation, "
        "last_modified_utc, etag, mime_type, local_present) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0);";
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
      "UPDATE photos SET local_present=1, filename=?, local_path=?, size_bytes=?, downloaded_utc=datetime('now') "
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
    const char* insertSql =
        "INSERT INTO photos(item_id, filename, local_path, size_bytes, local_present, downloaded_utc) "
        "VALUES(?, ?, ?, ?, 1, datetime('now'));";
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

}  // namespace odf
