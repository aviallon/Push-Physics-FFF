# MCM Helper — exact specification for adding a minimal MCM to PushAside

Sources are MCM Helper's own GitHub repo/wiki (Exit-9B/Parapets, MIT, v1.6.3) and the SKSE/Nexus pages. Everything below is quoted or directly derived from those; URLs are at the end of each item and collected at the bottom.

---

## 1. File layout under `Data/`

| Purpose | Exact path |
|---|---|
| Menu layout (written by author) | `Data/MCM/Config/PushAside/config.json` |
| Setting DEFAULTS (written by author, optional) | `Data/MCM/Config/PushAside/settings.ini` |
| USER's live values (written by MCM Helper at runtime; **this is the file our SKSE plugin reads**) | `Data/MCM/Settings/PushAside.ini` |

Wiki "Home", Folder Structure, exact tree:

```
Data/
└── MCM/
    ├── Config/
    │   ├── Mod1/
    │   │   ├── config.json         (Required) Menu Layout
    │   │   ├── keybinds.json       (Optional) Keybind Definitions
    │   │   ├── settings.ini        (Optional) Mod Setting Defaults
    │   ├── Mod2/
    │   └── Mod3/
    └─── Settings/
         ├── Mod1.ini               Mod Settings for Mod1
         ├── Mod2.ini               Mod Settings for Mod2
         └── Mod3.ini               Mod Settings for Mod3
```

Quote ("Home", https://github.com/Exit-9B/MCM-Helper/wiki): "MCM menus live in their own directory in `MCM\Config\ModName`, while user settings are located in `MCM\Settings\ModName.ini`."

Quote ("Setting Types, Storage, and Persistence", https://github.com/Exit-9B/MCM-Helper/wiki/Setting-Types,-Storage,-and-Persistence): "All mod settings must be defined in `MCM\Config\ModName\settings.ini`. User settings live in `MCM\Settings\ModName.ini`. This file is automatically created and updated by the MCM."

Source confirmation of the runtime path and format (`src/SettingStore.cpp`, https://github.com/Exit-9B/MCM-Helper/blob/main/src/SettingStore.cpp):
- Defaults loaded from `"Data/MCM/Config" / modName / "settings.ini"` (`LoadDefaults`).
- User settings loaded from `std::filesystem::path modSettingsPath{ "Data/MCM/Settings" }` (`LoadUserSettings`).
- On change, `CommitModSetting` builds `settingsPath / (modName + ".ini")` and calls `ini.SetValue(sectionName, settingName, value)` then `ini.SaveFile(iniPath)`. The setting name is split on the FIRST `':'` into `settingName` (before) and `sectionName` (after); if there is no `':'` it logs "Section could not be resolved." and does NOT save.
- Values are serialised with `std::to_string(...)`, so booleans land as `1`/`0`.

So `Data/MCM/Settings/PushAside.ini` after the user flips the toggle looks like:

```ini
[General]
bEnabled=1
```

A C++ SKSE plugin can read that with SimpleIni (`CSimpleIniA`) or any INI parser; MCM Helper itself uses SimpleIni (`#include <SimpleIni.h>`).

---

## 2. Minimal `config.json`

### Required / optional top-level keys

From the published JSON Schema (`docs/config.schema.json`, https://github.com/Exit-9B/MCM-Helper/blob/main/docs/config.schema.json):
- `"required": [ "modName", "displayName" ]`
- plus `"allOf": [ { "$ref": "#/$defs/content-or-customContent" } ]` — i.e. the schema additionally requires exactly one of `content` or `customContent`.
- `minMcmVersion`: optional integer, `"minimum": 13`, "Version code for minimum compatible MCM Helper release".
- `cursorFillMode`: optional, enum `leftToRight` | `topToBottom`, default `leftToRight`.

From the runtime parser (`src/Json/ConfigHandler.cpp`, https://github.com/Exit-9B/MCM-Helper/blob/main/src/Json/ConfigHandler.cpp): only `modName` and `displayName` are enforced (`ReportError(ErrorType::MissingRequiredField, "modName")` / `"displayName"`). `minMcmVersion` is optional; if given and `i > MCM_VERSION_RELEASE` it errors with `"Config requires {} plugin version: {}"`.

Wiki, "Menu Layout" (https://github.com/Exit-9B/MCM-Helper/wiki/Menu-Layout):
- `modName` (required): "This value should be set to your plugin name, without the file extension. This value must be unique and must match your directory name `MCM\Config\ModName\`."
- `displayName` (required): "the name that will be displayed in the left panel in the MCM."
- `minMcmVersion` (optional): "the minimum MCM Helper version-code that your menu requires. If the user has an older version of MCM Helper installed, they will be required to update before they can access the menu."
- `content` (optional per wiki; required-or-customContent per schema): "Your MCM menu layout goes here."

For PushAside the correct `modName` is the plugin filename stem, e.g. `PushAside` for `PushAside.esp`. `ConfigHandler::String` hard-checks it: `"modName: "{}" did not match plugin"`.

### Cursor / version keys — the important correction

There is **no `cursor` field and no `version` field** in MCM Helper's `config.json`. Neither appears in the schema, and `ConfigHandler::Key` errors with `ReportError(ErrorType::InvalidKey, str)` on any unrecognised key. Adding `"cursor"` or `"version"` would therefore be an invalid config. The real fields are:
- **Cursor:** top-level `cursorFillMode` (`leftToRight` default / `topToBottom`), plus per-control `position` (integer; "Specifying `"position": 1` would place a control at the top of the right-hand column" — Menu Layout).
- **Version:** optional `minMcmVersion` only. For MCM Helper v1.6.3 the compile-time release code is **15** (`CMakeLists.txt`: `set(PLUGIN_VERSION 15)` bound as `MCM_VERSION_RELEASE`). Omit `minMcmVersion` for maximum compatibility, or set 15 to require 1.6.3.

(Version-code landmarks from the wiki: `pluginRequirements`/code 3 = v1.0.2, `GlobalValue`/code 6 = v1.1.0, `minMcmVersion`/code 9 = v1.2.2, `$schema`/code 13 = v1.4.0.)

### Exact minimal `Data/MCM/Config/PushAside/config.json`

One interactive toggle plus one non-interactive text line, on the main page:

```json
{
  "$schema": "https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/docs/config.schema.json",
  "modName": "PushAside",
  "displayName": "PushAside",
  "content": [
    {
      "type": "text",
      "text": "PushAside is installed.",
      "help": "Physics-based pushing is active. Use the toggle below to enable or disable it."
    },
    {
      "type": "toggle",
      "id": "bEnabled:General",
      "text": "Enable Push Aside",
      "help": "Turn physics-based pushing of characters on or off.",
      "valueOptions": {
        "sourceType": "ModSettingBool",
        "defaultValue": true
      }
    }
  ]
}
```

Notes:
- The `text` control is "a generic text/value pair" and "cannot be interacted with" unless given an `action` ("Control Types", https://github.com/Exit-9B/MCM-Helper/wiki/Control-Types). With no `action` and no `id` it is just an information line.
- The `toggle` needs an `id` in `"<settingName>:<Section>"` form, because `SettingStore` splits the id on `':'` and refuses to save without a section. The `id` is also what `OnSettingChange` reports ("The control must have the `id` property in order to receive this event" — Papyrus Integration).
- The matching default file `Data/MCM/Config/PushAside/settings.ini`:

```ini
[General]
bEnabled=1
```

### Variant: an explicitly named sub-page "PushAside"

The main page has no `pageDisplayName`; the menu's own label is `displayName`. If you specifically want a left-panel sub-page named "PushAside", use `pages` (wiki: "A page must have a display name (`pageDisplayName`)"):

```json
{
  "modName": "PushAside",
  "displayName": "PushAside",
  "content": [],
  "pages": [
    {
      "pageDisplayName": "PushAside",
      "content": [
        { "type": "text", "text": "PushAside is installed." },
        {
          "type": "toggle",
          "id": "bEnabled:General",
          "text": "Enable Push Aside",
          "valueOptions": { "sourceType": "ModSettingBool", "defaultValue": true }
        }
      ]
    }
  ]
}
```

Wiki guidance: "If your mod uses sub-pages, it is recommended that your main page either be blank or use custom content. The main page will not be selectable once a different page is selected." (Note: MCM Helper's own shipped SkyUI_SE demo puts `"cursorFillMode"` inside page objects, but the current schema's page object only declares `pageDisplayName` + content/customContent — a schema/demo discrepancy; don't rely on page-level cursorFillMode for a new mod.)

---

## 3. `valueOptions` / `sourceType` variants

Full list ("Setting Types, Storage, and Persistence"):
- `PropertyValueBool`, `PropertyValueInt`, `PropertyValueFloat` — read/write a **Papyrus Auto Property** on a script/form; requires `propertyName` (and optional `scriptName`, `sourceForm`; default form is the quest the config script is attached to).
- `ModSettingBool`, `ModSettingInt`, `ModSettingFloat` — read/write **MCM Helper mod settings**, persisted to INI files (see below).
- `GlobalValue` (added v1.1.0, code 6) — read/write a **GlobalVariable (TESGlobal)** form; "GlobalValues are stored internally as floats" and "are recommended if the setting should be saved with the savegame/character or is not suitable for storage outside the savegame."
- Text-string source types (`PropertyValueString`, `ModSettingString`) are marked `"deprecated": true` in the schema and "are used automatically for the appropriate controls depending on the `id` or `propertyName` properties."

Control → value type table (same wiki page):

| Control Type | Value Type |
|---|---|
| Toggle | Bool, Int |
| Slider | Float, Int |
| Enum | Int |
| Color | Int |
| Keymap | Int |
| Text | String |
| Menu | String |
| Input | String |

### ModSetting vs Global — which to use

- **ModSetting** reads defaults from `Data/MCM/Config/<ModName>/settings.ini` and writes the user's value to `Data/MCM/Settings/<ModName>.ini` — a **plain INI file**. This is exactly what an SKSE plugin wants: `sourceType: "ModSettingBool"` with `id: "bEnabled:General"` gives you `Data/MCM/Settings/PushAside.ini` → `[General] bEnabled=1`.
- **GlobalValue** mutates a TESGlobal in memory (saved in the savegame). It does **not** produce a file your plugin can poll; your plugin would instead have to resolve the TESGlobal form itself. Prefer ModSetting for PushAside.

### Is a quest/ESP or a GlobalVariable required?

- A **GlobalVariable is NOT required** for `ModSetting*`. GlobalValue is the only source type that needs a TESGlobal form (referenced in `valueOptions`' `sourceForm`).
- However, **a quest with a script is required for every MCM Helper menu, ModSetting included.** "A configuration menu must at minimum, have a quest with an appropriate Papyrus script attached, and have a `config.json` file defined." ("Home"). The script "have to extend `MCM_ConfigBase`, which is provided by the MCM Helper SDK. This script must be bound to a quest in the Creation Kit." ("Creating a Config Script").
- Minimal ESP-side requirement ("Creating a Config Script"):
  1. A script `Scriptname PushAside_MCM extends MCM_ConfigBase` (`data/SDK/Scripts` / `scripts/private/MCM_ConfigBase.psc` ships it).
  2. A quest (start-game-enabled) with that script attached.
  3. Quest Aliases → new Reference Alias named `PlayerAlias`, Fill Type = Specific Reference, Cell `any`, Ref `PlayerRef`, with script `SKI_PlayerLoadGameAlias` in its Scripts list.
  The quest lives in `PushAside.esp` (an ESP/ESM/ESL is unavoidable).

---

## 4. Does MCM Helper require an ESP for a purely ini-backed menu?

**Yes — an ESP (or ESL/ESM) is required. Shipping only `Data/MCM/...` plus hard requirements on MCM Helper + SkyUI will NOT show a menu.**

Evidence:
- MCM Helper does not scan `Data/MCM/Config` for menus. On `kPostLoadGame` it calls `ConfigStore::ReadConfigs()`, which iterates **SkyUI's own config-manager array** and filters by script type (`src/ConfigStore.cpp`):
  - `auto configManager = SkyUI::ConfigManager::GetInstance(); if (!configManager) { logger::warn("Could not find SkyUI Config Manager."); return; }`
  - `auto modConfigs = ... GetVariable(configManager, "_modConfigs")`
  - `if (configScript && ScriptObject::IsType(configScript, "MCM_ConfigBase"))` → only then `GetModName(configScript)` and `ReadConfig(modName, configScript)`, which loads `Data/MCM/Config/<modName>/config.json`.
- Registration therefore comes from a **SkyUI/MCM_ConfigBase quest**, not from the filesystem. `MCM_ConfigBase.psc` itself is `Scriptname MCM_ConfigBase extends SKI_ConfigBase` (https://github.com/Exit-9B/MCM-Helper/blob/main/scripts/private/MCM_ConfigBase.psc), and the wiki states the quest is mandatory.
- `modName` is derived from the quest's owning plugin (`ConfigStore::GetModName` → `FormUtil::GetModName(quest)`), and `ConfigHandler` enforces that the JSON `modName` equals that plugin name. So the plugin filename and the `MCM/Config/<name>` folder must agree.

Does MCM Helper itself provide the menu host? **Partially.** MCM Helper is an SKSE plugin that implements SkyUI's MCM API natively (it overrides `OnPageReset`, `OnOptionSelect`, … in `MCM_ConfigBase`) and reads `config.json`; but the actual MCM UI/host is **SkyUI's `SKI_ConfigManager`**, and MCM Helper bails out if it can't find it. It does not replace SkyUI.

---

## 5. Hard dependencies and load order

- **SKSE** — MCM Helper's plugin declares `v.MinimumRequiredXSEVersion({ 2, 2, 5 });` and `v.UsesAddressLibrary(true); v.HasNoStructUse(true);` (`src/main.cpp`). It falls back to accepting any runtime `>= SKSE::RUNTIME_1_6_317`.
- **SkyUI** (Nexus 12604) — hard requirement; the menu cannot register without `SKI_ConfigManager`. MCM Helper 1.6.0 release notes: "SkyUI: Updated MCM to match version 6", and Nexus comments state v1.6.3 targets **SkyUI v6** (SkyUI VR, based on v5, reportedly fails with 1.6.3).
- **Address Library for SKSE Plugins** — since `UsesAddressLibrary(true)`, the runtime needs the Address Library (or the NG database) matching the game version.
- **MCM Helper 1.6.3** itself must be installed and enabled (it ships `Data/ESP/MCMHelper.esp` or the ESL variant, plus `MCMHelper.bsa` with the Papyrus scripts).
- **PushAside's ESP** must be present and load. No special ordering relative to MCM Helper is documented: the config is matched by plugin filename at post-load-game, not by load order. SkyUI must simply be installed. You can additionally list hard plugin requirements in `config.json` via `pluginRequirements` ("If the user does not have a required plugin installed and enabled, they will be notified which plugins are missing").
- Load-order implication for authors: because matching is by plugin filename stem, renaming `PushAside.esp` (or merging it) breaks the MCM. Also, `modName` must match the folder name and the JSON value.

---

## 6. Skyrim AE 1.7.104 and MCM Helper 1.6.3

- **SKSE 2.3.1 is the build for game 1.7.104.** skse.silverlock.org states: "Current Anniversary Edition build 2.3.1 (game version 1.7.104)".
- **MCM Helper v1.6.3 exists precisely because of the 1.7.99 update.** The latest `main` commit is `a303348` "fix: update for SkyrimSE 1.7.99" (26 Aug 2026, the same day the Nexus 1.6.3 file was published); its diff bumps `CMakeLists.txt` `VERSION 1.6.2 → 1.6.3` and updates the bundled CommonLibSSE submodule.
- **1.7.104 is covered by the same code path as 1.7.99.** `src/PCH/PCH.h` branches the `SkyrimVM::impl` offset on `REL::Module::get().version() < SKSE::RUNTIME_1_7_99`; 1.7.104 ≥ 1.7.99 takes the new path. `SKSEPlugin_Query` only rejects `ver < SKSE::RUNTIME_1_6_317`, so 1.7.104 is accepted. There is no exact-version allowlist.
- **Community evidence is mixed but mostly positive.** On the Nexus MCM Helper comments: one user wrote "not working in 1.7.104, tested it" and then struck it through with "seems to be working fine on 1.7.104" (6 Sep 2026); another reported getting "SkyUI & MCM working without any problems" on 1.7.104 after earlier SkyUI trouble (11 Sep 2026); a 10 Sep 2026 post still asked for "version for SAE 1.7.104 and SKSE 2.3.1 and Address Library 13", so some users are still hitting issues.
- **Gotchas:** (a) SkyUI must itself be updated for 1.7.104 — early reports blamed SkyUI, not MCM Helper; (b) v1.6.3 reportedly broke the **VR** build because it now expects SkyUI v6 while SkyUI VR is v5 (irrelevant for desktop AE but relevant to the FOMOD choice); (c) the whole 1.7.99→1.7.104 transition required recompiling CommonLibSSE-NG plugins, so make sure you use a current Address Library.
- Practical recommendation for PushAside on 1.7.104: require SKSE 2.3.1+, SkyUI v6 (or the current 1.7.104-compatible SkyUI), Address Library, and MCM Helper **1.6.3**; verify in-game on 1.7.104 rather than assuming, because the repo names 1.7.99 and never explicitly says 1.7.104.

---

## Gaps / limitations

- The Nexus "Description" (login-walled) could not be read; requirements quotes here come from the GitHub repo/wiki, the SKSE page, and Nexus comment text.
- The wiki's inline JSON examples are rendered as images/code widgets that the reader did not expose, so the wiki's own example snippets are paraphrased from their surrounding prose rather than quoted verbatim. The config.schema.json and source code were read in full and are quoted directly.
- The `GlobalValues` wiki example was truncated; the claim that a TESGlobal is referenced via `valueOptions` follows the schema's `sourceForm` field, not a verbatim example.

---

## Source URLs

- https://github.com/Exit-9B/MCM-Helper (README, latest commit "fix: update for SkyrimSE 1.7.99")
- https://github.com/Exit-9B/MCM-Helper/wiki (Home: folder structure, quest requirement)
- https://github.com/Exit-9B/MCM-Helper/wiki/Menu-Layout
- https://github.com/Exit-9B/MCM-Helper/wiki/Control-Types
- https://github.com/Exit-9B/MCM-Helper/wiki/Setting-Types,-Storage,-and-Persistence
- https://github.com/Exit-9B/MCM-Helper/wiki/Creating-a-Config-Script
- https://github.com/Exit-9B/MCM-Helper/wiki/Papyrus-Integration
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/docs/config.schema.json
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/docs/keybinds.schema.json
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/src/ConfigStore.cpp
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/src/SettingStore.cpp
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/src/Json/ConfigHandler.cpp
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/src/Json/ConfigHandler.h
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/src/main.cpp
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/src/PCH/PCH.h
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/CMakeLists.txt
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/scripts/private/MCM_ConfigBase.psc
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/data/MCM/Config/SkyUI_SE/config.json
- https://raw.githubusercontent.com/Exit-9B/MCM-Helper/main/data/MCM/Config/SkyUI_SE/settings.ini
- https://github.com/Exit-9B/MCM-Helper/releases (v1.6.0–v1.6.2 notes)
- https://github.com/Exit-9B/MCM-Helper/commit/a30334864ea46ab6ee9e74bca06187630b67c039
- https://www.nexusmods.com/skyrimspecialedition/mods/53000 (v1.6.3, updated 26 Aug 2026) and ?tab=posts (community 1.7.104 reports)
- https://skse.silverlock.org/ (SKSE 2.3.1 / game 1.7.104)
- https://forums.nexusmods.com/topic/13543597-skyrim-se-updated-to-1799/ (1.7.99 timeline)
- https://www.nexusmods.com/skyrimspecialedition/mods/12604 (SkyUI, requirement)
- https://www.nexusmods.com/skyrimspecialedition/mods/30379 (SKSE Nexus page)
