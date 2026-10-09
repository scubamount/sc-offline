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
- Co-presence over a LAN or VPN (planned, not in a release yet): anything a hostile peer can do by sending packets. That includes crashes, out-of-bounds reads or writes, messages accepted without authentication or validation, one peer posing as another, running commands on another player's PC, or reaching addresses outside the LAN or VPN ranges the player allowed.
- Local bridges to other games (planned, not in a release yet): shared-memory reads or writes outside a channel's bounds, or another local process using a channel for more than its declared messages.
- Anything in sc-offline that connects to Cloud Imperium Games' servers or online services, sends telemetry, weakens or skips anti-cheat or signature checks, touches accounts or entitlements, or forces the game's host type or network context. These are banned outright ([CONTRIBUTING](CONTRIBUTING.md#what-fits-this-project)); report them here.

## Out of scope

- Account bans or anything Cloud Imperium Games does about modding. Using the mod is at your own risk; see the [README](README.md).
- Crashes and game bugs with no security impact: use the [Bug report](https://github.com/scubamount/sc-offline/issues/new/choose) form.
- Problems in Star Citizen itself. Report those to Cloud Imperium Games.
