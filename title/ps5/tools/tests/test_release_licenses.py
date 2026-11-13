"""Release notice and nested-source regression tests; no PS5 SDK required."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release_licenses import dependency_part, dirty_sources, source_archives, validate_notices


class ReleaseNoticesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def git(self, path, *args):
        return subprocess.run(["git", "-C", str(path), *args], check=True,
                              capture_output=True, text=True).stdout.strip()

    def repo(self, name):
        path = self.root / name
        path.mkdir()
        self.git(path, "init", "-q")
        self.git(path, "config", "user.name", "Release fixture")
        self.git(path, "config", "user.email", "fixture@example.invalid")
        self.git(path, "remote", "add", "origin", f"https://example.invalid/{name}")
        (path / "LICENSE").write_text(f"License for {name}\n")
        (path / "code.c").write_text("int fixture;\n")
        self.git(path, "add", ".")
        self.git(path, "commit", "-qm", "fixture")
        return path

    def part(self, repo, out, ident="rexglue"):
        return dependency_part(repo, ident, "Fixture component", "BSD-3-Clause", ["Fixture"], out)

    def inventory(self):
        app = self.root / "app"
        parts = []
        for ident in ("rexglue", "webofshadows", "llvm-runtime"):
            folder = app / "licenses" / ident
            folder.mkdir(parents=True)
            (folder / "LICENSE").write_text("notice\n")
            parts.append(dict(id=ident, texts=f"licenses/{ident}/", source=dict(kind="git", revision="a" * 40)))
        return app, parts

    def test_verbatim_root_and_nested_notices_and_source_archives(self):
        leaf = self.repo("leaf")
        child = self.repo("child")
        self.git(child, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(leaf), "deps/leaf")
        self.git(child, "commit", "-qam", "add nested dependency")
        parent = self.repo("parent")
        self.git(parent, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(child), "deps/child")
        self.git(parent, "commit", "-qam", "add dependency")
        self.git(parent, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive")
        out = self.root / "notices"
        p = self.part(parent, out)
        self.assertEqual((out / "rexglue/LICENSE").read_bytes(), (parent / "LICENSE").read_bytes())
        self.assertTrue((out / "rexglue/deps/child/deps/leaf/LICENSE").is_file())
        archives = list(source_archives(parent, p["source"], p["id"]))
        self.assertEqual(len(archives), 3)
        for repo, rev, label, remote in archives:
            subprocess.run(["git", "-C", str(repo), "archive", "--output=" + str(self.root / (label + ".tar")), rev], check=True)
        (parent / "deps/child/deps/leaf/code.c").write_text("local change\n")
        p = self.part(parent, out)
        self.assertTrue(any("deps/leaf" in name for name in dirty_sources(p["source"], "rexglue")))

    def test_notices_are_read_from_commit_not_dirty_worktree(self):
        repo = self.repo("versioned")
        original = (repo / "LICENSE").read_bytes()
        (repo / "LICENSE").write_text("uncommitted replacement\n")
        p = self.part(repo, self.root / "out")
        self.assertTrue(p["source"]["dirty"])
        self.assertEqual((self.root / "out/rexglue/LICENSE").read_bytes(), original)

    def test_spdx_license_directory_texts_are_included(self):
        repo = self.repo("spdx")
        (repo / "LICENSES").mkdir()
        (repo / "LICENSES/Apache-2.0.txt").write_text("full upstream terms\n")
        self.git(repo, "add", "LICENSES")
        self.git(repo, "commit", "-qm", "SPDX license texts")
        self.part(repo, self.root / "out")
        self.assertEqual((self.root / "out/rexglue/LICENSES/Apache-2.0.txt").read_text(), "full upstream terms\n")

    def test_missing_and_empty_license_directories_are_rejected(self):
        app, parts = self.inventory()
        validate_notices(app, parts)
        (app / "licenses/llvm-runtime/LICENSE").unlink()
        with self.assertRaisesRegex(SystemExit, "llvm-runtime"):
            validate_notices(app, parts)

    def test_old_manifest_is_rejected(self):
        app, parts = self.inventory()
        with self.assertRaisesRegex(SystemExit, "rexglue"):
            validate_notices(app, parts[1:])

    def test_license_path_escape_is_rejected(self):
        app, parts = self.inventory()
        parts[0]["texts"] = "../outside"
        with self.assertRaisesRegex(SystemExit, "escapes"):
            validate_notices(app, parts)

    def test_uninitialized_root_cannot_be_mistaken_for_parent(self):
        repo = self.repo("outer")
        child = repo / "uninitialized"
        child.mkdir()
        with self.assertRaisesRegex(SystemExit, "Initialize"):
            self.part(child, self.root / "out")

    def test_stage_then_package_with_llvm_fallback(self):
        tools = Path(__file__).resolve().parents[1]

        def load(name, filename):
            spec = importlib.util.spec_from_file_location(name, tools / filename)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module

        title = self.repo("title")
        sdk = self.repo("PS5_PayloadSDK")
        vulkan = self.repo("PS5_Vulkan")
        mesa = self.repo("PS5_Mesa")
        rex = self.repo("rexglue-sdk")
        wos = self.repo("WebOfShadowsRecomp")
        for p in ("platform/src/regex/COPYRIGHT.musl", "platform/src/dlmalloc/SOURCE"):
            (sdk / p).parent.mkdir(parents=True, exist_ok=True)
            (sdk / p).write_text("upstream notice\n")
        self.git(sdk, "add", ".")
        self.git(sdk, "commit", "-qm", "platform notices")
        for p in ("docs/license.rst", "licenses/MIT"):
            (mesa / p).parent.mkdir(parents=True, exist_ok=True)
            (mesa / p).write_text("Mesa notice\n")
        self.git(mesa, "add", ".")
        self.git(mesa, "commit", "-qm", "Mesa notices")
        for p in ("LICENSE.md", "external/imgui/LICENSE.txt", "external/ktx/LICENSE.md",
                  "external/ktx/NOTICE.md", "external/tinygltf/LICENSE", "ps5/third_party/volk/LICENSE.md",
                  "ps5/licenses/llvm-runtime/LICENSE.TXT"):
            (title / p).parent.mkdir(parents=True, exist_ok=True)
            (title / p).write_text("upstream notice\n")
        param = title / "ps5/sce_sys/param.json"
        param.parent.mkdir(parents=True)
        param.write_text(json.dumps(dict(titleId="PPSA99300", localizedParameters={"en-US": {"titleName": "Fixture"}})))
        pin = title / "ps5/tools/setup-sdk.sh"
        pin.parent.mkdir(parents=True)
        pin.write_text("sdk_revision=" + self.git(sdk, "rev-parse", "HEAD") + "\n")
        (title / ".gitignore").write_text("dist/\nbuild/\n")
        self.git(title, "add", ".")
        self.git(title, "commit", "-qm", "title inputs")
        archive = self.root / "radv/build/archive.a"
        archive.parent.mkdir(parents=True)
        archive.write_bytes(b"fixture")
        (archive.parent.parent / "PROVENANCE.txt").write_text("revision: " + self.git(mesa, "rev-parse", "HEAD") + "\n")
        app = title / "dist/PPSA99300"
        app.mkdir(parents=True)
        (app / "eboot.bin").write_bytes(b"fixture executable")
        stage = load("fixture_stage", "stage-notices.py")
        stage.ROOT, stage.PS5 = title, title / "ps5"
        with patch.dict("os.environ", {"REXGLUE_SRC": str(rex), "WEBOFSHADOWSRECOMP_SRC": str(wos),
                                      "PS5_PAYLOAD_SDK_FORK": str(sdk)}), patch.object(sys, "argv", ["stage", str(app), str(vulkan), str(archive)]):
            stage.main()
        parts = json.loads((app / "licenses/components.json").read_text())
        self.assertEqual({p["id"] for p in parts}, {"title", "rexglue", "webofshadows", "platform", "ps5-vulkan", "radv", "llvm-runtime"})
        self.assertEqual((app / "licenses/llvm-runtime/LICENSE.TXT").read_text(), "upstream notice\n")
        package = load("fixture_package", "package-release.py")
        package.ROOT, package.PS5 = title, title / "ps5"
        with patch.object(sys, "argv", ["package", "v1.0.1"]):
            package.main()
        output = title / "dist/release-v1.0.1"
        self.assertEqual(len(list((output / "source").glob("*.tar.gz"))), 6)
        self.assertTrue((output / "source/SOURCES.txt").is_file())
        self.assertTrue((output / "SHA256SUMS").is_file())
        self.assertEqual(len(list(output.glob("*.zip"))), 1)


if __name__ == "__main__":
    unittest.main()
import importlib.util
import json
