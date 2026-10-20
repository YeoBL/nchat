// No terminal commands, personal data, or filesystem mutations.
#include "../src/terminalbadge.h"
#include <cassert>
#include <iostream>

int main()
{
  const char* names[] = { "NCHAT_KITTY_CONFIG_DIR", "XDG_CONFIG_HOME" };
  std::string saved[2];
  bool existed[2];
  for (int i = 0; i < 2; ++i)
  {
    const char* value = getenv(names[i]);
    existed[i] = value != nullptr;
    if (value) saved[i] = value;
  }
  unsetenv("NCHAT_KITTY_CONFIG_DIR");
  unsetenv("XDG_CONFIG_HOME");
  const char* taskUserHome = getenv("HOME");
  if (taskUserHome && *taskUserHome)
    assert(KittyBadgeConfigDir() == std::string(taskUserHome) + "/.config/kitty");
  setenv("XDG_CONFIG_HOME", "/tmp/nchat-portable-config", 1);
  assert(KittyBadgeConfigDir() == "/tmp/nchat-portable-config/kitty");
  setenv("NCHAT_KITTY_CONFIG_DIR", "/tmp/nchat-portable-kitty", 1);
  assert(KittyBadgeConfigDir() == "/tmp/nchat-portable-kitty");
  assert(KittyBadgeJsonString("a\"b\\c\nd") == "a\\\"b\\\\c\\u000ad");
  setenv("NCHAT_KITTY_CONFIG_DIR", "/dev/null/nchat-helper-test", 1);
  assert(!SetKittyBadge(true)); // Impossible directory: no terminal write.
  for (int i = 0; i < 2; ++i)
    if (existed[i]) setenv(names[i], saved[i].c_str(), 1); else unsetenv(names[i]);
  std::cout << "portable helper paths and JSON escaping passed\n";
}
