"""
_build_helper.py  —  called by build.bat to compile SuperVrExport and GeoVrExport.
Python handles subprocess argument quoting correctly regardless of spaces in paths.
"""
import sys, os, subprocess, shutil

def run(cmd, **kw):
    print(" ".join(f'"{a}"' if " " in str(a) else str(a) for a in cmd))
    r = subprocess.run(cmd, **kw)
    return r.returncode

def setup_env(vcvars, arch):
    """Run vcvarsall and capture the resulting environment."""
    cmd = f'"{vcvars}" {arch} && set'
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    env = {}
    for line in r.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            env[k] = v
    return env

def build(vcvars, src, out, inc, arch, env_base):
    """Compile one addon for one architecture."""
    name = os.path.basename(out)
    print(f"\n  Building {name} ({arch})...")

    env = setup_env(vcvars, arch)
    if not env:
        print(f"  ERROR: vcvarsall failed for {arch}")
        return 1

    # Prepend our include dirs to INCLUDE — cl reads this automatically.
    # No /I flag needed, so no quoting issues with spaces in paths.
    base_inc = env.get("INCLUDE", "")
    env["INCLUDE"] = inc + os.pathsep + src + os.pathsep + base_inc

    # Vulkan SDK
    vulkan_sdk = os.environ.get("VULKAN_SDK", "")
    vulkan_def = "/DSUPVR_VULKAN=0"
    if vulkan_sdk and os.path.isdir(vulkan_sdk):
        env["INCLUDE"] = os.path.join(vulkan_sdk, "Include") + os.pathsep + env["INCLUDE"]
        vulkan_def = "/DSUPVR_VULKAN=1"

    # Temp obj dir — separate per arch to avoid x86/x64 collision
    obj_dir = os.path.join(os.path.dirname(out), f"obj_{arch}")
    os.makedirs(obj_dir, exist_ok=True)

    os.makedirs(os.path.dirname(out), exist_ok=True)

    # GeoVrExport: strip /GL and /LTCG to keep binary small like the original.
    # /EHsc REQUIRED: the original uses std::stringstream / C++ exceptions.
    if "GeoVrExport" in str(out):
        cflags = ["/nologo", "/std:c++17", "/O2", "/EHsc", "/MD", "/W3",
                  "/DWIN32", "/D_WINDOWS", "/DNDEBUG", "/DUNICODE", "/D_UNICODE"]
        lflags = ["/DLL", "/OPT:REF", "/OPT:ICF", "/INCREMENTAL:NO"]
    else:
        cflags = ["/nologo", "/std:c++17", "/O2", "/GL", "/EHsc", "/W3",
                  "/DWIN32", "/D_WINDOWS", "/DNDEBUG", "/DUNICODE", "/D_UNICODE"]
        lflags = ["/DLL", "/OPT:REF", "/OPT:ICF", "/LTCG", "/INCREMENTAL:NO"]
    # GeoVrExport mirrors the ORIGINAL addon, which has BOTH a D3D11 and a D3D12
    # sharing path — so it needs d3d12.lib too (the original linked it).
    if "GeoVrExport" in str(out):
        libs = ["kernel32.lib", "user32.lib", "d3d11.lib", "d3d12.lib", "dxgi.lib"]
    else:
        libs = ["kernel32.lib", "user32.lib", "d3d9.lib", "d3d10.lib",
                "d3d11.lib", "d3d12.lib", "dxgi.lib", "opengl32.lib"]

    cmd = (
        ["cl"] + cflags + [vulkan_def,
        f"/Fo{obj_dir}\\",
        ] + (["dllmain.cpp"] if "GeoVrExport" in str(out) else ["dllmain.cpp","pch.cpp"]) + [
        "/link"] + lflags + libs + [f"/OUT:{out}"]
    )

    # shell=True so Windows uses the captured PATH (which has cl.exe) for lookup.
    import shlex
    cmd_str = subprocess.list2cmdline(cmd)
    r = subprocess.run(cmd_str, cwd=src, env=env, shell=True)
    if r.returncode != 0:
        print(f"  ERROR: Build failed for {name} ({arch})")
    return r.returncode

def main():
    # Args: vcvars src_super src_geo bindir inc_633 inc_64x inc_67x has_64x has_67x
    vcvars, src_super, src_geo, bindir, inc_633, inc_64x, inc_67x, has_64x, has_67x = sys.argv[1:10]
    has_64x = has_64x.strip() == "1"
    has_67x = has_67x.strip() == "1"

    # Build one output folder per header set that is available.
    header_sets = [
        (inc_633, "reshade_6.3.x"),
    ]
    if has_64x:
        header_sets.append((inc_64x, "reshade_6.4.x"))
    if has_67x:
        header_sets.append((inc_67x, "reshade_6.7.x"))

    configs = []
    for inc, subfolder in header_sets:
        outdir = os.path.join(bindir, subfolder)
        for addon, src in [("SuperVrExport", src_super), ("GeoVrExport", src_geo)]:
            for arch, ext in [("x86", "addon32"), ("x64", "addon64")]:
                configs.append((src, os.path.join(outdir, f"{addon}.{ext}"), inc, arch))

    total = len(configs)
    for i, (src, out, inc, arch) in enumerate(configs, 1):
        name = os.path.basename(out)
        folder = os.path.basename(os.path.dirname(out))
        print(f"\n[{i}/{total}] {name}  ({folder}, {arch})")
        rc = build(vcvars, src, out, inc, arch, {})
        if rc != 0:
            return rc

    folders = "  ".join(f"bin\\{s}\\" for _, s in header_sets)
    print(f"""
============================================================
  ALL BUILDS SUCCESSFUL

  SuperVrExport  for SuperDepth3D games (SD3D + 3DToElse)
  GeoVrExport    for Geo3D / native frame-sequential games

  {folders}

  .addon64 = 64-bit games   .addon32 = 32-bit games

  Copy ONE addon into your game folder (same as dxgi.dll).
============================================================""")
    return 0

sys.exit(main())
