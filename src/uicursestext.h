#pragma once

#include <algorithm>
#include <cwchar>
#include <cstdlib>
#include <ncurses.h>
#include <string>

namespace UiCursesText
{
  inline size_t GlyphEnd(const wchar_t* p_Text, size_t p_Position, size_t p_Limit)
  {
    size_t modifier = p_Position + 1;
    if (modifier < p_Limit && p_Text[modifier] == L'\ufe0f') ++modifier;
    if (modifier < p_Limit && wcwidth(p_Text[p_Position]) == 2 &&
        p_Text[modifier] >= L'\U0001f3fb' && p_Text[modifier] <= L'\U0001f3ff')
      return modifier + 1;
    return p_Position + 1;
  }

  inline int Draw(WINDOW* p_Window, int p_Y, int p_X, const wchar_t* p_Text, int p_Limit)
  {
    if (wmove(p_Window, p_Y, p_X) == ERR) return ERR;
    const bool throughScreen = std::getenv("STY") != nullptr;
    const size_t limit = std::min(std::wcslen(p_Text), static_cast<size_t>(std::max(0, p_Limit)));
    for (size_t position = 0; position < limit;)
    {
      const size_t end = GlyphEnd(p_Text, position, limit);
      if (end > position + 1)
      {
        // Emoji modifiers are spacing characters to libc, but part of the
        // preceding two-column glyph to the terminal. Keep one curses cell pair.
        cchar_t glyph = {};
        const wchar_t base[] = {p_Text[position], L'\0'};
        attr_t attributes = 0;
        short color = 0;
        wattr_get(p_Window, &attributes, &color, nullptr);
        if (setcchar(&glyph, base, attributes, color, nullptr) == ERR) return ERR;
        // Screen tracks modifiers as separate wide characters. Its relay can
        // then disagree with the terminal's combined glyph during scrolling.
        // Display the base in Screen while retaining the original source text.
        if (!throughScreen) std::copy_n(p_Text + position, end - position, glyph.chars);
        if (wadd_wch(p_Window, &glyph) == ERR) return ERR;
      }
      else if (waddnwstr(p_Window, p_Text + position, 1) == ERR) return ERR;
      position = end;
    }
    return OK;
  }

  inline int Width(const std::wstring& p_Text)
  {
    int width = 0;
    for (size_t position = 0; position < p_Text.size();)
    {
      width += std::max(0, wcwidth(p_Text[position]));
      position = GlyphEnd(p_Text.c_str(), position, p_Text.size());
    }
    return width;
  }
}
