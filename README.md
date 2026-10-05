# ChrisWareOffline

![A Vanduul holding a gun on a desert planet, with a line of Vanduul and a large ship behind it](images/screenshot.webp)

An offline, single-player mod menu for Star Citizen.

> [!WARNING]
> This mod may result in a ban. Use it at your own risk. It is for **offline single player only**.

**Jump to:** [Setup](#setup) · [Play](#play) · [Controls](#controls) · [Update](#update) · [Go back online](#go-back-online) · [Notes](#notes)

---

## Setup

### 1. Disable Easy Anti-Cheat

The mod will not run while Easy Anti-Cheat (EAC) is active. You only do this once.

> [!NOTE]
> With EAC disabled, the game cannot join online servers. See [Go back online](#go-back-online) to undo these steps.

**a. Rename the EAC executable**

Without the `.exe`, the EAC service cannot start, so nothing attaches to the game. Open PowerShell as administrator and run:

```powershell
ren "C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe" EasyAntiCheat_EOS.exe.bak
```

**b. Block the EAC download server**

This stops the launcher from downloading fresh EAC files.

1. Open Notepad as administrator.
2. Open `C:\Windows\System32\drivers\etc\hosts`.
3. Add this line at the bottom and save:

   ```text
   127.0.0.1 modules-cdn.eac-prod.on.epicgames.com
   ```

4. Open PowerShell and flush the DNS cache:

   ```powershell
   ipconfig /flushdns
   ```

### 2. Install the mod

1. Click the green **Code** button at the top of this page and choose **Download ZIP**.
2. Right-click the ZIP and choose **Extract All**.
3. Place the `ChrisWareOffline-main` folder anywhere you like, such as your Desktop.
4. Keep all files together in that folder.

The mod does **not** go in your game folder. `launch_offline.bat` copies it in when you play and removes it when you close the game.

**Custom install path:** if your game is not installed at the path below, right-click `launch_offline.bat`, choose **Edit**, and change the `SC_BIN` line at the top to your own `LIVE\Bin64` folder.

```text
C:\Program Files\Roberts Space Industries\StarCitizen\LIVE
```

---

## Play

1. Close the RSI Launcher and the game.
2. Double-click `launch_offline.bat`.
   - If it reports that it cannot copy the mod, right-click the file and choose **Run as administrator**.
3. Wait for the game to load you in.
4. Press **M** to open the menu.

> [!IMPORTANT]
> Leave the black script window open while you play. When you close the game, the script removes the mod from your game folder.

---

## Controls

| Key  | Action                                |
| ---- | ------------------------------------- |
| `M`  | Open / close the menu                 |
| `F7` | Save your current position            |
| `F8` | Teleport to your saved position       |
| `F6` | Turn build mode on / off              |

---

## Update

1. Close the game.
2. **Optional:** to keep your money and saved spot, copy `wallet.txt` and `spawn.txt` out of the old `data` folder first.
3. Delete the old version of the mod.
4. Download the new version (green **Code** button, then **Download ZIP**) and extract it.
5. Paste `wallet.txt` and `spawn.txt` into the new `data` folder.
6. Play as usual.

---

## Go back online

1. Make sure the game is closed and `dinput8.dll` is not in your `LIVE\Bin64` folder.
2. Open PowerShell as administrator and restore the EAC executable:

   ```powershell
   ren "C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe.bak" EasyAntiCheat_EOS.exe
   ```

3. Remove the `modules-cdn.eac-prod.on.epicgames.com` line from your hosts file.
4. Flush the DNS cache:

   ```powershell
   ipconfig /flushdns
   ```

Everything is back to normal.

---

## Notes

> [!CAUTION]
> If the script window is closed early, delete `dinput8.dll` from your `LIVE\Bin64` folder yourself **before** playing online.

- A game update can break the mod until the mod is updated.
- A log of what the mod does is saved to `mod.log` in the `data` folder.

---

## Disclaimer

AI was used in the making of this project, in a limited capacity.

This is a fan project. It is not made by or affiliated with Cloud Imperium Games or Roberts Space Industries. Star Citizen is a trademark of Cloud Imperium Games. Use it at your own risk.
