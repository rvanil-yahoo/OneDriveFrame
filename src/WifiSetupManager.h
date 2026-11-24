#pragma once

#include <Arduino.h>
#include <functional>
#include <utility>

namespace odf {

class WifiSetupManager {
 public:
  using PromptCallback = std::function<void(const String&, const String&, uint32_t)>;
  using ClearPromptCallback = std::function<void()>;

  void setPromptCallbacks(PromptCallback prompt, ClearPromptCallback clear) {
    promptCallback_ = std::move(prompt);
    clearPromptCallback_ = std::move(clear);
  }
  bool connect();

 private:
  PromptCallback promptCallback_;
  ClearPromptCallback clearPromptCallback_;
};

}  // namespace odf
