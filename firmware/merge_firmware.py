Import("env")
import os

def merge_bin(source, target, env):
    firmware = str(target[0])
    board = env.BoardConfig()
    mcu = board.get("build.mcu", "esp32")
    flash_mode = env.GetProjectOption("board_build.flash_mode", "dio")
    flash_freq = board.get("build.f_flash", "40000000L").replace("L", "")
    flash_freq_m = str(int(int(flash_freq) / 1000000)) + "m"
    flash_size = board.get("upload.flash_size", "4MB")

    # PlatformIO 8.x 把 esptool 包从 tool-esptoolpy(esptool.py) 换成了
    # tool-esptool(esptool.exe)，这里两种都兼容。
    platform = env.PioPlatform()
    esptool = ""
    for pkg_name in ("tool-esptool", "tool-esptoolpy"):
        try:
            pkg_dir = platform.get_package_dir(pkg_name)
        except Exception:
            pkg_dir = None
        if not pkg_dir:
            continue
        for exe_name in ("esptool.py", "esptool.exe"):
            cand = os.path.join(pkg_dir, exe_name)
            if os.path.isfile(cand):
                esptool = cand
                break
        if esptool:
            break
    if not esptool:
        print("[merge] esptool not found, skip merged binary")
        return
    output = os.path.join(env.subst("$BUILD_DIR"), "firmware_merged.bin")

    extra = env.get("FLASH_EXTRA_IMAGES", [])
    print("=== FLASH_EXTRA_IMAGES ===")
    for offset, path in extra:
        print(f"  {offset} -> {env.subst(path)}")
    print(f"  APP -> 0x10000 {firmware}")
    print("==========================")

    cmd = (
        ["$PYTHONEXE", esptool] if esptool.endswith(".py") else [esptool]
    ) + [
        "--chip", mcu,
        "merge_bin",
        "--flash_mode", flash_mode,
        "--flash_freq", flash_freq_m,
        "--flash_size", flash_size,
        "-o", output,
    ]

    for offset, path in extra:
        cmd.extend([offset, env.subst(path)])

    cmd.extend(["0x10000", firmware])

    env.Execute(env.VerboseAction(" ".join(cmd), "Merging firmware into single binary"))

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)
