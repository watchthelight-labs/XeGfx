#!/usr/bin/env python3
# Compile every drm-kmod source (drm, dmabuf, ttm, i915) for the XNU kernel target with -fsyntax-only and rank what
# breaks: missing headers first, then the most common first errors. The scoreboard is the X1 progress measure.
import argparse, collections, concurrent.futures as cf, glob, json, os, re, shlex, subprocess, sys

SRC = os.path.expanduser("~/src/xe-src")
KMOD = os.path.join(SRC, "drm-kmod")
LKPI = os.path.join(SRC, "freebsd/sys/compat/linuxkpi")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULES = {"drm": "drivers/gpu/drm", "dmabuf": "drivers/dma-buf", "ttm": "drivers/gpu/drm/ttm",
           "i915": "drivers/gpu/drm/i915"}


MAKEVARS = {"MACHINE_CPUARCH": "amd64", "MACHINE_ARCH": "amd64", "NO_FBDEV": "1"}
# drm-kmod features that have no counterpart on XNU
KCONFIG_DROP = {"DEBUG_FS", "PROC_FS", "DRM_PANIC", "VIDEO_CMDLINE", "MTRR", "DRM_I915_PXP", "HWMON"}


def make_cond(expr, kconfig):
    """Evaluate the subset of bmake conditionals drm-kmod uses; unknown forms count as false."""
    def term(t):
        t = t.strip()
        neg = t.startswith("!")
        t = t.lstrip("!").strip()
        m = re.match(r"^(empty|defined)\((\w+)(?::M(\S+))?\)$", t)
        if m:
            fn, var, pat = m.groups()
            if var == "KCONFIG" and pat:
                v = pat in kconfig
            else:
                v = var in MAKEVARS
            v = (not v) if fn == "empty" else v
        else:
            m = re.match(r'^\$\{(\w+)(?::M([^}]*))?\}\s*(==|!=)\s*"([^"]*)"$', t)
            if not m:
                return False
            var, pat, op, val = m.groups()
            cur = MAKEVARS.get(var, "")
            if pat:
                cur = cur if re.fullmatch(pat.replace("*", ".*"), cur) else ""
            v = (cur == val) if op == "==" else (cur != val)
        return v != neg
    return any(all(term(a) for a in o.split("&&")) for o in expr.split("||"))


def make_list(path, var, kconfig=None):
    """Values of VAR (and VAR+=) in a BSD makefile, following .if/.else/.endif."""
    out, cur, stack = [], None, []
    for line in open(path):
        line = line.split("#", 1)[0].rstrip()
        d = re.match(r"^\.\s*(if|elif|else|endif)\b(.*)$", line)
        if d:
            kw, expr = d.groups()
            if kw == "if":
                stack.append([make_cond(expr, kconfig or set()), False])
                stack[-1][1] = stack[-1][0]
            elif kw == "elif":
                take = not stack[-1][1] and make_cond(expr, kconfig or set())
                stack[-1] = [take, stack[-1][1] or take]
            elif kw == "else":
                stack[-1] = [not stack[-1][1], True]
            else:
                stack.pop()
            continue
        if not all(c[0] for c in stack):
            continue
        m = re.match(r"^(\w+)\s*(\+?=)\s*(.*)$", line)
        if m:
            cur = m.group(1) if m.group(1) == var else None
            line = m.group(3)
        if cur:
            cont = line.endswith("\\")
            out += line.rstrip("\\").split()
            if not cont:
                cur = None
    return out


# features drm-kmod leaves off that the shim provides: MMU_NOTIFIER enables i915 userptr (X5, shim/include/linux/mmu_notifier.h)
KCONFIG_ADD = ["MMU_NOTIFIER"]


def kconfig():
    return [v for v in make_list(os.path.join(KMOD, "kconfig.mk"), "KCONFIG") if v.partition("=")[0] not in KCONFIG_DROP] \
        + [k for k in KCONFIG_ADD]


def kconfig_defines():
    d = []
    for v in kconfig():
        # kconfig.mk values are shell words ('"*"' is the C string "*"); clang gets argv without a shell, so unquote
        # them here or the quotes reach the source (N-217: force_probe became the multi-char constant 0x222a22)
        k, _, val = shlex.split(v)[0].partition("=") if v else ("", "", "")
        d.append(f"-DCONFIG_{k}={val or 1}")
    return d


# XEGFX_OVERLAY: a directory with exclude.txt (source basenames to drop), include/ (searched before drm-kmod) and
# src/<module>/*.c (added to that module). The NvGfx fork keeps its non-GPL replacements for drm-kmod's GPL-2.0 files
# there (xkpi). Without an overlay the build still uses drm-kmod's GPL-only gplv2 headers and refuses --obj.
OVERLAY = os.environ.get("XEGFX_OVERLAY")


def overlay_exclude():
    f = os.path.join(OVERLAY, "exclude.txt") if OVERLAY else None
    if not f or not os.path.exists(f):
        return set()
    return {l.split("#")[0].strip() for l in open(f) if l.split("#")[0].strip()}


def overlay_sources(mod):
    d = os.path.join(OVERLAY, "src", mod) if OVERLAY else None
    if not d or not os.path.isdir(d):
        return []
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c"))


KERN_SRCS = ["kern/subr_eventhandler.c"]
# LinuxKPI's own implementation, minus subsystems the GPU stack does not use
LKPI_SKIP = ("80211", "usb", "netdev", "skbuff", "mhi", "page_pool", "i2cbb", "hdmikmod", "videokmod", "cmdline", "linux_i2c", "linux_acpi")


def sources(mod):
    drop = overlay_exclude()
    return [p for p in _sources(mod) if os.path.basename(p) not in drop] + overlay_sources(mod)


def _sources(mod):
    if mod == "kern":  # FreeBSD kernel sources reused unchanged
        return [os.path.join(SRC, "freebsd/sys", f) for f in KERN_SRCS]
    if mod == "shim":
        d = os.path.join(ROOT, "shim/src/fbsd")
        return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c"))
    if mod == "lkpi":
        d = os.path.join(LKPI, "common/src")
        return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c") and not any(k in f for k in LKPI_SKIP))
    mk = os.path.join(KMOD, mod, "Makefile")
    paths = [os.path.join(KMOD, MODULES[mod])]
    for line in open(mk):
        m = re.match(r"^\s*(?:\.PATH:)?\s*\$\{SRCDIR\}(/\S+)", line)
        if m:
            paths.append(os.path.join(KMOD, MODULES[mod]) + m.group(1).rstrip("\\").strip())
    found = []
    for s in make_list(mk, "SRCS", {v.partition("=")[0] for v in kconfig()}):
        if not s.endswith(".c"):
            continue
        for p in paths:
            if os.path.exists(os.path.join(p, s)):
                found.append(os.path.join(p, s))
                break
    return found


def make_cflags(mod):
    """The module Makefile's own CFLAGS+= lines with its make variables resolved."""
    if mod in ("lkpi", "shim", "kern"):
        return ['-DKBUILD_MODNAME="linuxkpi"', "-DLINUXKPI_PARAM_PREFIX=compat_"]
    out, kmodname = [], make_list(os.path.join(KMOD, mod, "Makefile"), "KMOD")
    sub = {"${.CURDIR:H}": KMOD, "${SRCDIR:H:H}": os.path.join(KMOD, "drivers"),
           "${SRCDIR}": os.path.join(KMOD, MODULES[mod]), "${KMOD}": kmodname[0] if kmodname else mod}
    for line in open(os.path.join(KMOD, mod, "Makefile")):
        m = re.match(r"^CFLAGS\s*\+=\s*(.*)$", line.strip())
        if not m or "KCONFIG" in line or "SYSDIR" in line:
            continue
        v = m.group(1)
        for k, r in sub.items():
            v = v.replace(k, r)
        if "${" in v:
            continue
        out += [t.strip("'") for t in re.findall(r"'[^']*'|\S+", v)]
    return out


def flags(mod):
    gpl = [] if OVERLAY else [os.path.join(KMOD, "linuxkpi/gplv2/include")]
    ovl = [os.path.join(OVERLAY, "include")] if OVERLAY and os.path.isdir(os.path.join(OVERLAY, "include")) else []
    inc = [os.path.join(ROOT, "shim/include")] + ovl + gpl + [
           os.path.join(KMOD, "linuxkpi/bsd/include"), os.path.join(LKPI, "common/include"),
           os.path.join(LKPI, "dummy/include"), os.path.join(KMOD, "include"),
           os.path.join(KMOD, "include/uapi"), os.path.join(KMOD, "drivers/gpu/drm"), os.path.join(KMOD, "drivers/gpu"),
           os.path.join(ROOT, "shim/gen"),
           os.path.join(KMOD, MODULES.get(mod, "drivers/gpu/drm")),
           # FreeBSD's own kernel headers as the fallback, machine/ and x86/ mapped to amd64
           os.path.join(ROOT, "shim/fbsd"), os.path.join(SRC, "freebsd/sys"),
           os.path.join(SRC, "freebsd/sys/contrib/ck/include")]
    f = ["-target", "x86_64-apple-macos15.0", "-ffreestanding", "-nostdinc", "-fno-builtin",
         "-mkernel", "-fno-common", "-O2",
         "-std=gnu11", "-D_KERNEL", "-DKLD_MODULE", "-D__FreeBSD__=15", "-U__APPLE__", "-U__MACH__", "-U__APPLE_CC__", "-D__amd64__", "-D__x86_64__", "-D__XNU_LKPI__",
         "-include", os.path.join(LKPI, "common/include/linux/compiler_types.h"),
         "-include", os.path.join(LKPI, "common/include/linux/kconfig.h"),
         "-include", os.path.join(ROOT, "shim/include/xg_bsdnames.h"),
         "-Wno-everything", "-ferror-limit=1"]
    if mod not in ("lkpi", "shim", "kern"):  # consumers only; the base-kernel LinuxKPI is built without it
        f.append("-DLINUXKPI_VERSION=61400")
    mk = make_cflags(mod)
    if OVERLAY:  # the module Makefiles add drm-kmod's gplv2 include dir themselves; the overlay replaces it
        mk = [x for x in mk if "gplv2" not in x]
        f = ["-I", os.path.join(OVERLAY, "include")] + f
    return f + kconfig_defines() + mk + [x for i in inc for x in ("-I", i)]


OBJ = None


PERMISSIVE = ("MIT", "BSD", "X11", "ISC", "Apache", "Zlib", "curl", "Linux-OpenIB")


def gpl_only(path):
    """True when the file's licence is GPL with no permissive alternative (SPDX line, else the GPL boilerplate)."""
    try:
        head = open(path, errors="ignore").read(4096)
    except OSError:
        return False
    m = re.search(r"SPDX-License-Identifier:\s*(.+)", head)
    if m:
        expr = m.group(1).strip().rstrip("*/ ").strip()
        return "GPL" in expr and "Linux-syscall-note" not in expr and not any(k in expr for k in PERMISSIVE)
    return "GNU General Public License" in head and not re.search(r"\b(MIT|BSD|dual)\b", head, re.I)


def licence_gate(jobs):
    files = {p for _, p in jobs}
    for d in glob.glob(os.path.join(OBJ, "*", "*.o.d")):
        for tok in open(d).read().replace("\\\n", " ").split()[1:]:
            if tok.endswith((".h", ".c")):
                files.add(os.path.normpath(tok))
    bad = sorted(f for f in files if gpl_only(f))
    with open(os.path.join(ROOT, "out", "licence.txt"), "w") as f:
        f.write("\n".join(bad) + ("\n" if bad else ""))
    return bad


def compile_one(args):
    mod, path = args
    out = ["-fsyntax-only"]
    if OBJ:
        o = os.path.join(OBJ, mod, os.path.basename(path) + ".o")
        os.makedirs(os.path.dirname(o), exist_ok=True)
        out = ["-c", "-o", o, "-MD", "-MF", o + ".d"]
    fl = flags(mod)
    if mod == "shim":  # our own code: real warnings
        fl = [x for x in fl if x not in ("-Wno-everything", "-ferror-limit=1")] + ["-ferror-limit=40", "-Wall", "-Wno-sign-conversion", "-Wno-shorten-64-to-32", "-Wno-macro-redefined", "-Wno-unused-variable", "-Wno-unused-function", "-Werror=implicit-function-declaration"]
    r = subprocess.run(["xcrun", "clang"] + fl + out + [path], capture_output=True, text=True)
    err = next((l for l in r.stderr.splitlines() if " error: " in l), "")
    if mod == "shim" and r.stderr.strip():
        print(r.stderr)
    return mod, os.path.relpath(path, KMOD), r.returncode == 0, err


def ensure_links():
    fb = os.path.join(ROOT, "shim/fbsd")
    os.makedirs(fb, exist_ok=True)
    for name, target in (("machine", "freebsd/sys/amd64/include"), ("x86", "freebsd/sys/x86/include")):
        link = os.path.join(fb, name)
        if not os.path.lexists(link):
            os.symlink(os.path.join(SRC, target), link)


def main():
    ensure_links()
    ap = argparse.ArgumentParser()
    ap.add_argument("--mods", default="shim,kern,lkpi,drm,dmabuf,ttm,i915")
    ap.add_argument("--obj", action="store_true", help="build objects in out/obj and list undefined symbols")
    ap.add_argument("--one", help="compile one shim source (shim flags, all errors shown) into out/one/; for parallel work")
    ap.add_argument("--json", default=os.path.join(ROOT, "out/score.json"))
    a = ap.parse_args()
    if a.one:
        src = os.path.abspath(a.one)
        o = os.path.join(ROOT, "out/one", os.path.basename(src) + ".o")
        os.makedirs(os.path.dirname(o), exist_ok=True)
        fl = [x for x in flags("shim") if x not in ("-Wno-everything", "-ferror-limit=1")]
        fl += ["-ferror-limit=60", "-Wall", "-Wno-sign-conversion", "-Wno-shorten-64-to-32", "-Wno-macro-redefined",
               "-Wno-unused-variable", "-Wno-unused-function", "-Werror=implicit-function-declaration"]
        r = subprocess.run(["xcrun", "clang"] + fl + ["-c", "-o", o, src], capture_output=True, text=True)
        msgs = [l for l in r.stderr.splitlines() if src in l and (" error: " in l or " warning: " in l)]
        print("\n".join(msgs) if msgs else r.stderr[-4000:])
        if r.returncode == 0:
            und = [l.split()[-1] for l in subprocess.run(["nm", "-u", o], capture_output=True, text=True).stdout.split()]
            print(f"OK {o}; undefined in this object: {len(und)}")
        else:
            print("FAILED")
        return r.returncode
    global OBJ
    if a.obj:
        OBJ = os.path.join(ROOT, "out/obj")
    jobs = [(m, p) for m in a.mods.split(",") for p in sources(m)]
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        res = list(ex.map(compile_one, jobs))
    if OBJ:
        bad = licence_gate(jobs)
        print(f"licence gate: {len(bad)} GPL-only files compiled or included (out/licence.txt)")
        if bad and OVERLAY:
            print("\n".join("  " + b for b in bad[:40]))
            return 2
    ok = collections.Counter(m for m, _, good, _ in res if good)
    tot = collections.Counter(m for m, _, _, _ in res)
    missing, kinds = collections.Counter(), collections.Counter()
    for _, _, good, e in res:
        if good:
            continue
        m = re.search(r"'([^']+)' file not found", e)
        if m:
            missing[m.group(1)] += 1
        else:
            kinds[re.sub(r"^.*? error: ", "", e)[:110]] += 1
    for m in tot:
        print(f"{m:7s} {ok[m]:4d}/{tot[m]:4d} compile")
    print(f"total   {sum(ok.values()):4d}/{sum(tot.values()):4d}")
    print("\nmissing headers (files blocked):")
    for h, n in missing.most_common(25):
        print(f"  {n:4d}  {h}")
    print("\nfirst errors:")
    for k, n in kinds.most_common(25):
        print(f"  {n:4d}  {k}")
    if OBJ:
        undefined_report()
    json.dump([dict(mod=m, file=f, ok=g, err=e) for m, f, g, e in res], open(a.json, "w"), indent=1)


def undefined_report():
    """Symbols the objects need that none of them define: the FreeBSD kernel API to implement on XNU."""
    defined, need = set(), collections.Counter()
    objs = [os.path.join(dp, f) for dp, _, fs in os.walk(OBJ) for f in fs if f.endswith(".o")]
    for o in objs:
        for line in subprocess.run(["nm", "-g", o], capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                need[parts[1]] += 1
            elif len(parts) == 3:
                defined.add(parts[2])
    und = {k: v for k, v in need.items() if k not in defined}
    open(os.path.join(ROOT, "out/undefined.txt"), "w").write("".join(f"{v} {k}\n" for k, v in sorted(und.items(), key=lambda x: -x[1])))
    print(f"\nundefined symbols: {len(und)} (out/undefined.txt)")
    for k, v in sorted(und.items(), key=lambda x: -x[1])[:40]:
        print(f"  {v:4d}  {k}")


if __name__ == "__main__":
    sys.exit(main())
