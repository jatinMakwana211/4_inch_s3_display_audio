import json
import pathlib
import subprocess
entries = json.load(open('build_voice/compile_commands.json'))
failed = False
for entry in entries:
    name = pathlib.Path(entry['file']).name
    if name not in {'main.c', 'voice_recorder.c', 'audio_player.c'}:
        continue
    result = subprocess.run(entry['command'], cwd=entry['directory'], capture_output=True, text=True, timeout=45)
    print(name, result.returncode, result.stdout, result.stderr)
    failed |= result.returncode != 0
raise SystemExit(failed)
