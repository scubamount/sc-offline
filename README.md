# ChrisWareOffline

If you do not want to use the source and just want to play the offline mod get it here https://github.com/cloudyyrust/ChrisWareOffline

Play Star Citizen offline on your own PC

You need to own Star Citizen to use this

Join the Discord https://discord.gg/979RRuMjDP

We encourage people to help on the project and add more modding tools

If you like it leave a star on the repo

## Warning

This mod could maybe cause a ban. Use it at your own risk. It is for offline single player only.

The mod does not go in your game folder the launcher script copies it in when you play and removes it when you close the game.

## Step 1 Turn off Easy Anti Cheat

The mod will not run while Easy Anti Cheat is on. You do this once.

### Rename the anti cheat so it can not start

Open PowerShell as administrator and run this

```powershell
ren "C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe" EasyAntiCheat_EOS.exe.bak
```

This renames C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe to EasyAntiCheat_EOS.exe.bak. Without the .exe the Easy Anti Cheat service can not start so nothing attaches to the game.

### Block the anti cheat download server

This stops the launcher from downloading new anti cheat files.

1. Open Notepad as administrator
2. Open C:\Windows\System32\drivers\etc\hosts
3. Add this line at the bottom and save

```
127.0.0.1 modules-cdn.eac-prod.on.epicgames.com
```

4. Open PowerShell and run this

```powershell
ipconfig /flushdns
```

With the anti cheat off the game can not join online servers.

## Step 2 Build the mod

You need Visual Studio 2026 with the Desktop development with C++ workload.

1. Open ChrisWareOffline.slnx
2. Pick Release and x64
3. Build the solution

The mod is built to x64\Release\dinput8.dll

If your game is not installed in C:\Program Files\Roberts Space Industries\StarCitizen\LIVE then right click launch_offline.bat and pick Edit. Change the SC_BIN line at the top to your own LIVE\Bin64 folder and save.

## Step 3 Play

1. Close the RSI Launcher and the game
2. Double click launch_offline.bat
3. If it says it can not copy the mod then right click launch_offline.bat and pick Run as administrator
4. Wait for the game to load you in
5. Press M to open the menu

Leave the black script window open while you play. When you close the game it removes the mod from your game folder.

## Update the mod

1. Close the game
2. Get the new source with git pull or download it again
3. Build it again like in Step 2

If you download a fresh copy then copy wallet.txt and spawn.txt from the old data folder into the new one to keep your money and saved spot.

## Play online again

1. Make sure the game is closed and dinput8.dll is not in your LIVE\Bin64 folder
2. Open PowerShell as administrator and run this

```powershell
ren "C:\Program Files (x86)\EasyAntiCheat_EOS\EasyAntiCheat_EOS.exe.bak" EasyAntiCheat_EOS.exe
```

3. Remove the modules-cdn.eac-prod.on.epicgames.com line from your hosts file
4. Run this

```powershell
ipconfig /flushdns
```

Everything is back to normal.

## Important

If the script window gets closed early delete dinput8.dll from your LIVE\Bin64 folder yourself before you play online.

A game update can break the mod until it is updated.

A log of what the mod does is saved to mod.log in the data folder.

## Disclaimer

AI was used in the making of this project for a bit.

This is a fan project. It is not made by or affiliated with Cloud Imperium Games or Roberts Space Industries. Star Citizen is a trademark of Cloud Imperium Games. Use it at your own risk.
