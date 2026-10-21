"""Stage the install QA kit into a test desktop's persist folder (the guest's Z:\\amqa) - see README.md here.

Writes under <persist>\\amqa:
  probe-<ver>\\                 a fake portable unzip per version: the probe as Agentmaster.exe, .portable,
                               .am-version and .am-manifest (what New-UnpackagedTerminalDistribution.ps1
                               -PortableMode ships)
  Agentmaster_<ver>_x64.zip     the newest version packed like a release zip (one agentmaster-<ver> folder), for
                               am-update.ps1 -Portable -ZipUrl file:///...
  repo\\tools\\...               Install-Agentmaster.ps1 and tests\\Test-AgentmasterInstaller.ps1 (the suite's own
  repo\\src\\...                 repo layout) and src\\cascadia\\TerminalApp\\AgentMaster\\am-update.ps1

Usage: python -I stage.py --persist <persist folder> [--probe <Agentmaster.exe>] [--versions 0.8.1.0 0.8.2.0 ...]
Re-running overwrites the staged copies (re-stage after every script or probe change).
"""
import argparse
import os
import shutil
import zipfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCRIPTS = (r"tools\Install-Agentmaster.ps1", r"tools\tests\Test-AgentmasterInstaller.ps1",
           r"src\cascadia\TerminalApp\AgentMaster\am-update.ps1")


def write(path, text):
    """Write an ASCII text file with CRLF line ends, creating its folder.

    Args:
        path: The file to write.
        text: Its content (pure ASCII).
    Raises:
        OSError: When the file cannot be written; UnicodeEncodeError for non-ASCII text.
    """
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\r\n") as f:
        f.write(text)


def stage_probe(kit, probe, version):
    """One fake portable unzip of `version`.

    Args:
        kit: The staging folder (<persist>\\amqa).
        probe: The built probe exe.
        version: The 4-part version stamp it carries.
    Returns:
        The unzip folder.
    Raises:
        OSError: When a file cannot be copied or written.
    """
    d = os.path.join(kit, "probe-" + version)
    os.makedirs(d, exist_ok=True)
    shutil.copyfile(probe, os.path.join(d, "Agentmaster.exe"))
    write(os.path.join(d, ".portable"), "")
    write(os.path.join(d, ".am-version"), version + "\n")
    names = [".am-manifest", ".am-version", ".portable", "Agentmaster.exe"]
    write(os.path.join(d, ".am-manifest"), "# Agentmaster portable distribution manifest - the top-level names this build ships.\n" + "\n".join(names) + "\n")
    return d


def pack_release_zip(kit, unzip, version):
    """Pack an unzip like a release zip: every top-level entry inside one agentmaster-<version> folder.

    Args:
        kit: The staging folder.
        unzip: The folder to pack.
        version: The version for the zip's and its folder's names.
    Returns:
        The zip path.
    Raises:
        OSError: When the zip cannot be written.
    """
    dst = os.path.join(kit, "Agentmaster_%s_x64.zip" % version)
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(unzip)):
            z.write(os.path.join(unzip, name), "agentmaster-%s/%s" % (version, name))
    return dst


def main():
    """Stage everything and list it.

    Raises:
        SystemExit: When the persist folder or the probe does not exist.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--persist", required=True, help="the test desktop's persist folder on this PC (its guest sees it as Z:)")
    ap.add_argument("--probe", default=r"Q:\tmp\amprobe\Agentmaster.exe", help="the probe built by build.cmd")
    ap.add_argument("--versions", nargs="+", default=["0.8.1.0", "0.8.2.0", "0.8.3.0"], help="the fake versions to stage")
    args = ap.parse_args()
    if not os.path.isdir(args.persist):
        raise SystemExit("no persist folder: " + args.persist)
    if not os.path.isfile(args.probe):
        raise SystemExit("no probe exe (run build.cmd): " + args.probe)
    kit = os.path.join(args.persist, "amqa")
    last = None
    for v in args.versions:
        last = (stage_probe(kit, args.probe, v), v)
    if last:
        pack_release_zip(kit, last[0], last[1])
    for rel in SCRIPTS:
        dst = os.path.join(kit, "repo", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(REPO, rel), dst)
    for root, _dirs, files in os.walk(kit):
        for f in files:
            p = os.path.join(root, f)
            print(os.path.relpath(p, args.persist), os.path.getsize(p))


if __name__ == "__main__":
    main()
