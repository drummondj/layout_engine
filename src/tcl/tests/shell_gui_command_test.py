"""Interactive le_shell on a pseudo-terminal runs a GUI-queued Tcl command.

Readline only calls le_shell's event hook, which runs commands queued by
GUI actions, when stdin is a terminal. The test queues one with
`le_shell_enqueue_tcl_command` while a half-typed line sits at the prompt:
the command must run, the shell must survive, and the half-typed line
must still be there to finish. HOME points at the work directory so the
user's saved settings are neither read nor written.

Usage: shell_gui_command_test.py <le_shell> <le_tcl module> <procs> <work dir>
"""

import os
import pty
import select
import subprocess
import sys
import time


def main() -> int:
    le_shell, module, procs, work_dir = sys.argv[1:5]
    os.makedirs(work_dir, exist_ok=True)
    primary, secondary = pty.openpty()
    proc = subprocess.Popen(
        [le_shell, "-module", module, "-procs", procs],
        stdin=secondary,
        stdout=secondary,
        stderr=secondary,
        env={**os.environ, "HOME": work_dir, "TERM": "xterm"},
        close_fds=True,
    )
    os.close(secondary)
    output = bytearray()

    def read_until(marker: bytes, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while marker not in output:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([primary], [], [], remaining)[0]:
                return False
            try:
                chunk = os.read(primary, 4096)
            except OSError:
                return False
            if not chunk:
                return False
            output.extend(chunk)
        return True

    def fail(message: str) -> int:
        proc.kill()
        proc.wait()
        print(f"{message}\noutput:\n{output.decode(errors='replace')}", file=sys.stderr)
        return 1

    if not read_until(b"le_shell > ", 60):
        return fail("no prompt")
    # The typed line queues the command; "puts ab" is half-typed at the
    # next prompt when the event hook runs it.
    os.write(primary, b"le_shell_enqueue_tcl_command {puts gui_[string toupper command]_ran}\nputs ab")
    if not read_until(b"gui_COMMAND_ran", 20):
        return fail("queued command didn't run")
    if not read_until(b"le_shell > puts ab", 10):
        return fail("half-typed line not redrawn")
    os.write(primary, b"c\n")
    if not read_until(b"abc", 10):
        return fail("half-typed line lost")
    # ::le_shell_builtin_exit skips the interactive `exit`'s unsaved-changes prompt.
    os.write(primary, b"::le_shell_builtin_exit 3\n")
    try:
        status = proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        return fail("le_shell didn't exit")
    if status != 3:
        return fail(f"expected exit status 3, got {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
