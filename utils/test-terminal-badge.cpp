// Must run in Kitty, or with STY pointing at its attached screen session.
#include "../src/terminalbadge.h"
int main(int argc, char**)
{
  for (int attempt = 0; attempt < 40; ++attempt)
  {
    if (SetKittyBadge(argc > 1)) return 0;
    usleep(50000);
  }
  return 1; // No acknowledgement from Kitty's real NSDockTile.
}
