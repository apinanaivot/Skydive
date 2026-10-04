"""Skydive asset installer. The mod ships no Just Cause 2 content: this builds Rico's animations,
the canopy, hook and wire models and the grapple / parachute sounds from the player's own JC2
install and writes them into the mod folder.

A small window (tkinter): the Just Cause 2 folder, the Skyrim Special Edition folder and where to
write, each found automatically where possible (Steam, GOG, the Bethesda launcher's registry keys)
and changeable with Browse. The default output is the mod folder the installer sits in (the one
holding SKSE/Plugins/Skydive.dll: Skyrim's Data folder, or the mod's folder under a mod manager),
else Skyrim's Data folder.

Skydive-Installer.exe [--jc2 <folder>] [--skyrim <folder>] [--out <folder>]   (prefill the window)
python install.py --cli [--jc2 ...] [--skyrim ...] [--out ...]               (no window)"""
import argparse, os, queue, re, shutil, sys, tempfile, threading, traceback

HERE = os.path.dirname(os.path.abspath(__file__))
for sub in (("tools", "anim"), ("tools", "re")):
    sys.path.insert(0, os.path.join(HERE, "..", *sub))

# JC2 files the build scripts read (skse/README.md, "JC2 assets"): Rico's clips and skeleton,
# the canopy rig and clips, the canopy / hook models and their textures.
SARC_PATTERNS = [
    r"^(?!km\d)(.*(chute|parach|freefall|birdsuit|grpl|reel|fall|roof).*\.ban|.*\.bsk|gae0[1-9].*|gea0[1-9].*|wea04.*|hook_.*dds)$",
    r"^(grpl_|reel_in_start|la_grpl|rico_(speed|turn)_tb)|chute_add_idle",
    r"^heli_default_(timeblend|exit)",
]
# Skyrim's own skeleton (the retarget target), from its animation archive.
SKYRIM_BSA = "Skyrim - Animations.bsa"
SKYRIM_FILES = r"actors.character.(character assets.skeleton.hkx|animations.mt_jumpfall)"
JC2_MARKER = os.path.join("archives_win32", "pc0.tab")

# The tools take their temporary folder from the environment when first imported, so there is
# one per run of the program (emptied before each build, deleted at the end).
WORK = tempfile.mkdtemp(prefix="jc2mech_")
os.environ["JC2MECH_EXTRACTED"] = WORK


# --- Finding the games ------------------------------------------------------------------------

def _reg_values(root, path, names):
    """Values of a registry key (missing ones as None)."""
    import winreg
    out = []
    try:
        with winreg.OpenKey(root, path) as k:
            for n in names:
                try:
                    out.append(winreg.QueryValueEx(k, n)[0])
                except OSError:
                    out.append(None)
    except OSError:
        return [None] * len(names)
    return out


def _reg_subkeys(root, path):
    import winreg
    try:
        with winreg.OpenKey(root, path) as k:
            i = 0
            while True:
                try:
                    yield path + "\\" + winreg.EnumKey(k, i)
                except OSError:
                    return
                i += 1
    except OSError:
        return


def steam_libraries():
    import winreg
    roots = [_reg_values(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", ["SteamPath"])[0],
             r"C:\Program Files (x86)\Steam"]
    libs = []
    for root in filter(None, roots):
        libs.append(root)
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            libs += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="ignore").read())]
    return list(dict.fromkeys(os.path.normpath(p) for p in libs))


def candidates(steam_name, steam_app, gog_name, extra_keys=()):
    """Install folders a game may be in: Steam libraries, Steam's and GOG's registry entries,
    other launchers' keys, common folders."""
    import winreg
    found = [os.path.join(lib, "steamapps", "common", steam_name) for lib in steam_libraries()]
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        for base in (r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
                     r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"):
            found.append(_reg_values(hive, base + "\\Steam App %d" % steam_app, ["InstallLocation"])[0])
            for key in _reg_subkeys(hive, base):
                name, loc = _reg_values(hive, key, ["DisplayName", "InstallLocation"])
                if name and gog_name.lower() in name.lower():
                    found.append(loc)
        for base in (r"SOFTWARE\WOW6432Node\GOG.com\Games", r"SOFTWARE\GOG.com\Games"):
            for key in _reg_subkeys(hive, base):
                name, path = _reg_values(hive, key, ["gameName", "path"])
                if name and gog_name.lower() in name.lower():
                    found.append(path)
    for hive, key, value in extra_keys:
        found.append(_reg_values(hive, key, [value])[0])
    for drive in "CDEFG":
        for parent in (r"Program Files (x86)", r"Program Files", "Games", r"GOG Games"):
            found.append(os.path.join(drive + ":\\", parent, steam_name))
    return [os.path.normpath(p) for p in dict.fromkeys(filter(None, found))]


def jc2_folder(path):
    """The Just Cause 2 folder for a picked path (the game folder or its archives_win32), or None."""
    if not path:
        return None
    path = os.path.normpath(path)
    for d in (path, os.path.dirname(path)):
        if os.path.isfile(os.path.join(d, JC2_MARKER)):
            return os.path.realpath(d)  # the real casing (Steam's registry is lowercase)
    return None


def skyrim_folder(path):
    """The Skyrim folder for a picked path (the game folder or its Data), or None."""
    if not path:
        return None
    path = os.path.normpath(path)
    for d in (path, os.path.dirname(path)):
        # SkyrimSE.exe: Oldrim and Skyrim VR have the same archive with another skeleton.
        if os.path.isfile(os.path.join(d, "SkyrimSE.exe")) and os.path.isfile(os.path.join(d, "Data", SKYRIM_BSA)):
            return os.path.realpath(d)  # the real casing (Steam's registry is lowercase)
    return None


def find_jc2():
    return next((d for d in map(jc2_folder, candidates("Just Cause 2", 8190, "Just Cause 2")) if d), None)


def find_skyrim():
    import winreg
    bethesda = [(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Bethesda Softworks\Skyrim Special Edition", "installed path")]
    return next((d for d in map(skyrim_folder, candidates("Skyrim Special Edition", 489830, "Skyrim", bethesda)) if d), None)


def mod_folder():
    """The mod folder this installer was unpacked into: the one with SKSE/Plugins/Skydive.dll,
    its own folder or the one above (the release puts it in "Skydive Installer/")."""
    here = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
    for d in (here, os.path.dirname(here)):
        if os.path.isfile(os.path.join(d, "SKSE", "Plugins", "Skydive.dll")):
            return d
    return None


# --- The build --------------------------------------------------------------------------------

def build(jc2, skyrim, out):
    """Extracts what the tools need into WORK and builds the files into out. Prints progress."""
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    import jc2arc
    from bsa import Bsa
    import build_anims, build_models, build_sounds
    jc2arc.ARC = os.path.join(jc2, "archives_win32")

    print("Just Cause 2:", jc2)
    print("Skyrim:      ", skyrim)
    print("Writing to:  ", out)
    print("\nReading the Just Cause 2 archives (a minute or two)...", flush=True)
    for pattern in SARC_PATTERNS:
        jc2arc.unsarc(pattern)
    bsa = Bsa(os.path.join(skyrim, "Data", SKYRIM_BSA))
    pat = re.compile(SKYRIM_FILES, re.I)
    for name in (n for n in bsa.files if pat.search(n)):
        p = os.path.join(WORK, "skyrim", name.replace("\\", os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(bsa.read(name))

    argv = sys.argv
    try:
        for title, tool in (("Animations", build_anims), ("Models", build_models), ("Sounds", build_sounds)):
            print("\n" + title, flush=True)
            sys.argv = [tool.__name__, "--data", out]
            tool.main()
    finally:
        sys.argv = argv
        shutil.rmtree(WORK, ignore_errors=True)


# --- The window -------------------------------------------------------------------------------

class QueueWriter:
    """stdout / stderr for the window: lines go to a queue the window reads."""

    def __init__(self, q):
        self.q = q

    def write(self, s):
        if s:
            self.q.put(s)
        return len(s)

    def flush(self):
        pass


def run_gui(args):
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    log_queue = queue.Queue()
    sys.stdout = sys.stderr = QueueWriter(log_queue)  # a windowed exe has no console

    root = tk.Tk()
    root.title("Skydive asset installer")
    root.minsize(640, 480)
    frame = ttk.Frame(root, padding=12)
    frame.pack(fill="both", expand=True)
    frame.columnconfigure(1, weight=1)
    ttk.Label(frame, text="Builds the mod's animations, parachute, grappling hook and sounds from "
                          "your own copy of Just Cause 2.", wraplength=600).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 10))

    jc2 = tk.StringVar(value=args.jc2 or find_jc2() or "")
    skyrim = tk.StringVar(value=args.skyrim or find_skyrim() or "")
    out = tk.StringVar(value=args.out or mod_folder() or "")
    out_auto = not args.out and not mod_folder()  # follows the Skyrim folder until picked by hand

    rows = [
        ("Just Cause 2 folder", jc2, lambda v: jc2_folder(v),
         "Not found: pick the Just Cause 2 folder (the one with archives_win32 in it)."),
        ("Skyrim Special Edition folder", skyrim, lambda v: skyrim_folder(v),
         "Not found: pick the Skyrim Special Edition folder (the one with SkyrimSE.exe in it)."),
        ("Install the files into", out, lambda v: v if v and os.path.isdir(v) else None,
         "Pick the mod's folder (with Mod Organizer 2 / Vortex) or Skyrim's Data folder."),
    ]
    statuses = []
    for i, (label, var, check, missing) in enumerate(rows):
        r = 1 + i * 2
        ttk.Label(frame, text=label).grid(row=r, column=0, sticky="w", padx=(0, 8))
        ttk.Entry(frame, textvariable=var).grid(row=r, column=1, sticky="ew")

        def browse(var=var, label=label, idx=i):
            nonlocal out_auto
            picked = filedialog.askdirectory(title=label, initialdir=var.get() or None)
            if picked:
                var.set(os.path.normpath(picked))
                if idx == 2:
                    out_auto = False

        ttk.Button(frame, text="Browse...", command=browse).grid(row=r, column=2, padx=(8, 0))
        status = ttk.Label(frame, foreground="gray")
        status.grid(row=r + 1, column=1, columnspan=2, sticky="w", pady=(0, 6))
        statuses.append((var, check, missing, status))

    def refresh(*_):
        if out_auto:
            s = skyrim_folder(skyrim.get())
            if s and out.get() != os.path.join(s, "Data"):
                out.set(os.path.join(s, "Data"))
        for var, check, missing, status in statuses:
            ok = check(var.get())
            status.configure(text=("OK" if ok else missing), foreground=("dark green" if ok else "firebrick"))

    for var in (jc2, skyrim, out):
        var.trace_add("write", refresh)
    refresh()

    button = ttk.Button(frame, text="Install")
    button.grid(row=7, column=0, columnspan=3, pady=8)
    log = tk.Text(frame, height=14, wrap="word", state="disabled")
    log.grid(row=8, column=0, columnspan=3, sticky="nsew")
    scroll = ttk.Scrollbar(frame, command=log.yview)
    scroll.grid(row=8, column=3, sticky="ns")
    log.configure(yscrollcommand=scroll.set)
    frame.rowconfigure(8, weight=1)

    DONE, FAILED = object(), object()

    def worker(a, b, c):
        try:
            build(a, b, c)
            log_queue.put(DONE)
        except Exception:
            traceback.print_exc()
            log_queue.put(FAILED)

    def poll():
        try:
            while True:
                item = log_queue.get_nowait()
                if item is DONE or item is FAILED:
                    button.configure(state="normal")
                    if item is DONE:
                        messagebox.showinfo("Skydive", "Done. The files are in:\n" + out.get() +
                                            "\n\nMod Organizer 2: press F5 (refresh). Vortex: deploy the mods.")
                    else:
                        messagebox.showerror("Skydive", "The install failed. The window shows what went wrong.")
                    continue
                log.configure(state="normal")
                log.insert("end", item)
                log.see("end")
                log.configure(state="disabled")
        except queue.Empty:
            pass
        root.after(100, poll)

    def start():
        a, b, c = jc2_folder(jc2.get()), skyrim_folder(skyrim.get()), out.get()
        if not a or not b or not (c and os.path.isdir(c)):
            messagebox.showerror("Skydive", "Pick all three folders first (the red lines say what's missing).")
            return
        button.configure(state="disabled")
        log.configure(state="normal")
        log.delete("1.0", "end")
        log.configure(state="disabled")
        threading.Thread(target=worker, args=(a, b, os.path.abspath(c)), daemon=True).start()

    button.configure(command=start)
    poll()
    root.mainloop()


def run_cli(args):
    if sys.stdout is None:  # the windowed exe has no console: the output goes to a log file
        log = os.path.join(tempfile.gettempdir(), "Skydive-Installer.log")
        sys.stdout = sys.stderr = open(log, "w", encoding="utf-8")
    jc2 = jc2_folder(args.jc2) if args.jc2 else find_jc2()
    skyrim = skyrim_folder(args.skyrim) if args.skyrim else find_skyrim()
    if not jc2:
        sys.exit("Just Cause 2 not found. Pass its folder with --jc2.")
    if not skyrim:
        sys.exit("Skyrim Special Edition not found. Pass its folder with --skyrim.")
    build(jc2, skyrim, os.path.abspath(args.out or mod_folder() or os.path.join(skyrim, "Data")))
    print("\nDone.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jc2", help="Just Cause 2 folder")
    ap.add_argument("--skyrim", help="Skyrim Special Edition folder")
    ap.add_argument("--out", help="where to write the files")
    ap.add_argument("--cli", action="store_true", help="no window (from Python: the exe has no console)")
    args = ap.parse_args()
    try:
        run_cli(args) if args.cli else run_gui(args)
    finally:
        shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
