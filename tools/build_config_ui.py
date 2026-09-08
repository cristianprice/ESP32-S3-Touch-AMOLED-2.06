Import("env")

import os
from pathlib import Path
import shutil
import subprocess
import sys

project_dir = Path(env["PROJECT_DIR"])
client_dir = project_dir / "config-ui"
bundle_dir = client_dir / "dist" / "config-ui" / "browser"
data_dir = project_dir / "data"


def find_npm():
    candidates = [
        shutil.which("npm"),
        Path("/usr/local/bin/npm"),
        Path("/usr/bin/npm"),
    ]
    candidates.extend(sorted(Path.home().glob(".nvm/versions/node/*/bin/npm"), reverse=True))

    for candidate in candidates:
        if candidate is not None and Path(candidate).is_file():
            return Path(candidate)

    raise RuntimeError("npm was not found; install Node.js or add npm to PATH before building.")


npm = find_npm()
subprocess_environment = os.environ.copy()
subprocess_environment["PATH"] = f"{npm.parent}{os.pathsep}{subprocess_environment.get('PATH', '')}"
subprocess.run([npm, "run", "build"], cwd=client_dir, check=True, env=subprocess_environment)

if data_dir.exists():
    shutil.rmtree(data_dir)
shutil.copytree(bundle_dir, data_dir)


def upload_all(source, target, env):
    command = [
        sys.executable,
        "-m",
        "platformio",
        "run",
        "--project-dir",
        str(project_dir),
        "--environment",
        env["PIOENV"],
    ]
    subprocess.run(command + ["--target", "uploadfs"], check=True)
    subprocess.run(command + ["--target", "upload"], check=True)


env.AddCustomTarget(
    name="uploadall",
    dependencies=None,
    actions=upload_all,
    title="Upload Firmware and Web UI",
    description="Upload the SPIFFS web files, then the firmware.",
)
