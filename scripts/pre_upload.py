Import("env")
import subprocess, sys

# pio run -e server --target upload の前に
# pio run -e server --target uploadfs を自動実行する
def upload_fs_before_firmware(source, target, env):
    print("\n>>> Uploading filesystem (LittleFS) first...\n")
    ret = subprocess.run(
        [sys.executable, "-m", "platformio", "run",
         "-e", env["PIOENV"], "--target", "uploadfs"],
        check=False
    )
    if ret.returncode != 0:
        print("\n[ERROR] uploadfs failed. Aborting firmware upload.\n")
        env.Exit(1)
    print("\n>>> Filesystem uploaded. Proceeding with firmware...\n")

env.AddPreAction("upload", upload_fs_before_firmware)
