import os

Import("env")


if os.name == "nt":
    toolchain_dir = os.path.join(
        env.subst("$PROJECT_PACKAGES_DIR"),
        "toolchain-gccmingw32",
    )
    compiler = os.path.join(toolchain_dir, "bin", "g++.exe")
    if not os.path.isfile(compiler):
        raise RuntimeError(
            "Missing native compiler. Install it with: "
            "pio pkg install --global --tool "
            "platformio/toolchain-gccmingw32@1.50100.0"
        )

    env.PrependENVPath("PATH", os.path.join(toolchain_dir, "bin"))
    env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
