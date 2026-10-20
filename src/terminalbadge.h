#pragma once

#ifdef __APPLE__
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <limits.h>
#include <libproc.h>
#include <unistd.h>

inline std::string KittyBadgeConfigDir()
{
  const char* explicitDir = getenv("NCHAT_KITTY_CONFIG_DIR");
  if (explicitDir && *explicitDir) return explicitDir;
  const char* configHome = getenv("XDG_CONFIG_HOME");
  if (configHome && *configHome) return std::string(configHome) + "/kitty";
  const char* userHome = getenv("HOME");
  return userHome && *userHome ? std::string(userHome) + "/.config/kitty" : "";
}

inline std::string KittyBadgeJsonString(const std::string& value)
{
  std::string result;
  const char hex[] = "0123456789abcdef";
  for (unsigned char ch : value)
  {
    if (ch == '"' || ch == '\\') result += '\\';
    if (ch < 32)
    {
      result += "\\u00";
      result += hex[ch >> 4];
      result += hex[ch & 15];
    }
    else result += ch;
  }
  return result;
}

// Resolve screen's attached terminal afresh: no cached tty after reattachment.
inline std::string KittyBadgeTty()
{
  const char* sty = getenv("STY");
  if (!sty || !*sty) return "/dev/tty";
  char* end = nullptr;
  const long pid = strtol(sty, &end, 10);
  if (pid <= 0 || !end || *end != '.') return "";
  const int size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
  if (size <= 0) return "";
  std::vector<proc_fdinfo> fds(size / sizeof(proc_fdinfo) + 16);
  const int count = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), fds.size() * sizeof(proc_fdinfo));
  for (int i = 0; i < count / (int)sizeof(proc_fdinfo); ++i)
  {
    if (fds[i].proc_fdtype != PROX_FDTYPE_VNODE) continue;
    vnode_fdinfowithpath info{};
    if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof(info)) != sizeof(info)) continue;
    const std::string path(info.pvip.vip_path);
    if (path.compare(0, 9, "/dev/ttys") == 0) return path;
  }
  return ""; // Detached screen: retry after attachment, never a different tty.
}

inline bool SetKittyBadge(bool active)
{
  const std::string requestedDir = KittyBadgeConfigDir();
  char resolvedDir[PATH_MAX];
  if (requestedDir.empty() || !realpath(requestedDir.c_str(), resolvedDir)) return false;
  const std::string configDir(resolvedDir);
  const std::string helper = configDir + "/nchat_badge.py";
  if (configDir.empty() || access(helper.c_str(), R_OK) != 0) return false;
  static std::string nonce;
  static bool lastActive = false;
  if (nonce.empty() || lastActive != active)
  {
    nonce = std::to_string(getpid()) + ":" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
    lastActive = active;
  }
  const auto tty = KittyBadgeTty();
  if (tty.empty()) return false;
  const int fd = open(tty.c_str(), O_WRONLY | O_NONBLOCK | O_NOCTTY);
  if (fd < 0) return false;
  const std::string command = std::string("\033P@kitty-cmd{\"cmd\":\"kitten\",\"version\":[0,40,0],\"no_response\":true,"
    "\"payload\":{\"kitten\":\"") + KittyBadgeJsonString(helper) + "\",\"args\":[\"" +
    (active ? "1" : "0") + "\",\"" + nonce + "\"]}}\033\\";
  const auto written = write(fd, command.data(), command.size());
  close(fd);
  if (written != (ssize_t)command.size()) return false;
  // The acknowledgement is from Kitty's actual NSDockTile property. Nothing
  // is returned through terminal input, where nchat could mistake it for keys.
  std::ifstream status(configDir + "/nchat-badge-ack");
  std::string received, label;
  int requested = -1;
  status >> received >> requested >> label;
  return received == nonce && requested == (active ? 1 : 0) && label == (active ? "!" : "-");
}
#endif
