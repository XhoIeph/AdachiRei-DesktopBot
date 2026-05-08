import subprocess, os, sys, shutil

def run(cmd, cwd=None):
    return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)

# Locate Berry library dir
libdeps = os.path.join(".pio", "libdeps")
berry_dir = None
for env_dir in os.listdir(libdeps) if os.path.isdir(libdeps) else []:
    path = os.path.join(libdeps, env_dir, "berry")
    if os.path.isdir(os.path.join(path, "src")):
        berry_dir = path
        break

if not berry_dir:
    print("berry_prebuild: Berry library not found, skipping")
    sys.exit(0)

gen_dir = os.path.join(berry_dir, "generate")
os.makedirs(gen_dir, exist_ok=True)

# Check if already generated
strtab = os.path.join(gen_dir, "be_const_strtab.h")
if os.path.exists(strtab) and os.path.getsize(strtab) > 100:
    print("berry_prebuild: be_const_strtab.h already exists, skipping")
    sys.exit(0)

# Try to clone Berry and run code gen
tmp_dir = "/tmp/berry_gen"
if not os.path.isdir(os.path.join(tmp_dir, "tools")):
    if os.path.isdir(tmp_dir):
        shutil.rmtree(tmp_dir)
    r = run("git clone --depth 1 https://github.com/berry-lang/berry.git " + tmp_dir)
    if r.returncode != 0:
        print("berry_prebuild: git clone failed, creating minimal stub")
        # Minimal stub if git fails
        with open(strtab, "w") as f:
            f.write("/* Auto-generated stub */\nstatic const be_const_str_t be_const_strtab[] = {{}};\n")
        sys.exit(0)

# Run Berry's code generator
tools = os.path.join(tmp_dir, "tools")
if os.path.isdir(tools):
    gen_py = os.path.join(tools, "code_gen.py")
    if os.path.isfile(gen_py):
        run(f"{sys.executable} {gen_py}", cwd=tmp_dir)
    else:
        # Try make
        run("make", cwd=tmp_dir)

# Copy generated files
src_gen = os.path.join(tmp_dir, "generate")
berry_src = os.path.join(berry_dir, "src")
if os.path.isdir(src_gen):
    for f in os.listdir(src_gen):
        src = os.path.join(src_gen, f)
        dst = os.path.join(gen_dir, f)
        if os.path.isfile(src):
            shutil.copy2(src, dst)
            print(f"berry_prebuild: copied {f}")
            # Also copy .c files to src/ so PlatformIO compiles them
            if f.endswith(".c"):
                dst_c = os.path.join(berry_src, f)
                if not os.path.exists(dst_c):
                    shutil.copy2(src, dst_c)
                    print(f"berry_prebuild: compiled {f} -> src/")
else:
    # Last resort: create minimal stub
    with open(strtab, "w") as f:
        f.write("/* Auto-generated stub */\nstatic const be_const_str_t be_const_strtab[] = {{}};\n")
    print("berry_prebuild: created minimal stub")

print("berry_prebuild: done")
