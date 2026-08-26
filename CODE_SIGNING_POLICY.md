# Code signing policy

## Project and scope

Codex-Quota-Bar is an independent, open-source Windows desktop utility maintained at
<https://github.com/xiumu-ops/codex-quota-bar>. It is not affiliated with or endorsed
by OpenAI.

This policy covers official stable Windows releases published from that repository.
The signed release set consists of:

- `Codex-Quota-Bar.exe`, the application;
- `Uninstall.exe`, the uninstaller embedded in the installer; and
- `Codex-Quota-Bar_version_<version>.exe`, the distributable installer.

The accompanying `.sha256` file is a checksum and is not an executable signing target.
Development builds, forks, locally rebuilt binaries, and pull-request artifacts are not
official signed releases.

## Signing provider and current status

Version 2.5.1 is the unsigned application release prepared for SignPath Foundation
review. Until enrollment and CI integration are complete, no project release is
represented as SignPath-signed. Users can confirm the current state through the
Windows signature properties or `Get-AuthenticodeSignature`.

After approval, signed release pages will carry this disclosure:

> Free code signing provided by [SignPath.io](https://signpath.io/), certificate by
> [SignPath Foundation](https://signpath.org/).

The signing private key is held by the signing provider and is never stored in this
repository, in GitHub Actions secrets, or on a maintainer workstation.

## Build and release controls

Official releases must meet all of the following requirements:

1. Source and build scripts are available under the repository's MIT license.
2. The release is built by the repository's GitHub Actions workflow on a GitHub-hosted
   Windows runner.
3. The semantic release tag `vX.Y.Z` exactly matches the project version declared in
   `CMakeLists.txt`.
4. Automated unit, integration, IPC, Hook, installation, and uninstallation tests pass.
5. The installer SHA-256 checksum is calculated after all signing operations finish.
6. Every production signing request requires explicit approval by the signing approver.
7. Signed assets are published only through the repository's GitHub Releases page.

The release workflow and build definition are reviewed as security-sensitive code.
Unsigned CI artifacts are retained only as temporary build outputs and are not
represented as SignPath-signed releases.

## Project roles

- **Committer and reviewer:** [xiumu-ops](https://github.com/xiumu-ops) maintains the
  repository and reviews changes submitted by other contributors before merge.
- **Signing approver:** [xiumu-ops](https://github.com/xiumu-ops) verifies the source
  revision, version, successful workflow results, and expected artifacts before
  approving a production signing request.

All maintainers and signing approvers must enable multi-factor authentication for both
GitHub and SignPath accounts. A contributor may not approve their own unreviewed
external contribution merely because it affects the release workflow.

## User-visible system changes

The installer and application make only the documented current-user changes:

- install application and uninstaller files under the selected `Codex-Quota-Bar`
  directory;
- create a current-user Start menu shortcut and HKCU uninstall registration;
- add three managed lifecycle handlers to the user's Codex `hooks.json`;
- optionally add an HKCU `Run` value when the user enables companion mode; and
- store settings and bounded diagnostic logs in the installation's `data` directory.

The uninstaller removes the managed Hook entries and companion startup value before
removing program files. The user may choose whether local settings and logs are retained.
Existing unrelated Codex Hooks and metadata are preserved. See [PRIVACY.md](PRIVACY.md)
for the complete data-handling statement.

## Incident response

If an official signed artifact is suspected of compromise or policy violation, the
maintainer will stop further signing, remove the affected download when appropriate,
investigate the source and build provenance, notify SignPath, and request certificate
revocation when required. A corrected release will use a new version and will document
the incident and remediation.

Security concerns can be reported privately through GitHub's security advisory feature:
<https://github.com/xiumu-ops/codex-quota-bar/security/advisories/new>.
