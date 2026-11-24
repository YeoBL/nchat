// uicontroller.cpp
//
// Copyright (c) 2019-2023 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "uicontroller.h"

#include <cwchar>

#include <unistd.h>
#include <sys/select.h>

#include "uikeyinput.h"

UiController::UiController()
{
}

UiController::~UiController()
{
}

void UiController::Init()
{
}

void UiController::Cleanup()
{
}

wint_t UiController::GetKey(int p_TimeOutMs)
{
  // Curses may already hold keys even when the OS input descriptor is empty.
  // Check that queue first so rapid input never waits for another terminal event.
  wint_t key = 0;
  const int previousDelay = wgetdelay(stdscr);
  wtimeout(stdscr, 0);
  const int buffered = UiKeyInput::GetWch(&key);
  wtimeout(stdscr, previousDelay);
  if (buffered != ERR) return key;

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  int maxfd = STDIN_FILENO;
  struct timeval tv = { (p_TimeOutMs / 1000), (p_TimeOutMs % 1000) * 1000 };
  select(maxfd + 1, &fds, NULL, NULL, &tv); // ignore select() rv to get resize events
  if (FD_ISSET(STDIN_FILENO, &fds))
  {
    UiKeyInput::GetWch(&key);
  }

  return key;
}
