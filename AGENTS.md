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

[VERSION](VERSION) holds the version `X.Y.Z` of the last release. It is the only copy that the
build reads (the documents that name the version are listed in `.github/app-release.json`); CMake reads
it (`cmake/QarOgreVersion.cmake`) for `project()`, the package version and
`qar_ar_core/version.hpp`. It changes only in a release commit, which CI makes. The full
version of a build is derived, never typed:

| HEAD | Version | Example (`VERSION` is `0.2.0`) |
|---|---|---|
| tagged `vX.Y.Z` (the release commit) | `X.Y.Z` | `0.2.0` |
| any other commit | `X.Y.(Z+1)-dev.<commits since the last v* tag>+<short sha>` | `0.2.1-dev.4+1a2b3c4d` |
| no git checkout (source archive) | `X.Y.(Z+1)-dev` | `0.2.1-dev` |

- A build of any other commit reports the next patch, so it sorts after the release it builds on
  and before the next one. The package version stays `X.Y.Z` of `VERSION`.
- A tag whose `X.Y.Z` differs from `VERSION` stops the CMake configure. `-DQAR_OGRE_ADAPTER_VERSION_FULL=<version>`
  replaces the derived part: a release `X.Y.Z` must equal `VERSION`, a `-dev` or pull request
  `-pr.N` version must be the next patch `X.Y.(Z+1)`.
- The version is fixed when CMake configures: configure again after a new commit or tag.
- Semantic versioning: a `MINOR` version adds features and tries hard not to break users; when it
  must, the change is under **Breaking** with its migration step, and a removal is deprecated at
  least one minor version earlier where possible. A `PATCH` version only fixes. While the major
  version is 0, the CMake package accepts a request only for the same minor version.
- Tags go on `main`; there are no release branches and no release candidates.
- This repository follows the shared QAROS versioning scheme: [ADR-0001: One versioning scheme for every QAROS repository](https://github.com/Quaternar/qaros/blob/main/docs/adr/0001-one-versioning-scheme.md),
  with the release on every merge of [ADR-0002: QAROS apps release on every merge that changes behaviour](https://github.com/Quaternar/qaros/blob/main/docs/adr/0002-app-release-on-merge.md).

Every merge to `main` that changes behaviour is a release, and CI makes it
(`.github/workflows/app-release.yml`): it writes the new version into `VERSION` and the other files
listed in `.github/app-release.json`, moves the entries of `## [Unreleased]` under
`## [X.Y.Z] - YYYY-MM-DD`, commits `chore(release): vX.Y.Z`, tags it `vX.Y.Z`, pushes both and
publishes a GitHub release with the changelog section as its notes. Nobody tags or edits `VERSION`
by hand.

A `release:*` label on the pull request decides the bump; without one, its title does:

| Label | Title | Bump |
|---|---|---|
| `release:major` | | major; the only way to `1.0.0` |
| | `feat!: ...`, `fix(x)!: ...` or `BREAKING` in the title | major, minor below `1.0.0` |
| `release:minor` | `feat: ...` | minor |
| `release:patch` | `fix: ...`, `perf: ...`, `revert: ...` | patch |
| `release:none` | `docs`, `ci`, `chore`, `test`, `refactor`, `style`, `build` | none: releases nothing |

- The check **Release bump** on every pull request shows the version that merging releases. It
  fails when the title is not a Conventional Commit of a known type and no `release:*` label is
  set, or when two `release:*` labels are set.
- The changelog sets a floor: a release ships every `Unreleased` entry, so `Breaking` or `Removed`
  entries make it at least a breaking bump, `Features`, `Enhancements` or `Deprecated` at least a
  minor, `Bug fixes` or `Security` at least a patch. The floor applies only when the merge
  releases at all.
- To merge without releasing (several pull requests that only make sense together), label them
  `release:none`; the next releasing merge ships them all.
- A release never moves: a broken release is fixed by the next one.
