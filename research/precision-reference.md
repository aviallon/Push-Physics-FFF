# Precision (Nexus 72347, github.com/ersh1/Precision) — what it validates for PushAside

Read at commit from `main` (shallow clone, 2026-09-23). License: **GPL-3.0-or-later WITH Modding
Exception** — the same as this project, so ideas/code may be reused with attribution.

## It is NOT the same mechanic as ours

- Its collision work is **weapon-vs-world/actor sweeps**: `hkpAllCdPointCollector` + `AddCdPoint`
  (`src/PendingHit.cpp`), and a **world contact listener** registered on
  `hkpWorld::contactListeners` (`src/Havok/Havok.cpp`, `src/Havok/ContactListener.cpp`).
- Features are attack collision, hitstop, recoil, trails, iframes — **no character-vs-character
  pushing**, and no `hkpCharacterProxyListener` override anywhere.
- Consequence: it does not answer our slot-4 question, and its contact channel (world contacts)
  is the wrong channel for character phantoms. Our manifold scan (via
  `ProcessConstraintsCallback`, empirically ~240/s) remains the detection path.

## What it does validate, exactly

1. **Character-controller accessor** — `GetCharProxyController(Actor*)` is
   `skyrim_cast<RE::bhkCharProxyController*>(a_actor->GetCharController())`
   (`src/Havok/Havok.cpp:37`). Same accessor PushAside now uses.
2. **The MCM mechanism, precisely as planned.** `scripts/source/Precision_MCM.psc` is 18 lines:
   ```papyrus
   ScriptName Precision_MCM Extends MCM_ConfigBase
   ...
   Event OnConfigClose() native
   Event OnConfigOpen()   GetModSettingInt("uSweepAttackMode:AttackCollisions") ...
   Event OnSettingChange(String a_ID)  GetModSettingInt(a_ID) ... RefreshMenu()
   ```
   So: **MCM Helper**, JSON-driven menu (`config.json` supplies the widgets; the script only adds
   helpers), script extends `MCM_ConfigBase`, settings addressed as `"<key>:<Section>"`.
3. **How the C++ side reads live settings** (`src/Settings.cpp:601-604`):
   ```cpp
   CSimpleIniA mcm; mcm.SetUnicode(); mcm.LoadFile(path.string().c_str());
   ```
   with
   ```cpp
   constexpr auto defaultSettingsPath = L"Data/MCM/Config/Precision/settings.ini";
   constexpr auto mcmPath             = L"Data/MCM/Settings/Precision.ini";
   ```
   i.e. defaults from `MCM/Config/<mod>/settings.ini`, **live user values from
   `MCM/Settings/<mod>.ini`**, parsed with SimpleIni. That is exactly the PushAside plan: poll
   `Data/MCM/Settings/PushAside.ini`, `[General] bEnabled=1`.

## What it does NOT do (so our pipeline goes further)

- The repo contains **no `.esp` and no compiled `.pex`** — only `scripts/source/*.psc`. Release
  packaging (`cmake/packaging.cmake`, CPack ZIP + `src/CMakeLists.txt` install rules) installs the
  **DLL only**. The quest ESP is produced/distributed outside the repo.
- Therefore our Nix-native pipeline (generate `PushAside.esp` + compile `.pex` + emit the MCM files
  + package the FOMOD, reproducibly) is strictly more automated than the reference mod.