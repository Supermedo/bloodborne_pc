"""Optional final-save backup after the native process has exited, including crashes."""
import bbport_saves as saves
import subprocess

def run_with_backup(command, user_dir, game_dir='', creationflags=0):
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, creationflags=creationflags)
    return wait_and_backup(process, user_dir, game_dir, enabled=True)

def wait_and_backup(process, user_dir, game_dir='', enabled=False, log=print):
    """Return the game's exit code unchanged even if a backup cannot be written."""
    code = process.wait()
    if enabled:
        try:
            path = saves.backup(user_dir, game_dir, reason='exit',
                                exit_info={'game_pid': process.pid, 'exit_code': code})
            log('Exit save backup: ' + (str(path) if path else 'no save files yet'))
        except Exception as error:
            try:
                log(f'Exit save backup failed: {type(error).__name__}: {error}')
            except Exception:
                pass
    return code
