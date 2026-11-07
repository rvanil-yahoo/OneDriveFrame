#include "TokenStore.h"

namespace odf {

bool TokenStore::begin() {
  return preferences_.begin("odf_auth", false);
}

void TokenStore::end() {
  preferences_.end();
}

bool TokenStore::load(OAuthToken& token) const {
  token.accessToken = preferences_.getString("access_token", "");
  token.refreshToken = preferences_.getString("refresh_token", "");
  token.expiresAtMs = 0;
  return !token.refreshToken.isEmpty() || !token.accessToken.isEmpty();
}

void TokenStore::save(const OAuthToken& token) {
  preferences_.putString("access_token", token.accessToken);
  preferences_.putString("refresh_token", token.refreshToken);
  preferences_.putULong("expires_at_ms", token.expiresAtMs);
}

void TokenStore::clear() {
  preferences_.remove("access_token");
  preferences_.remove("refresh_token");
  preferences_.remove("expires_at_ms");
}

}  // namespace odf
