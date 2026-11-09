#pragma once

#include <algorithm>
#include <cwchar>
#include <cstdlib>
#include <ncurses.h>
#include <string>
#include <cstdio>

#include "textglyph.h"

namespace UiCursesText
{
  void Prepare(WINDOW* window, int y, int x, int width);
  inline int Width(const std::wstring& text);
  void Record(WINDOW* window, int y, int x, const std::wstring& text, const cchar_t& marker);
  int Refresh(WINDOW* window);
  int Erase(WINDOW* window);
  int Delete(WINDOW* window);
  void SetOutput(FILE* stream);
  void BeginFrame();
  void EndFrame();
  std::wstring ScreenGlyph(int y, int x);

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
    if (TextGlyph::Enabled()) Prepare(p_Window, p_Y, p_X, Width(std::wstring(p_Text, limit)));
    for (size_t position = 0; position < limit;)
    {
      if (TextGlyph::Enabled())
      {
        const auto cluster = TextGlyph::Next(p_Text, position, limit);
        if (cluster.paint)
        {
          int y = 0, x = 0;
          getyx(p_Window, y, x);
          if (x + cluster.width > getmaxx(p_Window)) break;
          cchar_t marker = {};
          const wchar_t base[] = {cluster.width == 1 ? L'.' : L'\u3000', L'\0'};
          attr_t attributes = 0;
          short color = 0;
          wattr_get(p_Window, &attributes, &color, nullptr);
          if (setcchar(&marker, base, attributes | A_PROTECT, color, nullptr) == ERR) return ERR;
          Record(p_Window, y, x, std::wstring(p_Text + position, cluster.end - position), marker);
          if (wadd_wch(p_Window, &marker) == ERR) return ERR;
          position = cluster.end;
          continue;
        }
      }
      const wchar_t character = p_Text[position];
      // Curses counts joined emoji as separate spacing characters. Terminals
      // can merge them, or widen a narrow symbol after VS16, leaving their
      // real cursor behind curses' cursor. Draw terminal-safe components at
      // their default widths. This affects display only, including the editor.
      if (character == L'\u200d' || character == L'\ufe0e' || character == L'\ufe0f' ||
          character == L'\u20e3' || (character >= L'\U000e0020' && character <= L'\U000e007f'))
      {
        ++position;
        continue;
      }
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
      if (TextGlyph::Enabled())
      {
        const auto cluster = TextGlyph::Next(p_Text.c_str(), position, p_Text.size());
        width += cluster.width;
        position = cluster.end;
        continue;
      }
      width += std::max(0, wcwidth(p_Text[position]));
      position = GlyphEnd(p_Text.c_str(), position, p_Text.size());
    }
    return width;
  }
}
