#include "PhotoCatalog.h"

#include <SD.h>

extern "C" {
#include <sqlite3.h>
}

#include "FrameConfig.h"

namespace odf {

namespace {

constexpr const char* kCatalogPath = "/sd/catalog.db";

void resetStatement(sqlite3_stmt* statement) {
  if (statement != nullptr) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
  }
}

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
  Serial.println(db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
}

}  // namespace

PhotoCatalog::~PhotoCatalog() {
  end();
}

bool PhotoCatalog::openDatabase() {
  if (database_ != nullptr) {
    return true;
  }

  Serial.print("[Catalog] Opening persistent DB: ");
  Serial.println(kCatalogPath);
  const int rc = sqlite3_open(kCatalogPath, &database_);
  if (rc != SQLITE_OK) {
    logSqliteError("open database", database_, rc);
    closeDatabase();
    return false;
  }
  sqlite3_busy_timeout(database_, 1000);
  if (!execute("PRAGMA synchronous=NORMAL;PRAGMA temp_store=MEMORY;PRAGMA cache_size=-4096;")) {
    closeDatabase();
    return false;
  }
  return true;
}

void PhotoCatalog::closeDatabase() {
  if (database_ == nullptr) {
    return;
  }
  if (transactionActive_) {
    sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
    transactionActive_ = false;
  }
  finalizeStatements();
  sqlite3_close(database_);
  database_ = nullptr;
}

void PhotoCatalog::end() {
  closeDatabase();
  initialized_ = false;
}

bool PhotoCatalog::execute(const char* sql) {
  constexpr int kMaxAttempts = 3;
  if (database_ == nullptr) {
    return false;
  }

  for (int attempt = 1;; ++attempt) {
    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(database_, sql, nullptr, nullptr, &errorMessage);
    if (rc == SQLITE_OK) {
      return true;
    }

    Serial.print("[Catalog] sqlite3_exec failed: ");
    Serial.print(errorMessage != nullptr ? errorMessage : sqlite3_errstr(rc));
    sqlite3_free(errorMessage);
    Serial.printf(" (attempt %d/%d)\n", attempt, kMaxAttempts);
    if (((rc & 0xFF) != SQLITE_IOERR && rc != SQLITE_BUSY) || attempt >= kMaxAttempts) {
      return false;
    }
    delay(250 * attempt);
  }
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

  struct Migration {
    const char* column;
    const char* sql;
  };
  static constexpr Migration kMigrations[] = {
      {"remote_present", "ALTER TABLE photos ADD COLUMN remote_present INTEGER NOT NULL DEFAULT 0;"},
  };
  for (const Migration& migration : kMigrations) {
    if (hasColumn(database_, "photos", migration.column)) {
      continue;
    }
    bool migrated = false;
    for (int attempt = 1; attempt <= 3 && !migrated; ++attempt) {
      char* errorMessage = nullptr;
      const int rc = sqlite3_exec(database_, migration.sql, nullptr, nullptr, &errorMessage);
      if (rc == SQLITE_OK ||
          (errorMessage != nullptr && strstr(errorMessage, "duplicate column name") != nullptr)) {
        migrated = true;
      } else {
        Serial.print("[Catalog] Schema migration ");
        Serial.print(migration.column);
        Serial.print(" failed: ");
        Serial.print(errorMessage != nullptr ? errorMessage : sqlite3_errstr(rc));
        Serial.printf(" (attempt %d/3)\n", attempt);
        if (attempt < 3 && (rc == SQLITE_BUSY || (rc & 0xFF) == SQLITE_IOERR)) {
          delay(250 * attempt);
        } else {
          sqlite3_free(errorMessage);
          return false;
        }
      }
      sqlite3_free(errorMessage);
    }
    if (!migrated) {
      return false;
    }
  }

  return true;
}

bool PhotoCatalog::prepareStatements() {
  const auto prepare = [this](sqlite3_stmt** statement, const char* sql, const char* name) {
    const int rc = sqlite3_prepare_v2(database_, sql, -1, statement, nullptr);
    if (rc != SQLITE_OK) {
      logSqliteError(name, database_, rc);
      return false;
    }
    return true;
  };

  return prepare(&reconcileUpdate_, "UPDATE photos SET local_present=1, size_bytes=? WHERE local_path=?;",
                 "prepare reconcile update") &&
         prepare(&reconcileInsert_,
                 "INSERT INTO photos(item_id, filename, local_path, size_bytes, local_present, downloaded_utc) "
                 "VALUES(NULL, ?, ?, ?, 1, datetime('now'));",
                 "prepare reconcile insert") &&
         prepare(&remoteUpdate_,
                 "UPDATE photos SET filename=?, local_path=?, size_bytes=?, width=?, height=?, orientation=?, "
                 "last_modified_utc=?, etag=?, mime_type=?, remote_present=1 WHERE item_id=?;",
                 "prepare remote update") &&
         prepare(&remoteAdopt_,
                 "UPDATE photos SET item_id=?, filename=?, size_bytes=?, width=?, height=?, orientation=?, "
                 "last_modified_utc=?, etag=?, mime_type=?, remote_present=1 "
                 "WHERE local_path=?;",
                 "prepare remote adopt") &&
         prepare(&remoteInsert_,
                 "INSERT INTO photos(item_id, filename, local_path, size_bytes, width, height, orientation, "
                 "last_modified_utc, etag, mime_type, remote_present, local_present) "
                 "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, 0);",
                 "prepare remote insert") &&
         prepare(&downloadUpdate_,
                 "UPDATE photos SET local_present=1, remote_present=1, filename=?, local_path=?, "
                 "size_bytes=?, downloaded_utc=datetime('now') "
                 "WHERE item_id=?;",
                 "prepare download update") &&
         prepare(&downloadAdopt_,
                 "UPDATE photos SET item_id=?, filename=?, size_bytes=?, local_present=1, remote_present=1, "
                 "downloaded_utc=datetime('now') "
                 "WHERE local_path=?;",
                 "prepare download adopt") &&
         prepare(&downloadInsert_,
                 "INSERT INTO photos(item_id, filename, local_path, size_bytes, local_present, remote_present, "
                 "downloaded_utc) VALUES(?, ?, ?, ?, 1, 1, datetime('now'));",
                 "prepare download insert") &&
         prepare(&markMissing_, "UPDATE photos SET local_present=0 WHERE id=?;",
                 "prepare mark missing");
}

void PhotoCatalog::finalizeStatements() {
  sqlite3_finalize(reconcileUpdate_);
  sqlite3_finalize(reconcileInsert_);
  sqlite3_finalize(remoteUpdate_);
  sqlite3_finalize(remoteAdopt_);
  sqlite3_finalize(remoteInsert_);
  sqlite3_finalize(downloadUpdate_);
  sqlite3_finalize(downloadAdopt_);
  sqlite3_finalize(downloadInsert_);
  sqlite3_finalize(markMissing_);
  reconcileUpdate_ = nullptr;
  reconcileInsert_ = nullptr;
  remoteUpdate_ = nullptr;
  remoteAdopt_ = nullptr;
  remoteInsert_ = nullptr;
  downloadUpdate_ = nullptr;
  downloadAdopt_ = nullptr;
  downloadInsert_ = nullptr;
  markMissing_ = nullptr;
}

bool PhotoCatalog::begin() {
  if (initialized_ && database_ != nullptr) {
    return true;
  }
  if (!openDatabase() || !createSchema() || !prepareStatements()) {
    closeDatabase();
    initialized_ = false;
    return false;
  }
  initialized_ = true;
  Serial.println("[Catalog] SQLite catalog ready with persistent prepared statements.");
  return true;
}

bool PhotoCatalog::beginTransaction() {
  if (!initialized_ || database_ == nullptr || transactionActive_) {
    return false;
  }
  if (!execute("BEGIN IMMEDIATE;")) {
    return false;
  }
  transactionActive_ = true;
  return true;
}

bool PhotoCatalog::commitTransaction() {
  if (!transactionActive_ || !execute("COMMIT;")) {
    return false;
  }
  transactionActive_ = false;
  return true;
}

bool PhotoCatalog::rollbackTransaction() {
  if (!transactionActive_) {
    return true;
  }
  const bool ok = execute("ROLLBACK;");
  if (ok) {
    transactionActive_ = false;
  }
  return ok;
}

bool PhotoCatalog::reconcileWithStorage() {
  if ((!initialized_ && !begin()) || !beginTransaction()) {
    return false;
  }
  bool ok = execute("UPDATE photos SET local_present=0;");
  File root = SD.open(kPhotoDirectory);
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    rollbackTransaction();
    return false;
  }

  size_t files = 0;
  File entry = root.openNextFile();
  while (ok && entry) {
    if (!entry.isDirectory()) {
      const String filename = String(entry.name());
      const String localPath = String(kPhotoDirectory) + "/" + filename;
      if (!localPath.endsWith(".part")) {
        resetStatement(reconcileUpdate_);
        sqlite3_bind_int64(reconcileUpdate_, 1, static_cast<int64_t>(entry.size()));
        sqlite3_bind_text(reconcileUpdate_, 2, localPath.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(reconcileUpdate_);
        const bool updateDone = rc == SQLITE_DONE;
        const bool existsInCatalog = updateDone && sqlite3_changes(database_) > 0;
        if (!existsInCatalog) {
          resetStatement(reconcileInsert_);
          sqlite3_bind_text(reconcileInsert_, 1, filename.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(reconcileInsert_, 2, localPath.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64(reconcileInsert_, 3, static_cast<int64_t>(entry.size()));
          rc = sqlite3_step(reconcileInsert_);
          if (rc != SQLITE_DONE) {
            logSqliteError("insert local file", database_, rc);
            ok = false;
          }
        } else if (!updateDone) {
          logSqliteError("update local file", database_, rc);
          ok = false;
        }
        if (ok) {
          ++files;
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
    if ((files & 0x0F) == 0) {
      vTaskDelay(1);
    }
  }
  root.close();

  if (!ok || !commitTransaction()) {
    rollbackTransaction();
    Serial.println("[Catalog] Storage reconciliation rolled back.");
    return false;
  }
  Serial.printf("[Catalog] Reconciled catalog with %u SD files.\n", static_cast<unsigned>(files));
  return true;
}

bool PhotoCatalog::beginRemoteSync() {
  if (!initialized_ && !begin()) {
    return false;
  }
  return execute("UPDATE photos SET remote_present=0 WHERE item_id IS NOT NULL;");
}

bool PhotoCatalog::stepUpsertRemote(sqlite3_stmt* statement, const RemotePhotoMetadata& metadata, bool matchItemId) {
  resetStatement(statement);
  int index = 1;
  if (!matchItemId) {
    bindNullableText(statement, index++, metadata.itemId);
  }
  bindNullableText(statement, index++, metadata.filename);
  if (matchItemId) {
    bindNullableText(statement, index++, metadata.localPath);
  }
  sqlite3_bind_int64(statement, index++, static_cast<int64_t>(metadata.sizeBytes));
  metadata.width >= 0 ? sqlite3_bind_int(statement, index, metadata.width) : sqlite3_bind_null(statement, index);
  ++index;
  metadata.height >= 0 ? sqlite3_bind_int(statement, index, metadata.height) : sqlite3_bind_null(statement, index);
  ++index;
  bindNullableText(statement, index++, metadata.orientation);
  bindNullableText(statement, index++, metadata.modifiedUtc);
  bindNullableText(statement, index++, metadata.etag);
  bindNullableText(statement, index++, metadata.mimeType);
  bindNullableText(statement, index++, matchItemId ? metadata.itemId : metadata.localPath);

  const int rc = sqlite3_step(statement);
  if (rc != SQLITE_DONE) {
    logSqliteError("upsert remote metadata", database_, rc);
    return false;
  }
  return true;
}

bool PhotoCatalog::upsertRemotePhoto(const RemotePhotoMetadata& metadata) {
  if (!initialized_ && !begin()) {
    return false;
  }
  if (!stepUpsertRemote(remoteUpdate_, metadata, true)) {
    return false;
  }
  if (sqlite3_changes(database_) == 0) {
    resetStatement(remoteAdopt_);
    int index = 1;
    bindNullableText(remoteAdopt_, index++, metadata.itemId);
    bindNullableText(remoteAdopt_, index++, metadata.filename);
    sqlite3_bind_int64(remoteAdopt_, index++, static_cast<int64_t>(metadata.sizeBytes));
    metadata.width >= 0 ? sqlite3_bind_int(remoteAdopt_, index, metadata.width) : sqlite3_bind_null(remoteAdopt_, index);
    ++index;
    metadata.height >= 0 ? sqlite3_bind_int(remoteAdopt_, index, metadata.height)
                         : sqlite3_bind_null(remoteAdopt_, index);
    ++index;
    bindNullableText(remoteAdopt_, index++, metadata.orientation);
    bindNullableText(remoteAdopt_, index++, metadata.modifiedUtc);
    bindNullableText(remoteAdopt_, index++, metadata.etag);
    bindNullableText(remoteAdopt_, index++, metadata.mimeType);
    bindNullableText(remoteAdopt_, index++, metadata.localPath);
    const int adoptRc = sqlite3_step(remoteAdopt_);
    if (adoptRc != SQLITE_DONE) {
      logSqliteError("adopt local metadata", database_, adoptRc);
      return false;
    }
  }
  if (sqlite3_changes(database_) == 0) {
    resetStatement(remoteInsert_);
    int index = 1;
    bindNullableText(remoteInsert_, index++, metadata.itemId);
    bindNullableText(remoteInsert_, index++, metadata.filename);
    bindNullableText(remoteInsert_, index++, metadata.localPath);
    sqlite3_bind_int64(remoteInsert_, index++, static_cast<int64_t>(metadata.sizeBytes));
    metadata.width >= 0 ? sqlite3_bind_int(remoteInsert_, index, metadata.width) : sqlite3_bind_null(remoteInsert_, index);
    ++index;
    metadata.height >= 0 ? sqlite3_bind_int(remoteInsert_, index, metadata.height)
                         : sqlite3_bind_null(remoteInsert_, index);
    ++index;
    bindNullableText(remoteInsert_, index++, metadata.orientation);
    bindNullableText(remoteInsert_, index++, metadata.modifiedUtc);
    bindNullableText(remoteInsert_, index++, metadata.etag);
    bindNullableText(remoteInsert_, index, metadata.mimeType);
    const int insertRc = sqlite3_step(remoteInsert_);
    if (insertRc != SQLITE_DONE) {
      logSqliteError("insert remote metadata", database_, insertRc);
      return false;
    }
  }
  return true;
}

bool PhotoCatalog::stepMarkDownloaded(sqlite3_stmt* statement, const String& itemId, const String& filename,
                                      const String& localPath, uint32_t sizeBytes, bool matchItemId) {
  resetStatement(statement);
  int index = 1;
  if (!matchItemId) {
    bindNullableText(statement, index++, itemId);
  }
  bindNullableText(statement, index++, filename);
  if (matchItemId) {
    bindNullableText(statement, index++, localPath);
  }
  sqlite3_bind_int64(statement, index++, static_cast<int64_t>(sizeBytes));
  if (matchItemId) {
    bindNullableText(statement, index++, itemId);
  } else {
    bindNullableText(statement, index++, localPath);
  }
  const int rc = sqlite3_step(statement);
  if (rc != SQLITE_DONE) {
    logSqliteError("mark downloaded", database_, rc);
    return false;
  }
  return true;
}

bool PhotoCatalog::markDownloaded(const String& itemId, const String& filename, const String& localPath,
                                  uint32_t sizeBytes) {
  if (!initialized_ && !begin()) {
    return false;
  }
  if (!stepMarkDownloaded(downloadUpdate_, itemId, filename, localPath, sizeBytes, true)) {
    return false;
  }
  if (sqlite3_changes(database_) == 0 &&
      !stepMarkDownloaded(downloadAdopt_, itemId, filename, localPath, sizeBytes, false)) {
    return false;
  }
  if (sqlite3_changes(database_) == 0) {
    resetStatement(downloadInsert_);
    bindNullableText(downloadInsert_, 1, itemId);
    bindNullableText(downloadInsert_, 2, filename);
    bindNullableText(downloadInsert_, 3, localPath);
    sqlite3_bind_int64(downloadInsert_, 4, static_cast<int64_t>(sizeBytes));
    const int rc = sqlite3_step(downloadInsert_);
    if (rc != SQLITE_DONE) {
      logSqliteError("insert downloaded photo", database_, rc);
      return false;
    }
  }
  return true;
}

bool PhotoCatalog::removeFilesNotInRemoteCatalog(size_t& deletedCount, uint64_t& freedBytes) {
  deletedCount = 0;
  freedBytes = 0;
  if (!initialized_ && !begin()) {
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  const char* selectSql =
      "SELECT id, local_path, size_bytes, item_id FROM photos "
      "WHERE local_present=1 AND (item_id IS NULL OR remote_present=0);";
  int rc = sqlite3_prepare_v2(database_, selectSql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    logSqliteError("prepare stale photo query", database_, rc);
    return false;
  }

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const int64_t rowId = sqlite3_column_int64(stmt, 0);
    const char* pathText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const uint64_t sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    if (pathText == nullptr) {
      continue;
    }
    const String path(pathText);
    if (SD.exists(path) && SD.remove(path)) {
      ++deletedCount;
      freedBytes += sizeBytes;
    }
    resetStatement(markMissing_);
    sqlite3_bind_int64(markMissing_, 1, rowId);
    if (sqlite3_step(markMissing_) != SQLITE_DONE) {
      sqlite3_finalize(stmt);
      return false;
    }
    if (sqlite3_column_type(stmt, 3) == SQLITE_NULL) {
      sqlite3_stmt* deleteStmt = nullptr;
      if (sqlite3_prepare_v2(database_, "DELETE FROM photos WHERE id=?;", -1, &deleteStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(deleteStmt, 1, rowId);
        sqlite3_step(deleteStmt);
        sqlite3_finalize(deleteStmt);
      }
    }
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

uint64_t PhotoCatalog::totalLocalBytes() {
  if (!initialized_ && !begin()) {
    return 0;
  }
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(database_,
                              "SELECT COALESCE(SUM(size_bytes),0) FROM photos WHERE local_present=1;", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return 0;
  }
  uint64_t total = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    total = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return total;
}

bool PhotoCatalog::pruneOldestLocalFilesToQuota(uint64_t quotaBytes, size_t& deletedCount, uint64_t& deletedBytes) {
  deletedCount = 0;
  deletedBytes = 0;
  uint64_t usage = totalLocalBytes();
  if (usage <= quotaBytes) {
    return true;
  }

  sqlite3_stmt* stmt = nullptr;
  const char* selectSql =
      "SELECT id, local_path, size_bytes FROM photos WHERE local_present=1 "
      "ORDER BY CASE WHEN last_modified_utc IS NULL THEN 1 ELSE 0 END, last_modified_utc ASC, id ASC;";
  int rc = sqlite3_prepare_v2(database_, selectSql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    logSqliteError("prepare quota query", database_, rc);
    return false;
  }
  while (usage > quotaBytes && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const int64_t rowId = sqlite3_column_int64(stmt, 0);
    const char* pathText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const uint64_t sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    if (pathText != nullptr) {
      const String path(pathText);
      if (SD.exists(path) && SD.remove(path)) {
        ++deletedCount;
        deletedBytes += sizeBytes;
        usage = usage > sizeBytes ? usage - sizeBytes : 0;
      }
    }
    resetStatement(markMissing_);
    sqlite3_bind_int64(markMissing_, 1, rowId);
    if (sqlite3_step(markMissing_) != SQLITE_DONE) {
      sqlite3_finalize(stmt);
      return false;
    }
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

}  // namespace odf
