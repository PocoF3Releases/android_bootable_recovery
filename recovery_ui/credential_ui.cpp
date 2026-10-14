/*
 * Copyright (C) 2026 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "recovery_ui/screen_ui.h"

#include <linux/input.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include <android-base/scopeguard.h>

#include "minui/minui.h"

namespace {
constexpr size_t kMinimumCredentialLength = 4;
enum CredentialAction { kCharacter, kErase, kShift, kMode, kSpace, kCancel, kUnlock };
}  // namespace

void ScreenRecoveryUI::DrawCredentialLocked() {
  gr_color(0, 0, 0, 255);
  gr_clear();
  credential_keys_.clear();
  const int gap = std::max(3, std::min({ PixelsFromDp(credential_pin_ ? 8 : 3), ScreenWidth() / 60,
                                         ScreenHeight() / 64 }));
  const int available_width = ScreenWidth() - 2 * std::max(margin_width_, PixelsFromDp(8));
  const int width =
      credential_pin_ ? std::min(available_width - 2 * gap, PixelsFromDp(360)) : available_width;
  const int x = (ScreenWidth() - width) / 2;
  const int rows = credential_pin_ || !credential_symbols_ ? 4 : 5;
  const int header = 3 * menu_char_height_ + 2 * gap;
  const int field = 2 * menu_char_height_ + 2 * gap;
  const int key_height = std::max(
      menu_char_height_ + 2 * gap,
      std::min(PixelsFromDp(credential_pin_ ? 72 : 52),
               (ScreenHeight() - header - field - 12 * gap - 2 * margin_height_) / (rows + 1)));
  const int total = header + field + (rows + 1) * (key_height + gap) + 3 * gap;
  int y = std::max(margin_height_ + 2 * gap, (ScreenHeight() - total) / 2);
  auto text = [&](const std::string& label, int cy, UIElement color, bool bold = false) {
    SetColor(color);
    gr_text(gr_menu_font(), (ScreenWidth() - gr_measure(gr_menu_font(), label.c_str())) / 2, cy,
            label.c_str(), bold);
  };
  auto rounded = [&](int bx, int by, int bw, int bh, int radius) {
    radius = std::min(radius, std::min(bw, bh) / 2);
    gr_fill(bx, by + radius, bx + bw, by + bh - radius);
    for (int dy = 0; dy < radius; ++dy) {
      const int inset =
          radius -
          static_cast<int>(std::sqrt(radius * radius - (radius - dy - 1) * (radius - dy - 1)));
      gr_fill(bx + inset, by + dy, bx + bw - inset, by + dy + 1);
      gr_fill(bx + inset, by + bh - dy - 1, bx + bw - inset, by + bh - dy);
    }
  };
  text("Unlock internal storage", y, UIElement::INFO, true);
  y += menu_char_height_ + gap;
  text(credential_pin_ ? "Enter your screen lock PIN" : "Enter your screen lock password", y,
       UIElement::MENU);
  y += 2 * menu_char_height_ + gap;
  SetColor(UIElement::MENU_BG);
  rounded(x, y, width, field, PixelsFromDp(12));
  std::string mask(std::min<size_t>(credential_length_, 18), '*');
  if (credential_length_ > 18) mask += " +" + std::to_string(credential_length_ - 18);
  text(mask.empty() ? (credential_pin_ ? "PIN" : "Password") : mask,
       y + (field - menu_char_height_) / 2, UIElement::INFO);
  y += field + 3 * gap;
  auto key = [&](int bx, int by, int bw, const std::string& label, char value, int action) {
    credential_keys_.push_back({ bx, by, bw, key_height, label, value, action });
  };
  auto row = [&](const std::string& chars, int columns) {
    const int w = (width - (columns - 1) * gap) / columns;
    const int start = x + (width - static_cast<int>(chars.size()) * (w + gap) + gap) / 2;
    for (size_t i = 0; i < chars.size(); ++i)
      key(start + i * (w + gap), y, w, std::string(1, chars[i]), chars[i], kCharacter);
    y += key_height + gap;
  };
  if (credential_pin_) {
    row("123", 3);
    row("456", 3);
    row("789", 3);
    const int w = (width - 2 * gap) / 3;
    key(x + w + gap, y, w, "0", '0', kCharacter);
    key(x + 2 * (w + gap), y, w, "DEL", 0, kErase);
    y += key_height + gap;
  } else {
    if (credential_symbols_) {
      row("1234567890", 10);
      row("!@#$%^&*()", 10);
      row("-_=+[]{}\\|", 10);
      row(";:'\",.<>/?`~", 12);
    } else {
      row(credential_upper_ ? "QWERTYUIOP" : "qwertyuiop", 10);
      row(credential_upper_ ? "ASDFGHJKL" : "asdfghjkl", 10);
      const int w = (width - 8 * gap) / 9;
      key(x, y, w, "Shift", 0, kShift);
      const std::string letters = credential_upper_ ? "ZXCVBNM" : "zxcvbnm";
      for (size_t i = 0; i < letters.size(); ++i)
        key(x + (i + 1) * (w + gap), y, w, std::string(1, letters[i]), letters[i], kCharacter);
      key(x + 8 * (w + gap), y, w, "DEL", 0, kErase);
      y += key_height + gap;
    }
    const int side = (width - 2 * gap) / 5;
    key(x, y, side, credential_symbols_ ? "ABC" : "123", 0, kMode);
    key(x + side + gap, y, width - 2 * side - 2 * gap, "Space", 0, kSpace);
    if (credential_symbols_)
      key(x + width - side, y, side, "DEL", 0, kErase);
    else
      key(x + width - side, y, side, ".", '.', kCharacter);
    y += key_height + gap;
  }
  y += gap;
  key(x, y, (width - gap) / 2, "Cancel", 0, kCancel);
  key(x + (width + gap) / 2, y, (width - gap) / 2, "Unlock", 0, kUnlock);
  for (size_t i = 0; i < credential_keys_.size(); ++i) {
    const auto& button = credential_keys_[i];
    const bool selected = credential_focus_ && credential_selection_ == static_cast<int>(i);
    const bool can_unlock = credential_length_ >= kMinimumCredentialLength;
    const bool active_shift = button.action == kShift && credential_upper_;
    SetColor(selected || active_shift || (button.action == kUnlock && can_unlock)
                 ? UIElement::MENU_SEL_BG
                 : UIElement::MENU_BG);
    rounded(button.x, button.y, button.width, button.height, PixelsFromDp(12));
    SetColor(button.action == kUnlock && !can_unlock ? UIElement::LOG : UIElement::MENU_SEL_FG);
    std::string label = button.label;
    // Keep compact keyboard labels inside their actual touch target.
    if (button.action == kShift) label = "^";
    gr_text(gr_menu_font(),
            button.x + (button.width - gr_measure(gr_menu_font(), label.c_str())) / 2,
            button.y + (button.height - menu_char_height_) / 2, label.c_str(), false);
  }
}

bool ScreenRecoveryUI::RequestCredential(bool pin, std::string* credential) {
  if (!credential || !credential->empty()) return false;
  credential->reserve(64);
  const bool old_reboot = IsRebootEnabled();
  SetEnableReboot(false);
  FlushKeys();
  bool old_text;
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    old_text = show_text;
    show_text = true;
    credential_active_ = true;
    credential_pin_ = pin;
    credential_upper_ = credential_symbols_ = credential_focus_ = false;
    credential_length_ = 0;
    credential_selection_ = 0;
    update_screen_locked();
  }
  auto cleanup = android::base::make_scope_guard([&]() {
    SetEnableReboot(old_reboot);
    FlushKeys();
    std::lock_guard<std::mutex> lock(updateMutex);
    credential_active_ = false;
    credential_length_ = 0;
    credential_keys_.clear();
    show_text = old_text;
    update_screen_locked();
  });
  for (;;) {
    const auto event = WaitInputEvent();
    if (event.type() == EventType::EXTRA) {
      if (event.key() == static_cast<int>(KeyError::INTERRUPTED) ||
          event.key() == static_cast<int>(KeyError::TIMED_OUT))
        return false;
      continue;
    }
    if (event.type() == EventType::SCROLL) continue;
    std::lock_guard<std::mutex> lock(updateMutex);
    const auto old_length = credential_length_;
    const auto old_selection = credential_selection_;
    const bool old_focus = credential_focus_;
    const bool old_upper = credential_upper_;
    const bool old_symbols = credential_symbols_;
    int chosen = -1;
    if (event.type() == EventType::TOUCH) {
      const Point point = TranslateTouch(event.pos());
      credential_focus_ = false;
      for (size_t i = 0; i < credential_keys_.size(); ++i) {
        const auto& button = credential_keys_[i];
        if (point.x() >= button.x && point.x() < button.x + button.width && point.y() >= button.y &&
            point.y() < button.y + button.height) {
          chosen = i;
          break;
        }
      }
    } else {
      const int code = event.key();
      if (code == KEY_BACK || code == KEY_ESC) return false;
      if (code == KEY_VOLUMEUP || code == KEY_UP) {
        credential_selection_ =
            (credential_selection_ + credential_keys_.size() - 1) % credential_keys_.size();
        credential_focus_ = true;
      } else if (code == KEY_VOLUMEDOWN || code == KEY_DOWN) {
        credential_selection_ = (credential_selection_ + 1) % credential_keys_.size();
        credential_focus_ = true;
      } else if (code == KEY_HOME) {
        credential_selection_ = 0;
        credential_focus_ = true;
      } else if (code == KEY_POWER || code == KEY_ENTER)
        chosen = credential_selection_;
    }
    if (chosen >= 0) {
      const auto button = credential_keys_[chosen];
      switch (button.action) {
        case kCancel:
          return false;
        case kUnlock:
          if (credential->size() >= kMinimumCredentialLength) return true;
          break;
        case kErase:
          if (!credential->empty()) {
            credential->back() = '\0';
            credential->pop_back();
          }
          break;
        case kShift:
          credential_upper_ = !credential_upper_;
          break;
        case kMode:
          credential_symbols_ = !credential_symbols_;
          credential_selection_ = 0;
          break;
        case kSpace:
          if (credential->size() < 64) credential->push_back(' ');
          break;
        case kCharacter:
          if (credential->size() < 64) credential->push_back(button.character);
          break;
      }
      credential_length_ = credential->size();
    }
    if (credential_length_ != old_length || credential_selection_ != old_selection ||
        credential_focus_ != old_focus || credential_upper_ != old_upper ||
        credential_symbols_ != old_symbols) {
      update_screen_locked();
    }
  }
}
