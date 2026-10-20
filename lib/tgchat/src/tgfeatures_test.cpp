#include "tgfeatures_util.h"
#include <cassert>

int main()
{
  int64_t number = 0;
  assert(TgFeatureParseNumber("-100123", number) && number == -100123);
  assert(!TgFeatureParseNumber("23garbage", number));
  assert(!TgFeatureParseNumber("99999999999999999999999999999", number));
  int32_t timestamp = 0;
  assert(TgFeatureParseDate("2026-10-21", timestamp) && timestamp > 0);
  assert(!TgFeatureParseDate("2026-02-31", timestamp));
  assert(!TgFeatureParseDate("2026-10-21 trailing", timestamp));
  assert(TgFeaturePollPageHasMore(0, 12, 200)); // TDLib may return short pages.
  assert(!TgFeaturePollPageHasMore(0, 0, 200));
  assert(!TgFeaturePollPageHasMore(150, 50, 200));
}
