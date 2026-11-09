// uidialog.cpp
//
// Copyright (c) 2019-2025 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "uidialog.h"

#include "strutil.h"
#include "uicolorconfig.h"
#include "uimodel.h"
#include "uihistoryview.h"
#include "uiview.h"

UiDialog::UiDialog(const UiDialogParams& p_Params)
  : m_Model(p_Params.model)
  , m_Title(p_Params.title)
  , m_WReq(p_Params.wReq)
  , m_HReq(p_Params.hReq)
{
  UiHistoryView::SuspendThumbnails();
  Init();
  curs_set(0);
}

UiDialog::~UiDialog()
{
  Cleanup();
  UiHistoryView::ResumeThumbnails();
  curs_set(1);
}

void UiDialog::Init()
{
  int screenW = m_Model->GetScreenWidth();
  int screenH = m_Model->GetScreenHeight();
  int w = (m_WReq > 1.0f) ? m_WReq : (screenW * m_WReq);
  int h = (m_HReq > 1.0f) ? m_HReq : (screenH * m_HReq);
  int x = (screenW - w) / 2;
  int y = (screenH - h) / 3;

  m_BorderWin = newwin(h, w, y, x);

  m_W = w - 4;
  m_H = h - 2;
  m_X = x + 2;
  m_Y = y + 1;

  DrawBorder();

  m_Win = newwin(m_H, m_W, m_Y, m_X);
}

void UiDialog::Cleanup()
{
  UiCursesText::Delete(m_Win);
  UiCursesText::Delete(m_BorderWin);
}

void UiDialog::SetFooter(const std::string& p_Footer)
{
  m_Footer = p_Footer;
  DrawBorder();
}

void UiDialog::DrawBorder()
{
  static int colorPair = UiColorConfig::GetColorPair("dialog_color");
  static int attribute = UiColorConfig::GetAttribute("dialog_attr");

  UiCursesText::Erase(m_BorderWin);
  wbkgd(m_BorderWin, colorPair | ' ');
  wattron(m_BorderWin, attribute | colorPair);

  wborder(m_BorderWin, 0, 0, 0, 0, 0, 0, 0, 0);
  const int maxTextWidth = m_W - 2;

  std::wstring wtitle = L" " + StrUtil::TrimWString(StrUtil::ToWString(m_Title), maxTextWidth) + L" ";
  int titlex = std::max(0, (m_W - StrUtil::WStringWidth(wtitle)) / 2) + 2;
  int titlew = wtitle.size();
  UiCursesText::Draw(m_BorderWin, 0, titlex, wtitle.c_str(), titlew);

  if (!m_Footer.empty())
  {
    std::wstring wfooter = L" " + StrUtil::TrimWString(StrUtil::ToWString(m_Footer), maxTextWidth) + L" ";
    int footerx = std::max(0, (m_W - StrUtil::WStringWidth(wfooter)) / 2) + 2;
    int footerw = wfooter.size();
    UiCursesText::Draw(m_BorderWin, m_H + 1, footerx, wfooter.c_str(), footerw);
  }

  wattroff(m_BorderWin, attribute | colorPair);
  UiCursesText::Refresh(m_BorderWin);
}
