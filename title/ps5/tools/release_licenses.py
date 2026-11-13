"""License and source inventory helpers for the PS5 release tools.

Copyright (C) 2026 KongaTime
SPDX-License-Identifier: MIT
"""
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


def published_patch(repo):
    """The patch the parent repository publishes for this submodule, when the
    checkout's changes are exactly that patch (EdgeOfTimeRecomp's build guide
    applies ps5/patches/plume-ps5.patch to thirdparty/plume): the source is then
    the recorded revision with that patch, both published. None otherwise."""
    parent = git(repo, "rev-parse", "--show-superproject-working-tree")
    if not parent:
        return None
    patch = Path(parent) / "ps5" / "patches" / f"{Path(repo).name}-ps5.patch"
    if not patch.is_file() or git(parent, "status", "--porcelain", "--", str(patch.relative_to(parent))):
        return None
    changed = git(repo, "diff", "--name-only", "HEAD").split("\n")
    if not changed or changed == [""] or git(repo, "ls-files", "--others", "--exclude-standard"):
        return None
    with tempfile.TemporaryDirectory() as tmp:
        for path in changed:
            (Path(tmp) / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(Path(repo) / path, Path(tmp) / path)
        if subprocess.run(["git", "apply", "-R", str(patch)], cwd=tmp, capture_output=True).returncode:
            return None
        for path in changed:
            head = subprocess.run(["git", "-C", str(repo), "show", f"HEAD:{path}"], capture_output=True).stdout
            if (Path(tmp) / path).read_bytes() != head:
                return None
    return str(patch.relative_to(parent))


def source_info(repo, revision="HEAD"):
    repo = Path(repo).resolve()
    # An uninitialized submodule otherwise resolves to the parent repository.
    if Path(git(repo, "rev-parse", "--show-toplevel")).resolve() != repo:
        raise SystemExit(f"Initialize the dependency at {repo} before staging notices")
    rev = git(repo, "rev-parse", f"{revision}^{{commit}}")
    remote = git(repo, "remote", "get-url", "origin").removesuffix(".git")
    info = dict(kind="git", path=str(repo), remote=remote, revision=rev,
                dirty=bool(git(repo, "status", "--porcelain", "--untracked-files=normal")))
    if info["dirty"] and (patch := published_patch(repo)):
        info["dirty"] = False
        info["patch"] = patch
    return info


def submodule_sources(repo, rev):
    """Capture checked-out submodules, including nested ones and local commits."""
    result = {}
    for record in git(repo, "ls-tree", "-rz", rev).split("\0"):
        if not record:
            continue
        header, path = record.split("\t", 1)
        if header.split()[0] != "160000":
            continue
        child = Path(repo) / path
        if not (child / ".git").exists():
            # Some desktop/tool dependencies are not used or initialized on PS5.
            continue
        info = source_info(child)
        info["submodules"] = submodule_sources(child, info["revision"])
        result[path] = info
    return result


def copy_notices(repo, rev, dest):
    """Copy tracked license documents without copying generated game files."""
    copied = []
    for record in git(repo, "ls-tree", "-rz", rev).split("\0"):
        if not record:
            continue
        header, path = record.split("\t", 1)
        mode, kind, blob = header.split()
        if kind != "blob" or mode == "120000":
            continue
        named_notice = re.match(r"^(licen[cs]e|copying|copyright|notice)([._-]|$)",
                                Path(path).name, re.I)
        in_notice_directory = any(p.lower() in ("licenses", "licences", "license", "licence", "notices")
                                  for p in Path(path).parts[:-1])
        if not named_notice and not in_notice_directory:
            continue
        target = Path(dest) / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(subprocess.run(["git", "-C", str(repo), "cat-file", "blob", blob],
                                         check=True, capture_output=True).stdout)
        copied.append(path)
    return copied


def dependency_part(repo, ident, name, licence, copyright, out):
    info = source_info(repo)
    info["submodules"] = submodule_sources(repo, info["revision"])
    dest = Path(out) / ident
    docs = copy_notices(repo, info["revision"], dest)
    if not any("/" not in p and Path(p).name.lower().startswith(("license", "licence", "copying"))
               for p in docs):
        raise SystemExit(f"Missing top-level license for {name} at {repo}")

    def collect(parent, modules, target):
        for path, child in modules.items():
            child_repo = Path(parent) / path
            copy_notices(child_repo, child["revision"], Path(target) / path)
            collect(child_repo, child.get("submodules", {}), Path(target) / path)

    collect(repo, info["submodules"], dest)
    return dict(id=ident, name=name, licence=licence, copyright=copyright,
                artifacts=["eboot.bin"], texts=f"licenses/{ident}/", source=info)


def validate_notices(folder, parts):
    """Reject incomplete inventories instead of publishing broken references."""
    folder = Path(folder).resolve()
    ids = [p["id"] for p in parts]
    if len(ids) != len(set(ids)):
        raise SystemExit("Duplicate component IDs in licenses/components.json")
    for required in ("rexglue", "webofshadows", "llvm-runtime"):
        if required not in ids:
            raise SystemExit(f"Missing {required} component; rebuild the title with updated notice tools")
    for part in parts:
        target = (folder / part["texts"]).resolve()
        if not target.is_relative_to(folder):
            raise SystemExit(f"License path escapes title folder: {part['texts']}")
        if not target.is_dir() or not any(p.is_file() and p.stat().st_size for p in target.rglob("*")):
            raise SystemExit(f"Missing license text for {part['id']}: {part['texts']}")
        if part["source"]["kind"] == "git" and not re.fullmatch(r"[0-9a-f]{40}", part["source"]["revision"]):
            raise SystemExit(f"Missing exact source revision for {part['id']}")


def dirty_sources(source, label):
    found = [label] if source.get("dirty") else []
    for path, child in source.get("submodules", {}).items():
        found.extend(dirty_sources(child, f"{label}/{path}"))
    return found


def source_archives(repo, source, label):
    """Yield every source archive, preserving nested dependency identities."""
    where = source["remote"]
    if source.get("patch"):
        where += f" with the parent's {source['patch']} applied"
    yield Path(repo), source["revision"], label, where
    for path, child in source.get("submodules", {}).items():
        child_label = label + "-" + path.replace("/", "-")
        yield from source_archives(Path(repo) / path, child, child_label)
