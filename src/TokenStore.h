#pragma once

#include <Preferences.h>

#include "OAuthToken.h"

namespace odf {

class TokenStore {
 public:
  bool begin();
  void end();
  bool load(OAuthToken& token) const;
  void save(const OAuthToken& token);
  void clear();

 private:
  mutable Preferences preferences_;
};

}  // namespace odf
