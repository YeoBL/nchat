#pragma once

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <string>
#include <unistd.h>

namespace TextGlyph
{
  inline std::string HelperPath()
  {
    const char* config = std::getenv("NCHAT_KITTY_CONFIG_DIR");
    if (config && *config) return std::string(config) + "/nchat_emoji.py";
    config = std::getenv("XDG_CONFIG_HOME");
    if (config && *config) return std::string(config) + "/kitty/nchat_emoji.py";
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/kitty/nchat_emoji.py" : "";
  }

  inline bool Enabled()
  {
    const char* override = std::getenv("NCHAT_FULL_EMOJI");
    if (override) return std::strcmp(override, "1") == 0;
    static const bool supported = std::getenv("KITTY_WINDOW_ID") && access(HelperPath().c_str(), R_OK) == 0;
    return supported;
  }

  inline bool Regional(wchar_t value) { return value >= 0x1f1e6 && value <= 0x1f1ff; }
  inline bool Tone(wchar_t value) { return value >= 0x1f3fb && value <= 0x1f3ff; }
  inline bool Emoji(wchar_t value)
  {
    return (value >= 0x1f000 && value <= 0x1faff) || (value >= 0x2190 && value <= 0x27ff) ||
      value == 0x00a9 || value == 0x00ae || value == 0x203c || value == 0x2049 ||
      value == 0x2122 || value == 0x2139 || value == 0x3030 || value == 0x303d ||
      value == 0x3297 || value == 0x3299;
  }

  struct Glyph { size_t end; int width; bool paint; };

  inline Glyph Next(const wchar_t* text, size_t start, size_t limit)
  {
    const wchar_t base = text[start];
    size_t end = start + 1;
    if (Regional(base) && end < limit && Regional(text[end])) return {end + 1, 2, true};
    const bool emojiBase = Emoji(base);
    const bool keycapBase = (base >= L'0' && base <= L'9') || base == L'#' || base == L'*';
    bool combined = false, marks = false, textPresentation = false, joined = false;
    while (end < limit && end - start < 64)
    {
      const wchar_t value = text[end];
      const bool keycap = keycapBase && (value == 0x20e3 ||
        (value == 0xfe0f && end + 1 < limit && text[end + 1] == 0x20e3));
      if (keycap || (emojiBase && (value == 0xfe0e || value == 0xfe0f || Tone(value) ||
                                  (value >= 0xe0020 && value <= 0xe007f))))
      {
        combined = true;
        if (value == 0xfe0e || value == 0xfe0f) textPresentation = value == 0xfe0e;
        ++end;
      }
      else if (emojiBase && value == 0x200d && end + 1 < limit && end + 2 - start <= 64 && Emoji(text[end + 1]))
      {
        combined = true;
        joined = true;
        end += 2;
      }
      else if (value >= 0x0300 && wcwidth(value) == 0 && value != 0x200d &&
               !(value >= 0x202a && value <= 0x202e) && !(value >= 0x2066 && value <= 0x2069))
      {
        // Screen's combining-mark repaint can also desynchronize relative
        // cursor moves. Keep accents with their base and paint them in Kitty.
        marks = true;
        ++end;
      }
      else break;
    }
    // Kitty's Unicode width table is newer than the host libc table.
    const bool terminalWide = Regional(base) || base == 0x261d ||
      (base >= 0x2630 && base <= 0x2637) || (base >= 0x268a && base <= 0x268f) ||
      base == 0x26f9 || base == 0x270c || base == 0x270d || base == 0x1f3cb ||
      base == 0x1f3cc || base == 0x1f574 || base == 0x1f575 || base == 0x1f590;
    const int plainWidth = wcwidth(base);
    const int width = combined ? (textPresentation && !joined ? 1 : 2) :
      (terminalWide || (emojiBase && plainWidth < 1) ? 2 : std::max(0, plainWidth));
    return {end, width, width > 0 && (combined || marks || emojiBase)};
  }
}
