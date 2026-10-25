"""Sends one human-monitor (HMP) command to a dockur VM's QEMU and prints the answer.

Run inside the dockur container, whose QEMU listens on /run/shm/monitor.sock (dockur's `-monitor unix:...`):
    docker exec -i <container> python3 - "info balloon" < hmp.py
Used by the build server's measurements to read and set the balloon (`info balloon`, `balloon <MiB>`).
The monitor prints a banner and a "(qemu)" prompt; the answer is what arrives between the command's echo and the next
prompt. Exits 1 when the socket is missing or does not answer within the timeout.
"""
import socket
import sys
import time

SOCKET_PATH = "/run/shm/monitor.sock"


def read_until_prompt(conn: socket.socket, timeout: float) -> str:
    """Reads from the monitor until its "(qemu)" prompt or the timeout.

    Args:
        conn: the connected monitor socket.
        timeout: seconds to wait in total.

    Returns:
        Everything read, decoded as UTF-8 with undecodable bytes replaced.
    """
    deadline = time.monotonic() + timeout
    data = b""
    while time.monotonic() < deadline:
        conn.settimeout(max(0.05, deadline - time.monotonic()))
        try:
            chunk = conn.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
        if data.rstrip().endswith(b"(qemu)"):
            break
    return data.decode("utf-8", "replace")


def main() -> int:
    """Connects, waits for the banner's prompt, sends argv[1], prints the answer.

    Returns:
        0 on an answer, 1 when the monitor cannot be reached.
    """
    command = sys.argv[1] if len(sys.argv) > 1 else "info balloon"
    try:
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.connect(SOCKET_PATH)
    except OSError as error:
        print(f"monitor not reachable: {error}")
        return 1
    with conn:
        read_until_prompt(conn, 3.0)
        conn.sendall(command.encode() + b"\n")
        answer = read_until_prompt(conn, 5.0)
    # The answer starts with the echoed command and ends with the next prompt; keep the lines in between.
    lines = [line for line in answer.replace("\r", "").split("\n") if line.strip() and "(qemu)" not in line]
    lines = [line for line in lines if line.strip() != command]
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
