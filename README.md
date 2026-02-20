# Rust MultiInstance Skins Downloader
## Why
When you need to download 1k+ skins from skinboxes/skinner and you have skins id list its really faster to download using steamcmd via multiinstances than via steam that has a lot od delays and rate limits or similar.
## How to use
1. Make folder (anywhere) and put .exe files in it.
2. Download steamcmd and put it in the same folder. [Steamcmd Link](https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip)
3. Edit ImportedSkins.json and put there skins (might be config or whatever - script searchers for "<id>":  example: "490217825": ).
4. Run Downloader2_2.exe (i use 100 instances cuz why not) - it silently run multiple steamcmd scripts in background, status update when the instances are finishig or its broken idk.
5. You can close it faster but then i use cleanup_instances.exe to move unmoved files to rust_workshop folder from instances folder and clean them up.
6. Then move all folders/skins from "rust_skins_downloader\rust_workshop\steamapps\workshop\content\252490" to your steam rust workshop folder (for me "C:\Program Files (x86)\Steam\steamapps\workshop\content\252490").
7. Run acfupdate.exe to update manifest data off instaled files (or do it manually - there is file in \rust_workshop\steamapps\workshop\appworkshop_252490.acf).
8. All done.
