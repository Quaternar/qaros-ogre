# Rules for changing qaros-ogre

For everyone who changes this repository, people and coding agents alike. How to build and test is
in [docs/DEVELOPING.md](docs/DEVELOPING.md) and [docs/TESTING.md](docs/TESTING.md).

## Public repository

- This repository is public. Commits, pull requests, issues, code comments and documentation never
  cite internal documents (internal architecture decision records, private notes or paths); link a
  public issue or pull request instead, here or in
  [Quaternar/Quaternar](https://github.com/Quaternar/Quaternar/issues).
- No AI attribution anywhere: no `Co-Authored-By` trailer for a tool, no "Generated with" line, no
  tool or assistant name in commits, pull requests or issues.
- Commit subjects follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/)
  (`feat:`, `fix:`, `docs:`, `build:`, `refactor:`, `test:`, `ci:`; `!` marks a breaking change),
  with a short bullet list as the body.

## Changelog

[CHANGELOG.md](CHANGELOG.md) is written with every change, not at release time:

- Every change a user can notice gets one entry under `## [Unreleased]`, in the same commit as the
  change. A user is whoever builds against the CMake package, runs the examples or reads the
  documentation: a header, type, function, default, CMake variable or target, install layout,
  environment variable, example option or documented behaviour.
- One line per entry: what changed for the user, and what they must do if anything, followed by a
  link to the related GitHub issue or pull request when there is one, as a full markdown link, for
  example `([#5](https://github.com/Quaternar/qaros-ogre/pull/5))`. Issues of
  `Quaternar/Quaternar` may be linked as well.
- Sections in this order, only those that have entries: `### Breaking`, `### Features`,
  `### Enhancements`, `### Bug fixes`, `### Deprecated`, `### Removed`, `### Security`.
- `Breaking` is anything that makes a user act: a removed or renamed API, a changed default, unit,
  frame or other assumption, a changed install step. The entry carries its migration step.
- No entry for refactors, tests, CI or internal build changes that a user cannot notice.
- A later commit that refines the same change amends its existing `Unreleased` entry.

## Versions and releases

[VERSION](VERSION) holds the next planned version `X.Y.Z` and is its only copy; CMake reads it
(`cmake/QarOgreVersion.cmake`) for `project()`, the package version and
`qar_ar_core/version.hpp`. The full version of a build is derived, never typed:

| HEAD | Version | Example |
|---|---|---|
| tagged `vX.Y.Z` | `X.Y.Z` | `0.3.0` |
| tagged `vX.Y.Z-rc.N` | `X.Y.Z-rc.N` | `0.3.0-rc.1` |
| any other commit | `X.Y.Z-dev.<commits since the last v* tag>+<short sha>` | `0.3.0-dev.4+1a2b3c4d` |
| no git checkout (source archive) | `X.Y.Z-dev` | `0.3.0-dev` |

- A tag whose `X.Y.Z` differs from `VERSION` stops the CMake configure. `-DQAR_OGRE_ADAPTER_VERSION_FULL=<version>`
  replaces the derived part; its `X.Y.Z` must equal `VERSION` too.
- The version is fixed when CMake configures: configure again after a new commit or tag.
- Semantic versioning: a `MINOR` version adds features and tries hard not to break users; when it
  must, the change is under **Breaking** with its migration step, and a removal is deprecated at
  least one minor version earlier where possible. A `PATCH` version only fixes. While the major
  version is 0, the CMake package accepts a request only for the same minor version.
- Tags go on `main`; there are no release branches.

To release `X.Y.Z`:

1. Optional release candidate: tag the commit to test `vX.Y.Z-rc.N` on `main` and push the tag.
2. Release commit: move the entries of `## [Unreleased]` under a new heading
   `## [X.Y.Z] - YYYY-MM-DD` and leave an empty `## [Unreleased]` above it. Change nothing else in
   this commit.
3. Tag that commit `vX.Y.Z` on `main` and push the tag.
4. Bump: in the next commit set `VERSION` to the next planned version (`X.(Y+1).0`), never in the
   tagged commit.
