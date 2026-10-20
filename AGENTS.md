# Personal nchat fork

This checkout contains the user's custom nchat features. The personal GitHub
fork is `YeoBL/nchat`; `upstream` is the original `d99kris/nchat` repository.

## Versioning requirement

- Keep every completed customization under version control, including widget,
  terminal integration, helper scripts, tests, and documentation.
- Make a focused commit for each feature. Keep shared prerequisites and later
  bug fixes in clearly named separate commits. Do not combine unrelated features.
- Run relevant checks before committing. Record untestable service limitations
  honestly; never fabricate successful live tests or historical data.
- Preserve unrelated uncommitted work. Inspect the diff before staging and
  never discard the user's changes.
- Never commit account databases, chat exports, private message fixtures,
  downloaded media, credentials, authentication secrets, or build products.
- Future nchat changes should also be committed, with the feature/access guide
  updated when behavior or shortcuts change. Publish only to the personal fork,
  never to the original upstream repository.

The initial customization history is a reconstruction from an existing working
copy, not a claim that each feature was originally authored on its commit date.
