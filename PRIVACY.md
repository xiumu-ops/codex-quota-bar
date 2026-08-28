# Privacy policy

Last updated: August 26, 2026

## Summary

Codex-Quota-Bar is a local Windows desktop utility. The project maintainer does not
operate a telemetry, analytics, advertising, crash-reporting, update, or data-collection
service for the application. The application does not transmit personal information,
Codex credentials, account identifiers, usage responses, configuration, or diagnostic
logs to the maintainer.

Codex-Quota-Bar is an independent third-party project and is not affiliated with or
endorsed by OpenAI.

## Data processed in memory

To display account quota and usage information, the application launches the official
`codex app-server` installed on the user's computer and communicates with that local
child process over standard input and output. It processes the returned quota reset
times, percentages, reset-credit information, aggregate token usage, longest-chat
duration, and streak information in memory.

Codex-Quota-Bar does not ask for, copy, persist, or transmit Codex authentication tokens.
Any communication between the official Codex software and OpenAI is performed by that
software under the user's account and is governed by the applicable OpenAI terms and
privacy policy. Codex-Quota-Bar does not send these responses to a project-operated
server.

## Data stored locally

The application may store the following data in the selected installation root's
`data` directory, normally `%LOCALAPPDATA%\Codex-Quota-Bar\data`:

- `config-users.json`: interface scale, refresh interval, companion-mode preference, appearance, and
  window position;
- `diagnostic.log`: local timestamps, severity, process/thread identifiers, and bounded
  operational messages; and
- `diagnostic.previous.log`: one rotated previous diagnostic log.

Each diagnostic log is limited to 256 KiB. Logs are intentionally designed not to
contain raw App Server JSON, authentication tokens, token-level conversation content,
prompts, responses, or account identifiers. No quota or usage history database is
created by the application.

## Local system access and changes

The application and installer access or modify only what is needed for their documented
features:

- locate and launch the locally installed official Codex executable;
- enumerate relevant local processes when companion mode is enabled;
- read and structurally update the current user's Codex `hooks.json` to add three
  lifecycle handlers;
- create a current-user Start menu shortcut and HKCU uninstall registration; and
- add or remove an HKCU `Run` value when the user enables or disables companion mode.

The installer does not read or modify Codex `config.toml`. Hook updates preserve
unrelated handlers and metadata. The application does not inspect Codex conversation
files or upload local files.

## Network communication

Codex-Quota-Bar contains no direct telemetry, analytics, advertising, update-checking,
or maintainer-operated network client. Its quota synchronization is performed through
the locally launched official Codex App Server as described above. Downloading the
installer from GitHub and any network activity performed by GitHub, Windows, Codex, or
OpenAI are governed by those providers' respective policies.

## Retention and deletion

All application-managed persistent data remains on the user's computer. Interactive
uninstallation offers a choice to retain or delete the `data` directory. Quiet
uninstallation retains it by default. The user can delete the directory manually after
the application has exited. Disabling companion mode removes its startup value, and
uninstallation removes the lifecycle Hook entries managed by Codex-Quota-Bar.

## Third parties and children

The application is a general-purpose developer utility and is not directed to children.
The project maintainer does not sell or share user data. The application relies on the
user's separately installed Codex software; use of that software is subject to its
provider's policies.

## Changes and contact

Material privacy changes will be committed publicly and reflected by the date above.
Privacy questions may be submitted at
<https://github.com/xiumu-ops/codex-quota-bar/issues>.

