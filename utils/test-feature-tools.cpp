#include "featureutil.h"

#include <cassert>
#include <cstdlib>
#include <ctime>

int main()
{
  setenv("TZ", "Europe/London", 1);
  tzset();
  int64_t start = 0, end = 0;
  assert(FeatureUtil::DateBound("", false, start) && start == 0);
  assert(FeatureUtil::DateBound("2026-03-29", false, start));
  assert(FeatureUtil::DateBound("2026-03-29", true, end));
  assert(end - start + 1 == 23LL * 60 * 60 * 1000);
  assert(FeatureUtil::DateBound("2026-10-25", false, start));
  assert(FeatureUtil::DateBound("2026-10-25", true, end));
  assert(end - start + 1 == 25LL * 60 * 60 * 1000);
  assert(!FeatureUtil::DateBound("2026-02-30", false, start));
  assert(!FeatureUtil::DateBound("2026-10-20junk", false, start));
  assert(!FeatureUtil::DateBound("2026-1-1", false, start));
  assert(FeatureUtil::SafeUrl("https://example.test/path?q=1"));
  assert(FeatureUtil::SafeUrl("tg://resolve?domain=example"));
  assert(!FeatureUtil::SafeUrl("file:///etc/passwd"));
  assert(!FeatureUtil::SafeUrl("javascript:alert(1)"));
  assert(!FeatureUtil::SafeUrl("https://example.test/\ncommand"));
}
