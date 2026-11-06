#pragma once

namespace odf {

enum class AppState {
  kStart,
  kWifiConnect,
  kLoadToken,
  kCheckTokenValidity,
  kRefreshOrLogin,
  kListOneDriveFolder,
  kDownloadNewPhotos,
  kPruneOldPhotos,
  kDisplaySlideshow,
  kIdleWaitForSync
};

}  // namespace odf
