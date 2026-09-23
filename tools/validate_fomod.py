#!/usr/bin/env python3
"""Validate PushAside's FOMOD before it becomes a release asset.

This is the check that must be able to FAIL. It is deliberately self-contained
(unlike HeapSentinel's original, which was generated from a single source of
truth by tools/gen-fomod-profiles.py): PushAside keeps the profile inis as
hand-maintained files, so the drift gate here is between the files themselves
and src/Config.cpp, not between generated output and a generator.

It does these independent checks, each a real gate:

  1. KEY NAMES: every key in every profile must be read by src/Config.cpp, and
     every key src/Config.cpp reads must appear in every profile and in the
     standalone config. A renamed key, or a profile that forgets a whole
     section such as [Push], fails here.

  2. XML: the FOMOD is well-formed, validates against the vendored FOMOD schema
     (fomod/schema/ModConfig5.0.xsd) when xmllint is available, and satisfies
     the structural rules the schema is vague about (one Recommended default in
     the SelectExactlyOne profile group; every profile flag has exactly one
     conditional pattern and vice versa).

  3. PACKAGE LAYOUT: every <file source="..."> in ModuleConfig.xml resolves to a
     real file in the package, and the package carries the FOMOD, the DLL and a
     profile ini per choice.

  4. CONDITIONAL LOGIC: an independent simulation of the FOMOD flag rules
     asserts that selecting each profile installs that profile's ini to
     SKSE/Plugins/PushAside.ini and no other profile's ini.

  5. ARCHIVE: optionally builds the installable zip and re-checks its contents.

Usage:
    python3 tools/validate_fomod.py --dll path/to/PushAside.dll
    python3 tools/validate_fomod.py --dll ... --zip PushAside-0.1.0-fomod.zip
    python3 tools/validate_fomod.py                  # skips the DLL/zip checks

Exit status is 0 only if every applicable check passed.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCHEMA = REPO_ROOT / "fomod" / "schema" / "ModConfig5.0.xsd"
MODULECONFIG = REPO_ROOT / "fomod" / "ModuleConfig.xml"
INFOXML = REPO_ROOT / "fomod" / "info.xml"
PROFILES_DIR = REPO_ROOT / "fomod" / "profiles"
STANDALONE_INI = REPO_ROOT / "config" / "PushAside.ini"
REQUIRED_DLL = "SKSE/Plugins/PushAside.dll"

EXPLANATION_GROUP = "Install PushAside"
PROFILE_GROUP = "Push strength profile"
EXPLANATION_NEEDLES = ("physics-based", "not a ragdoll", "nothing leaves the machine")


class Checker:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.skips: list[str] = []

    def fail(self, message: str) -> None:
        self.failures.append(message)
        print(f"  FAIL: {message}")

    def ok(self, message: str) -> None:
        print(f"  ok:   {message}")

    def skip(self, message: str) -> None:
        self.skips.append(message)
        print(f"  SKIP: {message}")


def strip_ns(tag: str) -> str:
    return tag.split("}", 1)[1] if "}" in tag else tag


def read_version() -> str:
    m = re.search(r'set_version\("([^"]+)"\)', (REPO_ROOT / "xmake.lua").read_text(encoding="utf-8"))
    return m.group(1) if m else ""


# ---------------------------------------------------------------------------
# 1. every profile key exists in the parser, and vice versa
# ---------------------------------------------------------------------------

def parser_keys() -> set[tuple[str, str]]:
    source = (REPO_ROOT / "src" / "Config.cpp").read_text(encoding="utf-8")
    pairs = re.findall(r'Read(?:Bool|UInt)\("([^"]+)",\s*"([^"]+)"', source)
    return {(section, key) for section, key in pairs}


def ini_keys(text: str) -> dict[tuple[str, str], str]:
    """{(section, key): value} for a rendered ini. Ignores comments/blanks."""
    found: dict[tuple[str, str], str] = {}
    section = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if "=" in line:
            key, _, value = line.partition("=")
            found[(section, key.strip())] = value.strip()
    return found


def check_keys(c: Checker) -> None:
    print("[1] ini keys agree with src/Config.cpp")
    known = parser_keys()
    if not known:
        c.fail("no keys parsed from src/Config.cpp - the parser regex broke")
        return

    targets = {"config/PushAside.ini": STANDALONE_INI.read_text(encoding="utf-8")}
    for path in sorted(PROFILES_DIR.glob("*.ini")):
        targets[f"fomod/profiles/{path.name}"] = path.read_text(encoding="utf-8")

    if len(targets) < 2:
        c.fail("no profiles found in fomod/profiles/")
        return

    for rel, text in targets.items():
        present = ini_keys(text)
        unknown = sorted(set(present) - known)
        missing = sorted(known - set(present))
        for section, key in unknown:
            c.fail(f"{rel}: key [{section}] {key} is not read by src/Config.cpp")
        for section, key in missing:
            c.fail(f"{rel}: src/Config.cpp reads [{section}] {key} but the profile omits it")
        if not unknown and not missing:
            c.ok(f"{rel}: {len(present)} keys, all known and complete")


# ---------------------------------------------------------------------------
# 2. XML well-formedness, schema, structure
# ---------------------------------------------------------------------------

def parse_profile_group(root: ET.Element) -> list[ET.Element]:
    for group in root.iter():
        if strip_ns(group.tag) == "group" and group.get("name") == PROFILE_GROUP:
            plugins = [p for p in group if strip_ns(p.tag) == "plugins"]
            if plugins:
                return [p for p in plugins[0] if strip_ns(p.tag) == "plugin"]
    return []


def plugin_flag_value(plugin: ET.Element) -> str:
    for child in plugin:
        if strip_ns(child.tag) == "conditionFlags":
            for flag in child:
                if strip_ns(flag.tag) == "flag" and flag.get("name") == "profile":
                    return (flag.text or "").strip()
    return ""


def conditional_patterns(root: ET.Element) -> list[tuple[str, str, str]]:
    """[(flag_value, source, destination)] for every profile pattern."""
    out: list[tuple[str, str, str]] = []
    for cfi in root:
        if strip_ns(cfi.tag) != "conditionalFileInstalls":
            continue
        for pats in cfi:
            for pat in pats:
                if strip_ns(pat.tag) != "pattern":
                    continue
                value = ""
                for dep in pat.iter():
                    if strip_ns(dep.tag) == "flagDependency" and dep.get("flag") == "profile":
                        value = dep.get("value", "")
                for files_el in pat:
                    if strip_ns(files_el.tag) != "files":
                        continue
                    for file_el in files_el:
                        if strip_ns(file_el.tag) == "file":
                            out.append((value, file_el.get("source", ""),
                                        file_el.get("destination", "")))
    return out


def check_xml(c: Checker) -> list[ET.Element]:
    print("[2] ModuleConfig.xml / info.xml are valid FOMOD")
    try:
        root = ET.parse(MODULECONFIG).getroot()
    except ET.ParseError as exc:
        c.fail(f"ModuleConfig.xml is not well-formed: {exc}")
        return []
    c.ok("ModuleConfig.xml is well-formed")

    if strip_ns(root.tag) != "config":
        c.fail(f"ModuleConfig.xml root is <{strip_ns(root.tag)}>, expected <config>")
        return []

    # FOMOD schema validation (the vendored schema is the upstream spec with a
    # one-character typo fixed: type=" xs:string" -> type="xs:string").
    if shutil.which("xmllint"):
        proc = subprocess.run(
            ["xmllint", "--noout", "--schema", str(SCHEMA), str(MODULECONFIG)],
            capture_output=True,
            text=True,
        )
        if proc.returncode == 0:
            c.ok("ModuleConfig.xml validates against fomod/schema/ModConfig5.0.xsd")
        else:
            for line in (proc.stderr or proc.stdout).splitlines():
                if "validity error" in line or "fails to validate" in line:
                    c.fail(f"schema: {line.strip()}")
    else:
        c.skip("xmllint not found - schema validation skipped (structural checks still run)")

    steps = [e for e in root if strip_ns(e.tag) == "installSteps"]
    step_list = list(steps[0]) if steps else []
    if len(step_list) != 2:
        c.fail(f"expected 2 install steps, found {len(step_list)}")
    else:
        c.ok("ModuleConfig.xml has the explanation step and the profile step")

    explanation_plugins: list[ET.Element] = []
    for step in step_list:
        for groups in step:
            if strip_ns(groups.tag) != "optionalFileGroups":
                continue
            for group in groups:
                if strip_ns(group.tag) != "group" or group.get("name") != EXPLANATION_GROUP:
                    continue
                for plugins in group:
                    if strip_ns(plugins.tag) != "plugins":
                        continue
                    for plugin in plugins:
                        if strip_ns(plugin.tag) == "plugin":
                            explanation_plugins.append(plugin)

    if len(explanation_plugins) != 1:
        c.fail(f"expected exactly 1 explanation plugin, found {len(explanation_plugins)}")
    else:
        text = "".join(explanation_plugins[0].itertext()).lower()
        for needle in EXPLANATION_NEEDLES:
            if needle not in text:
                c.fail(f"explanation step is missing the honest claim {needle!r}")
        c.ok("explanation plugin is present and states what the plugin is/is not")

    plugins = parse_profile_group(root)
    if not plugins:
        c.fail(f"profile group {PROFILE_GROUP!r} not found")
        return plugins

    # SelectExactlyOne.
    for group in root.iter():
        if strip_ns(group.tag) == "group" and group.get("name") == PROFILE_GROUP:
            if group.get("type") != "SelectExactlyOne":
                c.fail(f"{PROFILE_GROUP} must be SelectExactlyOne")
            break

    recommended = 0
    for plugin in plugins:
        td = next((ch for ch in plugin if strip_ns(ch.tag) == "typeDescriptor"), None)
        if td is not None and any(
            strip_ns(t.tag) == "type" and t.get("name") == "Recommended" for t in td
        ):
            recommended += 1
    if recommended != 1:
        c.fail(f"profile group has {recommended} Recommended plugins, expected exactly 1")
    else:
        c.ok("exactly one profile is the Recommended default")

    # Every profile flag has exactly one conditional pattern, and every pattern
    # names a real profile.
    patterns = conditional_patterns(root)
    flag_values = [v for v, _, _ in patterns]
    profile_flags = [plugin_flag_value(p) for p in plugins]
    if sorted(flag_values) != sorted(profile_flags):
        c.fail(f"conditional patterns cover {sorted(flag_values)}, expected {sorted(profile_flags)}")
    elif len(set(flag_values)) != len(flag_values):
        c.fail("a profile flag is covered by more than one conditional pattern")
    else:
        c.ok("each profile has exactly one conditional-install pattern")

    try:
        info = ET.parse(INFOXML).getroot()
    except (ET.ParseError, FileNotFoundError) as exc:
        c.fail(f"info.xml problem: {exc}")
        return plugins
    info_text = {strip_ns(e.tag): (e.text or "").strip() for e in info}
    for field in ("Name", "Author", "Version", "Description"):
        if not info_text.get(field):
            c.fail(f"info.xml is missing <{field}>")
    version = read_version()
    if info_text.get("Version") != version:
        c.fail(f"info.xml Version is {info_text.get('Version')!r}, xmake.lua says {version!r}")
    else:
        c.ok(f"info.xml is well-formed and its version matches xmake.lua ({version})")
    return plugins


# ---------------------------------------------------------------------------
# 3. package layout
# ---------------------------------------------------------------------------

def check_package(c: Checker, dll: Path | None, stage: Path) -> None:
    print("[3] package layout")
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    for rel in ["fomod/ModuleConfig.xml", "fomod/info.xml"] + [
        f"fomod/profiles/{p.name}" for p in sorted(PROFILES_DIR.glob("*.ini"))
    ]:
        dest = stage / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO_ROOT / rel, dest)
    shutil.copyfile(REPO_ROOT / "README.md", stage / "README.md")

    if dll is not None:
        dest = stage / REQUIRED_DLL
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(dll, dest)

    root = ET.parse(MODULECONFIG).getroot()
    sources: list[str] = []
    for file_el in root.iter():
        if strip_ns(file_el.tag) == "file":
            source = file_el.get("source")
            destination = file_el.get("destination")
            if not source or not destination:
                c.fail(f"a <file> has an empty source or destination: {file_el.attrib}")
                continue
            sources.append(source)
    for source in sources:
        if not (stage / source).is_file():
            if source == REQUIRED_DLL and dll is None:
                c.skip(f"{source} not staged (no --dll given)")
            else:
                c.fail(f"<file source={source!r}> does not exist in the package")
    if dll is not None:
        if not (stage / REQUIRED_DLL).is_file():
            c.fail("the DLL was not staged")
        else:
            c.ok(f"the DLL is staged at {REQUIRED_DLL}")

    declared_sources = set(sources)
    for path in sorted(PROFILES_DIR.glob("*.ini")):
        rel = f"fomod/profiles/{path.name}"
        if rel not in declared_sources:
            c.fail(f"profile {path.name} is not referenced by ModuleConfig.xml")
        if not (stage / rel).is_file():
            c.fail(f"profile file {rel} is missing from the package")
    count = len(list(PROFILES_DIR.glob("*.ini")))
    c.ok(f"{count} profile variants are present and declared")


# ---------------------------------------------------------------------------
# 4. independent conditional-flag simulation
# ---------------------------------------------------------------------------

def simulate_selection(root: ET.Element, selections: dict[str, dict[str, list[str]]]):
    """A deliberately small, independent FOMOD resolver.

    Returns (required, conditional). It is NOT the manager's code: agreement
    between this and the manager is the point.
    """
    def flag_dep_met(dep: ET.Element, flags: dict[str, str]) -> bool:
        tag = strip_ns(dep.tag)
        if tag == "flagDependency":
            return flags.get(dep.get("flag", "")) == dep.get("value", "")
        if tag == "dependencies":
            subs = [flag_dep_met(child, flags) for child in dep]
            operator = dep.get("operator", "And")
            return all(subs) if operator == "And" else any(subs)
        return True

    required: list[str] = []
    for files_el in root:
        if strip_ns(files_el.tag) == "requiredInstallFiles":
            for file_el in files_el:
                if strip_ns(file_el.tag) == "file":
                    required.append(file_el.get("source", ""))

    flags: dict[str, str] = {}
    for step in root.iter():
        if strip_ns(step.tag) != "installStep":
            continue
        by_group = selections.get(step.get("name", ""), {})
        for groups in step:
            if strip_ns(groups.tag) != "optionalFileGroups":
                continue
            for group in groups:
                if strip_ns(group.tag) != "group":
                    continue
                chosen = set(by_group.get(group.get("name", ""), []))
                for plugins in group:
                    if strip_ns(plugins.tag) != "plugins":
                        continue
                    for plugin in plugins:
                        if strip_ns(plugin.tag) != "plugin":
                            continue
                        if group.get("type") == "SelectAll" or plugin.get("name") in chosen:
                            for child in plugin:
                                if strip_ns(child.tag) == "conditionFlags":
                                    for flag in child:
                                        flags[flag.get("name", "")] = (flag.text or "").strip()

    conditional: list[tuple[str, str]] = []
    for cfi in root:
        if strip_ns(cfi.tag) != "conditionalFileInstalls":
            continue
        for pats in cfi:
            for pat in pats:
                if strip_ns(pat.tag) != "pattern":
                    continue
                deps = [ch for ch in pat if strip_ns(ch.tag) == "dependencies"]
                if deps and flag_dep_met(deps[0], flags):
                    for files_el in pat:
                        if strip_ns(files_el.tag) == "files":
                            for file_el in files_el:
                                if strip_ns(file_el.tag) == "file":
                                    conditional.append(
                                        (file_el.get("source", ""), file_el.get("destination", ""))
                                    )
    return required, conditional


def check_conditional(c: Checker, plugins: list[ET.Element]) -> None:
    print("[4] conditional-install flag simulation")
    if not plugins:
        c.skip("no profile plugins parsed")
        return
    root = ET.parse(MODULECONFIG).getroot()
    base_selection = {"PushAside": {EXPLANATION_GROUP: ["Install PushAside"]}}
    seen_sources: set[str] = set()
    for plugin in plugins:
        title = plugin.get("name", "")
        value = plugin_flag_value(plugin)
        selection = dict(base_selection)
        selection["Configuration profile"] = {PROFILE_GROUP: [title]}
        required, conditional = simulate_selection(root, selection)
        expected_src = f"fomod/profiles/{value}.ini"
        if conditional != [(expected_src, "SKSE/Plugins/PushAside.ini")]:
            c.fail(f"profile {value}: conditional installs are {conditional!r}")
            continue
        seen_sources.add(expected_src)
        c.ok(f"{title} -> {expected_src}")
    if len(seen_sources) == len(plugins):
        c.ok("every profile selects a distinct ini file")
    else:
        c.fail(f"only {len(seen_sources)} distinct profile inis are reachable")


# ---------------------------------------------------------------------------
# 5. archive
# ---------------------------------------------------------------------------

def build_zip(c: Checker, stage: Path, zip_path: Path) -> None:
    print("[5] archive")
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(stage).as_posix())
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())
    failures_before = len(c.failures)
    required = {"fomod/ModuleConfig.xml", "fomod/info.xml", REQUIRED_DLL}
    for name in sorted(required):
        if name not in names:
            c.fail(f"{name} missing from {zip_path.name}")
    if not any(n.startswith("fomod/profiles/") for n in names):
        c.fail("no profile inis in the archive")
    if len(c.failures) == failures_before:
        c.ok(f"{zip_path.name}: {len(names)} entries, FOMOD + DLL + profiles present")


# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Validate PushAside's FOMOD.")
    parser.add_argument("--dll", type=Path, default=None, help="built PushAside.dll to stage")
    parser.add_argument("--zip", type=Path, default=None, help="write the FOMOD archive here")
    parser.add_argument("--stage", type=Path, default=None, help="staging directory (default: temp)")
    args = parser.parse_args(argv)

    c = Checker()
    check_keys(c)
    plugins = check_xml(c)

    stage = args.stage or Path(tempfile.mkdtemp(prefix="pushaside-fomod-stage-"))
    check_package(c, args.dll, stage)
    check_conditional(c, plugins)
    if args.zip is not None:
        build_zip(c, stage, args.zip)
    elif args.stage is None:
        shutil.rmtree(stage, ignore_errors=True)

    print()
    if c.skips:
        print(f"{len(c.skips)} check(s) skipped:")
        for item in c.skips:
            print(f"  - {item}")
    if c.failures:
        print(f"RESULT: FAILED ({len(c.failures)} failure(s))")
        return 1
    print("RESULT: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
