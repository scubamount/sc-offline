# Security policy

## Reporting a vulnerability

**Report privately, not in a public issue.** Use GitHub's private reporting: [Report a vulnerability](https://github.com/scubamount/sc-offline/security/advisories/new) (Security tab → **Report a vulnerability**).

Include what you found, the version (`sc-offline.exe status`), steps to reproduce, and what an attacker could do with it. We'll reply on the advisory, fix it in a new release, and credit you unless you'd rather not be named. This is a volunteer fan project: there's no bug bounty.

## Supported versions

Only the [latest release](https://github.com/scubamount/sc-offline/releases/latest) gets security fixes. The launcher updates itself; run `sc-offline.exe update` to get the newest one.

## In scope

- The self-update: installing files that don't match the release's `manifest.json`, writing outside the install folder, downgrades, or getting around the checks described in [Updates](docs/launcher.md#updates).
- The administrator helper: anything that lets it do more than its listed steps, or lets another program use it.
- PC changes that aren't undone, or that touch settings the user made themselves (hosts file, firewall rules, renamed files; see [PC changes](docs/launcher.md#pc-changes)).
- Crash reports or logs leaking personal data the redaction should remove.
- Release integrity: a published zip or `dinput8.dll` that doesn't match its SHA-256.

## Out of scope

- Account bans or anything Cloud Imperium Games does about modding. Using the mod is at your own risk; see the [README](README.md).
- Crashes and game bugs with no security impact: use the [Bug report](https://github.com/scubamount/sc-offline/issues/new/choose) form.
- Problems in Star Citizen itself. Report those to Cloud Imperium Games.
