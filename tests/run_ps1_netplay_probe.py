"""Check real PS1 workers on Windows or Windows and WSL without opening a window.

Set ANYPS5_PS1_NETPLAY_TOOLS to AnyPS5 Desktop's tool/ps1_netplay directory,
which holds the trace format, the CTR finish gate and the network model."""

import argparse
import hashlib
from contextlib import ExitStack
import json
import os
from pathlib import Path
from queue import Queue
from shutil import copyfile
from subprocess import CREATE_NO_WINDOW, IDLE_PRIORITY_CLASS, PIPE, Popen, TimeoutExpired, run
import sys
from tempfile import TemporaryDirectory
from threading import Thread
from time import monotonic, perf_counter, sleep


class Peer:
    def __init__(self, executable, core, disc, bios, seat, *, local=False, saves=None, recompiler=False,
                 rollback=False, audio_control=False, input_trace=None, presentation_core=None, linux=False, step_breakdown=False):
        self.lines = Queue()
        arguments = [executable, core, disc, bios, str(seat)]
        if local:
            arguments.append("local")
        if recompiler and not (rollback or audio_control):
            arguments.append("recompiler")
        if rollback:
            mode = "rollback-dual" if presentation_core else "rollback"
            arguments.append(mode + ("-recompiler" if recompiler else ""))
            if presentation_core:
                arguments.append(presentation_core)
            if input_trace is not None:
                arguments.append(input_trace)
        if audio_control:
            arguments.append("audio-control-recompiler" if recompiler else "audio-control")
        if saves is not None:
            arguments.extend(("ordinary", saves))
        if linux:
            paths = [executable, core, disc, bios, input_trace]
            linux_paths = [
                run(["wsl.exe", "-d", "Ubuntu", "--", "wslpath", "-a", path.as_posix()],
                    capture_output=True, text=True, check=True, creationflags=CREATE_NO_WINDOW).stdout.strip()
                for path in paths
            ]
            arguments = ["wsl.exe", "-d", "Ubuntu", "--", "env", "TMPDIR=/dev/shm",
                         "nice", "-n", "19", "ionice", "-c3", "timeout", "-k", "10s", "540s",
                         *linux_paths[:4], str(seat), "rollback", linux_paths[4]]
        self.process = Popen(
            arguments,
            stdin=PIPE, stdout=PIPE, text=True,
            env={**os.environ, "ANYPS5_PS1_PROBE_STEP_TIMING": "1" if step_breakdown else ""},
            creationflags=CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS,
        )
        Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.strip())
        self.lines.put(None)

    def receive(self):
        line = self.lines.get(timeout=35)
        if line is None:
            raise RuntimeError(f"Native probe exited with code {self.process.wait(timeout=5)}")
        return line

    def send(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.receive()

    def close(self):
        try:
            if self.process.poll() is None:
                try:
                    self.process.stdin.write("quit\n")
                    self.process.stdin.flush()
                except BrokenPipeError:
                    pass
            self.process.wait(timeout=10)
        except TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise
        if self.process.returncode:
            raise RuntimeError(f"Native probe exited with code {self.process.returncode}")

    def sync(self, acknowledgement=-1, inputs=()):
        values = [acknowledgement, len(inputs)]
        values.extend(value for row in inputs for value in row)
        result = iter(self.send("sync " + " ".join(map(str, values))).split())
        frame, received, count = (int(next(result)) for _ in range(3))
        local = [(int(next(result)), int(next(result))) for _ in range(count)]
        fingerprints = {int(next(result)): next(result) for _ in range(int(next(result)))}
        if next(result, None) is not None:
            raise RuntimeError("Unexpected native probe response")
        return frame, received, local, fingerprints


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for path in ("probe", "core", "disc", "bios"):
        parser.add_argument("--" + path, type=Path, required=True)
    parser.add_argument("--frames", type=int, default=650)
    parser.add_argument("--rollback", action="store_true",
                        help="Compare a native prediction worker to a confirmed-input control")
    parser.add_argument("--rollback-recompiler", action="store_true",
                        help="Use the ordinary Play CPU profile in rollback, including ordinary restore probes")
    parser.add_argument("--presentation-core", type=Path,
                        help="Use a separate byte-identical DLL for video during native rollback")
    parser.add_argument("--trace", type=Path, help="Drive the exact two-player CTR fixture and require the race finish")
    parser.add_argument("--network-profile", choices=("clean", "120ms-loss"), default="120ms-loss",
                        help="Network model for a paced rollback trace")
    parser.add_argument("--sample-frames", type=int, help="Measure a prefix of a paced rollback trace")
    parser.add_argument("--checkpoint-guest", type=Path, help="Verify a held peer can import with a renamed CUE")
    parser.add_argument("--cutover", action="store_true", help="Replace the second player and verify the new input epoch")
    parser.add_argument("--cutover-frame", type=int, help="Trace frame where the replacement guest takes over")
    parser.add_argument("--local-handoff", action="store_true", help="Join a host already playing locally")
    parser.add_argument("--ordinary-checkpoint", action="store_true",
                        help="Verify that normal play exports only a blank match card")
    parser.add_argument("--ordinary-control-core", type=Path,
                        help="Also check an older core lacks the ordinary-play export capability")
    parser.add_argument("--ordinary-restore", action="store_true",
                        help="Restore an ordinary hardware checkpoint into two isolated match workers")
    parser.add_argument("--ordinary-park", action="store_true",
                        help="Hold and recover the original worker using its private native checkpoint")
    parser.add_argument("--ordinary-replace", action="store_true",
                        help="Exercise replacing and recovering workers through the native method channel")
    parser.add_argument("--ordinary-continue", action="store_true",
                        help="Recover the latest confirmed rollback frame through the native method channel")
    parser.add_argument("--linux-probe", type=Path, help="Use a headless Ubuntu worker for seat two")
    parser.add_argument("--linux-core", type=Path, help="Linux SwanStation library accessible from WSL")
    parser.add_argument("--work-breakdown", action="store_true",
                        help="Read per-stage work timing from a rebuilt Windows probe")
    parser.add_argument("--step-breakdown", action="store_true",
                        help="Read video-callback time and thread cycles from a rebuilt Windows probe")
    args = parser.parse_args()
    if args.linux_probe or args.linux_core:
        if not (args.linux_probe and args.linux_core and args.rollback and args.trace):
            parser.error("Mixed-platform runs require both Linux paths, --rollback and --trace")
        if args.rollback_recompiler or args.presentation_core:
            parser.error("Mixed-platform runs use the cached interpreter and one worker per peer")
    if (args.work_breakdown or args.step_breakdown) and not (args.rollback and args.trace):
        parser.error("Work and step breakdowns require a paced rollback trace")
    rollback_restore = args.ordinary_restore and args.rollback_recompiler
    if args.rollback_recompiler and not (args.rollback or rollback_restore):
        parser.error("--rollback-recompiler requires --rollback or --ordinary-restore")
    if args.presentation_core:
        if not (args.rollback or rollback_restore) or args.presentation_core.samefile(args.core):
            parser.error("--presentation-core requires rollback and a separate DLL copy")
        with args.core.open("rb") as primary, args.presentation_core.open("rb") as presentation:
            if hashlib.file_digest(primary, "sha256").digest() != hashlib.file_digest(presentation, "sha256").digest():
                parser.error("The presentation DLL must match the primary DLL")
    if args.frames < 120:
        parser.error("at least 120 frames are required to compare a fingerprint")
    if (args.sample_frames is not None or args.network_profile != "120ms-loss") and not (args.rollback and args.trace):
        parser.error("Network diagnostics require --rollback and --trace")
    if args.sample_frames is not None and args.sample_frames < 120:
        parser.error("--sample-frames must include at least 120 frames")
    if args.rollback and (args.checkpoint_guest or args.ordinary_checkpoint or args.local_handoff):
        parser.error("--rollback runs independently of the checkpoint transfer probes")
    if args.cutover and not args.checkpoint_guest:
        parser.error("--cutover requires --checkpoint-guest")
    if args.cutover_frame is not None and not (args.cutover and args.trace):
        parser.error("--cutover-frame requires --cutover and --trace")
    if args.cutover and args.trace and args.cutover_frame is None:
        parser.error("a traced cutover requires --cutover-frame")
    if args.local_handoff and (not args.checkpoint_guest or args.trace or args.cutover):
        parser.error("--local-handoff requires --checkpoint-guest and runs independently of --trace and --cutover")
    if args.ordinary_checkpoint and (args.local_handoff or args.checkpoint_guest or args.trace):
        parser.error("--ordinary-checkpoint runs independently of netplay checks")
    if args.ordinary_control_core and not args.ordinary_checkpoint:
        parser.error("--ordinary-control-core requires --ordinary-checkpoint")
    if args.ordinary_restore and not args.ordinary_checkpoint:
        parser.error("--ordinary-restore requires --ordinary-checkpoint")
    if args.ordinary_park and not args.ordinary_restore:
        parser.error("--ordinary-park requires --ordinary-restore")
    if args.ordinary_replace and (not args.ordinary_checkpoint or not args.ordinary_control_core):
        parser.error("--ordinary-replace requires --ordinary-checkpoint and --ordinary-control-core")
    if args.ordinary_continue and not (args.ordinary_replace and rollback_restore and args.presentation_core):
        parser.error("--ordinary-continue requires rollback restore, a presentation core and --ordinary-replace")

    with ExitStack() as cleanup:
        if args.rollback:
            if args.trace:
                traced_rollback_race(args, cleanup)
            else:
                verify_native_rollback(args, cleanup)
            return
        if args.ordinary_checkpoint:
            verify_ordinary_checkpoint(args, cleanup)
            return
        if args.local_handoff:
            verify_local_handoff(args, cleanup)
            return
        frames = read_trace(args.trace) if args.trace else None
        peers = []
        for seat in range(2):
            peer = Peer(args.probe.resolve(), args.core.resolve(), args.disc.resolve(), args.bios.resolve(), seat)
            cleanup.callback(peer.close)
            peers.append(peer)
        assert all(peer.receive() == "ready" for peer in peers)
        states = [peer.sync() for peer in peers]
        assert all(state[:3] == (0, -1, [(0, 0), (1, 0)]) for state in states)
        assert peers[0].send("start") == "started"
        sleep(0.075)
        assert peers[0].sync()[0] == 0, "A missing remote input advanced the emulator"
        assert peers[1].send("start") == "started"
        if args.trace:
            traced_race(peers, frames, args, cleanup)
            return
        assert peers[0].send("pad 8") == "pad"
        assert peers[1].send("pad 256") == "pad"
        histories = [{}, {}]
        active = [False, False]
        def advance(target):
            nonlocal states
            deadline = monotonic() + (target - min(state[0] for state in states)) / 25 + 15
            while min(state[0] for state in states) < target:
                if monotonic() > deadline:
                    raise TimeoutError(f"Both peers did not reach the target, frames={list(s[0] for s in states)}")
                states = [peer.sync(states[1 - seat][1], states[1 - seat][2])
                          for seat, peer in enumerate(peers)]
                for seat, state in enumerate(states):
                    histories[seat].update(state[3])
                    active[seat] |= any(buttons == (8 if seat == 0 else 256) for _, buttons in state[2])
                sleep(0.002)

        advance(args.frames)
        assert all(active), "A local controller never entered the exchanged timeline"
        for frame in range(119, args.frames, 120):
            assert histories[0][frame] == histories[1][frame], f"PS1 RAM diverged at frame {frame}"
        print(f"Both embedded cores confirmed {args.frames} frames, with both seats active "
              f"and {args.frames // 120} matching RAM fingerprints.")
        if args.checkpoint_guest:
            def during_transfer(frame):
                assert peers[0].send("pad 64") == "pad"
                assert peers[1].send("pad 8") == "pad"
                advance(frame + 180)
            verify_checkpoint(args, peers[0], cleanup, during_transfer)


def verify_ordinary_checkpoint(args, cleanup):
    root = Path(cleanup.enter_context(TemporaryDirectory(prefix="anyps5-ordinary-ps1-")))
    saves = root / "saves"
    saves.mkdir()
    bios = root / "scph1001.bin"
    copyfile(args.bios, bios)
    disc = args.disc.resolve()
    card = saves / (disc.stem + ".srm")
    marker = b"AnyPS5 synthetic ordinary save marker"
    snapshot = root / "snapshot"
    path = json.dumps(snapshot.as_posix())

    def boot(core, owner):
        peer = Peer(args.probe.resolve(), core.resolve(), disc, bios, 0, saves=saves)
        owner.callback(peer.close)
        assert peer.receive() == "ready"
        return peer

    with ExitStack() as seed:
        boot(args.core, seed)
    blank = card.read_bytes()
    assert len(blank) == 128 * 1024
    private = blank[:-len(marker)] + marker
    card.write_bytes(private)

    with ExitStack() as active:
        peer = boot(args.core, active)
        renderer = peer.send("renderer")
        assert peer.send("local-support") == "yes"
        assert peer.send("resume-checkpoint") == "unavailable"
        assert peer.send(f"checkpoint 1 {path}") == "unavailable"
        assert peer.send(f"stale-blank 1 {path}") == "unavailable"
        assert peer.send(f"cancel-blank 1 {path}") == "cancelled"
        assert not snapshot.exists()
        before = int(peer.send("published"))
        deadline = monotonic() + 3
        while True:
            reply = peer.send(f"blank-checkpoint 1 {path}")
            if reply not in ("rejected", "unavailable") or monotonic() >= deadline:
                break
            sleep(0.017)
        name, frame, size = reply.split()
        data = snapshot.read_bytes()
        assert name == "checkpoint" and int(frame) > 0 and int(size) == len(data)
        assert marker not in data and blank in data
        assert card.read_bytes() == private
        deadline = monotonic() + 3
        while int(peer.send("published")) <= before + 1:
            if monotonic() >= deadline:
                raise TimeoutError("Ordinary play stopped publishing after the checkpoint")
            sleep(0.017)
        deadline = monotonic() + 3
        while True:
            assert peer.send("pause") == "pause"
            sleep(0.05)
            paused = peer.send("published")
            # A busy boundary still completes the request without moving
            # the paused source. A match can retry on a later boundary.
            operation = "park-checkpoint" if args.ordinary_park else "blank-checkpoint"
            paused_reply = peer.send(f"{operation} 1 {path}")
            assert paused_reply.startswith("checkpoint ") or paused_reply == "rejected"
            assert peer.send("published") == paused
            if paused_reply != "rejected" or not args.ordinary_restore or monotonic() >= deadline:
                break
            assert peer.send("resume") == "resume"
            wait_for_picture(peer, int(paused) + 1)
        if args.ordinary_restore:
            assert renderer == "hardware", "The source did not use hardware rendering"
            assert peer.send("cpu") == "Recompiler"
            assert paused_reply.startswith("checkpoint "), "The held frame could not be exported"
            held_snapshot = snapshot.read_bytes()
            assert len(held_snapshot) == int(paused_reply.split()[2])
            assert marker not in held_snapshot and blank in held_snapshot
            if args.ordinary_park:
                assert peer.send("resume") == "resume"
                sleep(0.08)
                assert peer.send("published") == paused, "Normal playback released the match hold"
                assert peer.send(f"park-checkpoint 1 {path}") == "unavailable"
                assert peer.send(f"erase-card-tail {len(marker)}") == "erased"
            verify_ordinary_restore(args, peer, bios, path)
            assert peer.send("published") == paused, "The original session moved during the match probe"
            if args.ordinary_park:
                assert peer.send("pause") == "pause"
                assert peer.send("resume-checkpoint") == "resumed"
                assert peer.send("resume-checkpoint") == "unavailable"
                assert peer.send(f"cancel-park 1 {path}") == "cancelled"
                assert peer.send("resume-checkpoint") == "unavailable"
                print("Native recovery restored the held source after a scratch card mutation.")
        assert peer.send("resume") == "resume"
        wait_for_picture(peer, int(paused) + 1)
    assert card.read_bytes() == private

    if args.ordinary_control_core:
        with ExitStack() as active:
            peer = boot(args.ordinary_control_core, active)
            assert peer.send("local-support") == "no"
            assert peer.send(f"blank-checkpoint 1 {path}") == "unavailable"
    assert card.read_bytes() == private
    if args.ordinary_replace:
        private_core = root / args.core.name
        copyfile(args.core, private_core)
        command = [args.probe.resolve(), private_core, disc, bios, args.ordinary_control_core.resolve(),
                   "replace", saves]
        if args.presentation_core:
            command.append(args.presentation_core.resolve())
        if args.ordinary_continue:
            command.append("continue")
        result = run(command, timeout=60, capture_output=True, text=True,
                     creationflags=CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f"Native replacement exited {result.returncode}")
        print(result.stdout.strip())
        assert card.read_bytes() == private
    print(f"Ordinary {renderer} worker exported {size} bytes at frame {frame}, kept the source card, "
          "and continued publishing after cancellation and export.")


def verify_ordinary_restore(args, source, bios, path):
    expected = source.send("fingerprint")
    if args.rollback_recompiler:
        with ExitStack() as match:
            verify_native_rollback(args, match, checkpoint=(path, expected))
        assert source.send("fingerprint") == expected
        return
    with ExitStack() as match:
        peers = []
        for seat in range(2):
            peer = Peer(args.probe.resolve(), args.core.resolve(), args.disc.resolve(), bios, seat,
                        recompiler=True)
            match.callback(peer.close)
            assert peer.receive() == "ready"
            assert peer.send("cpu") == "Recompiler"
            assert peer.send("fingerprint") != expected
            assert peer.send(f"restore 1 {path} 0") == "restored"
            assert peer.send("fingerprint") == expected
            assert peer.send("start") == "started"
            peers.append(peer)
        verify_new_input_epoch(*peers, input_masks=(64, 256))
        assert all(peer.send("renderer") == "software" and int(peer.send("published")) > 0
                   for peer in peers)
    assert source.send("fingerprint") == expected
    print("Both match workers restored the original hardware session's RAM and ran a new input epoch.")


def verify_checkpoint(args, host, cleanup, during_transfer):
    checkpoint = capture_checkpoint(host, cleanup)
    during_transfer(checkpoint[0])
    guest = replay_checkpoint(args, host, cleanup, checkpoint)
    if args.cutover:
        verify_new_input_epoch(host, guest)


def capture_checkpoint(host, cleanup):
    # Allow the final batch of queued inputs to drain before capturing RAM.
    sleep(0.1)
    held_frame = host.sync()[0]
    checkpoint = capture_held_checkpoint(host, cleanup, held_frame)
    assert host.sync()[0] == held_frame
    return checkpoint


def capture_held_checkpoint(host, cleanup, held_frame):
    expected = host.send("fingerprint")
    directory = cleanup.enter_context(TemporaryDirectory(prefix="anyps5-worker-state-"))
    path = json.dumps((Path(directory) / "state").as_posix())
    assert host.send(f"checkpoint 1 {path}").startswith(f"checkpoint {held_frame} ")
    return held_frame, expected, path


def replay_checkpoint(args, host, cleanup, checkpoint, *, local=False):
    held_frame, expected, path = checkpoint
    guest = Peer(args.probe.resolve(), args.core.resolve(), args.checkpoint_guest.resolve(), args.bios.resolve(), 1)
    cleanup.callback(guest.close)
    assert guest.receive() == "ready"
    before = guest.send("fingerprint")
    assert before != expected, "The guest already had the expected state before import"
    assert guest.send(f"restore 2 {path} {held_frame}") == "rejected"
    assert guest.send("fingerprint") == before
    assert guest.send(f"restore 1 {path} {held_frame}") == "restored"
    assert guest.send("fingerprint") == expected
    assert guest.send(f"replay {held_frame + 1} 1 0 0") == "unavailable"
    published = guest.send("published")
    cursor = held_frame
    if local:
        end = hold_host(host)
    elif args.cutover:
        end = hold_host(host)
        host.sync(inputs=[(end, 65535)])
        sleep(0.08)
        assert host.sync()[0] == end, "A queued remote input advanced the held host"
    else:
        end = host.sync()[0]
    assert end - held_frame >= 180
    host_input_seen = False
    while cursor < end:
        count, *pads = host.send(f"history {cursor}").split()
        assert 0 < int(count) <= 120 and len(pads) == int(count) * 2
        if local:
            assert all(mask == "0" for mask in pads[1::2]), "Local play fed input to the guest port"
            host_input_seen |= "8" in pads[::2]
        assert guest.send(f"replay {cursor} {count} {' '.join(pads)}") == "replayed"
        cursor += int(count)
    if local:
        assert host_input_seen, "The local controller did not enter the recorded history"
    assert cursor == end
    assert guest.send("fingerprint") == host.send("fingerprint")
    assert guest.send("published") == published, "Catch-up published a guest video frame"
    assert guest.send(f"checkpoint 1 {path}") == "unavailable"
    assert guest.sync()[0] == 0, "The guest advanced before the match started"
    assert guest.send("start") == "started"
    assert guest.send(f"restore 1 {path} {held_frame}") == "unavailable"
    assert guest.send(f"replay {cursor} 1 0 0") == "unavailable"
    assert guest.send(f"checkpoint 1 {path}") == "unavailable"
    print(f"The held guest replayed {end - held_frame} host frames and matched the host RAM.")
    if args.cutover or local:
        assert host.send("start") == "started"
    return guest


def hold_host(host):
    status, frame = host.send("hold").split()
    assert status == "held"
    return int(frame)


def wait_for_picture(host, target):
    deadline = monotonic() + 15
    while int(host.send("published")) < target:
        if monotonic() > deadline:
            raise TimeoutError("The local host stopped publishing frames")
        sleep(0.01)


def verify_local_handoff(args, cleanup):
    host = Peer(args.probe.resolve(), args.core.resolve(), args.disc.resolve(), args.bios.resolve(), 0, local=True)
    cleanup.callback(host.close)
    assert host.receive() == "ready"
    assert host.send("start") == "error", "A playing host joined before reaching the cutover barrier"
    wait_for_picture(host, 240)
    first = hold_host(host)
    picture = int(host.send("published"))
    sleep(0.1)
    assert int(host.send("published")) == picture
    checkpoint = capture_held_checkpoint(host, cleanup, first)
    assert host.send("resume-local") == "resumed"
    assert host.send("pad 8") == "pad"
    wait_for_picture(host, picture + 240)
    guest = replay_checkpoint(args, host, cleanup, checkpoint, local=True)
    assert host.send("resume-local") == "unavailable"
    verify_new_input_epoch(host, guest)
    hold_host(host)
    picture = int(host.send("published"))
    assert host.send("resume-local") == "resumed"
    wait_for_picture(host, picture + 30)


def verify_new_input_epoch(host, guest, *, input_masks=None):
    peers = [host, guest]
    states = [peer.sync() for peer in peers]
    assert all(frame == 0 and received == -1 and not hashes
               and all(buttons == 0 for _, buttons in local[:2])
               for frame, received, local, hashes in states), "Stale input or fingerprints survived cutover"
    if input_masks:
        for peer, mask in zip(peers, input_masks):
            assert peer.send(f"pad {mask}") == "pad"
    seen = [False, False]
    fingerprints = [{}, {}]
    deadline = monotonic() + 15
    while min(state[0] for state in states) < 120:
        if monotonic() > deadline:
            raise TimeoutError("The new host and guest input epoch did not reach frame 120")
        states = [peer.sync(states[1 - seat][1], states[1 - seat][2])
                  for seat, peer in enumerate(peers)]
        for seat, state in enumerate(states):
            fingerprints[seat].update(state[3])
            if input_masks:
                seen[seat] |= any(buttons == input_masks[seat] for _, buttons in state[2])
        sleep(0.002)
    assert fingerprints[0][119] == fingerprints[1][119], "PS1 RAM diverged after cutover"
    assert not input_masks or all(seen), "Each seat must supply its own controller input"
    print("The held host and replacement guest matched the first RAM fingerprint in the new input epoch.")


def verify_native_rollback(args, cleanup, *, checkpoint=None):
    target = max(args.frames, 240)
    local_trace = None
    if args.presentation_core:
        directory = Path(cleanup.enter_context(TemporaryDirectory(prefix="anyps5-rollback-short-")))
        local_trace = directory / "local.txt"
        local_trace.write_text("0\n" * target, encoding="ascii")
    peers = []
    for prediction in (False, True):
        peer = Peer(args.probe.resolve(), args.core.resolve(), args.disc.resolve(), args.bios.resolve(),
                    0, rollback=prediction, audio_control=not prediction,
                    recompiler=args.rollback_recompiler,
                    input_trace=local_trace if prediction else None,
                    presentation_core=args.presentation_core.resolve() if prediction and args.presentation_core else None)
        cleanup.callback(peer.close)
        peers.append(peer)
    assert all(peer.receive() == "ready" for peer in peers)
    if args.rollback_recompiler:
        assert all(peer.send("cpu") == "Recompiler" for peer in peers)
    if checkpoint:
        path, expected = checkpoint
        for peer in peers:
            assert peer.send("fingerprint") != expected
            assert peer.send(f"restore 2 {path} 0") == "rejected"
        assert peers[1].send(f"restore 1 {path} 1") == "unavailable"
        for peer in peers:
            assert peer.send(f"restore 1 {path} 0") == "restored", "Match did not import the ordinary checkpoint"
            assert peer.send("fingerprint") == expected
        assert peers[1].send(f"restore 1 {path} 0") == "unavailable"
        print("Both match workers imported the ordinary hardware checkpoint at epoch zero.")
    assert all(peer.send("start") == "started" for peer in peers)
    states = [peer.sync() for peer in peers]
    sent = [set(), set()]
    fingerprints = [{}, {}]
    gap = 119
    gap_crossing = 12 if args.presentation_core else 3

    def pump(seat, hold_gap):
        frame, _, local, _ = states[seat]
        inputs = [(index, 256 if gap <= index < gap + 8 else 0)
                  for index in range(min(target, frame + 12))
                  if index not in sent[seat] and (index != gap or not hold_gap)]
        states[seat] = peers[seat].sync(local[-1][0], inputs)
        assert states[seat][0] >= frame, "Published progress moved backward during replay"
        sent[seat].update(index for index, _ in inputs)
        for index, fingerprint in states[seat][3].items():
            assert fingerprints[seat].setdefault(index, fingerprint) == fingerprint, (
                f"Worker {seat} changed a published fingerprint at frame {index}")

    deadline = monotonic() + 35
    while states[0][0] < gap or states[1][0] < gap + gap_crossing:
        if monotonic() > deadline:
            raise TimeoutError(f"Native worker did not predict through the gap: {states}")
        for seat in range(2):
            pump(seat, True)
        sleep(0.002)
    assert states[0][0] <= gap
    assert gap not in fingerprints[1], "A speculative fingerprint was published before correction"
    if args.presentation_core:
        deadline = monotonic() + 2
        while int(peers[1].send("audio-drained")) != gap - 1:
            assert monotonic() < deadline, "Confirmed audio did not reach the held frame"
            sleep(0.002)
    before = [peer.send("audio").split() for peer in peers]
    assert before[0] == before[1] and int(before[0][0]) > 0, "Audio escaped before correction"
    deadline = monotonic() + target / 25 + 15
    last_fingerprint = target // 120 * 120 - 1
    while min(state[0] for state in states) < target or any(last_fingerprint not in hashes for hashes in fingerprints):
        if monotonic() > deadline:
            raise TimeoutError(f"Native workers did not complete confirmed replay: {states}")
        for seat in range(2):
            pump(seat, False)
        sleep(0.002)
    for frame in range(gap, target, 120):
        assert fingerprints[0][frame] == fingerprints[1][frame], f"Corrected RAM diverged at frame {frame}"
    while int(peers[1].send("audio-drained")) != target - 1:
        assert monotonic() < deadline, "Prediction worker did not drain audio through the last confirmed frame"
        sleep(0.002)
    after = [peer.send("audio").split() for peer in peers]
    while after[0] != after[1] and monotonic() < deadline:
        sleep(0.002)
        after = [peer.send("audio").split() for peer in peers]
    assert after[0] == after[1] and int(after[0][0]) > int(before[0][0]), (
        f"Corrected audio did not match the confirmed-input control: {after}")
    if args.presentation_core:
        steps, video, depth = map(int, peers[1].send("presentation-steps").split())
        assert steps > 0 and video > 0, "The spare core never published video during replay"
        assert gap_crossing <= depth <= 16, f"Unexpected peak prediction depth: {depth}"
        assert steps < 60, f"The primary did not catch the spare after the single correction: {steps} steps"
        underruns, queue_peak = map(int, peers[1].send("audio-pacing").split())
        assert 1 <= queue_peak <= 64, "Confirmed audio did not use the bounded playout queue"
        print(f"Confirmed audio playout recovered from {underruns} underruns; peak queue {queue_peak} frames.")
        if target % 120 == 0:
            assert peers[1].send("fingerprint") == fingerprints[1][target - 1], (
                "The probe read RAM from the wrong core")
        print(f"The spare core published {video} texture updates across {steps} presentation steps.")
    print(f"Native rollback corrected frame {gap}, drove {target} frames and matched "
          f"{target // 120} confirmed RAM fingerprints and {after[0][0]} stereo sample pairs.")


def read_trace(trace):
    # Reuse the original harness' trace validator and game-owned finish gate.
    sys.path.insert(0, os.environ["ANYPS5_PS1_NETPLAY_TOOLS"])
    from determinism import normalize_trace

    spans = normalize_trace(json.loads(trace.read_text()))
    return [pads for count, *pads in spans for _ in range(count)]


def traced_rollback_race(args, cleanup):
    frames = read_trace(args.trace)
    from rollback import PacketLink, PROFILES, TICK_MS
    if args.sample_frames is not None:
        if args.sample_frames >= len(frames):
            raise ValueError("--sample-frames must be shorter than the complete race trace")
        frames = frames[:args.sample_frames]

    assert frames[:2] == [[0, 0]] * 2, "The native delay starts with neutral input"
    directory = Path(cleanup.enter_context(TemporaryDirectory(prefix="anyps5-rollback-input-")))
    peers = []
    for seat in range(2):
        trace = directory / f"seat-{seat}.txt"
        trace.write_text("\n".join(str(pads[seat]) for pads in frames), encoding="ascii")
        linux = seat == 1 and args.linux_probe is not None
        probe = args.linux_probe if linux else args.probe
        core = args.linux_core if linux else args.core
        peer = Peer(probe.resolve(), core.resolve(), args.disc.resolve(), args.bios.resolve(),
                    seat, rollback=True, input_trace=trace, recompiler=args.rollback_recompiler,
                    presentation_core=args.presentation_core.resolve() if args.presentation_core else None,
                    linux=linux, step_breakdown=args.step_breakdown and not linux)
        cleanup.callback(peer.close)
        peers.append(peer)
    assert all(peer.receive() == "ready" for peer in peers)
    if args.rollback_recompiler:
        assert all(peer.send("cpu") == "Recompiler" for peer in peers)
    assert all(peer.send("start") == "started" for peer in peers)
    states = [peer.sync() for peer in peers]
    histories = [{}, {}]
    links = [PacketLink(PROFILES[args.network_profile], 2401 + seat) for seat in range(2)]
    target = len(frames)
    next_report = 1800
    epoch = perf_counter()
    for tick in range(target * 2 + 300):
        delay = epoch + tick * TICK_MS / 1000 - perf_counter()
        if delay > 0:
            sleep(delay)
        for seat, (frame, received, local, fingerprints) in enumerate(states):
            for index, mask in local:
                assert mask == (frames[index][seat] if index < target else 0), (
                    f"Seat {seat + 1} supplied the wrong input at frame {index}")
            for index, fingerprint in fingerprints.items():
                assert histories[seat].setdefault(index, fingerprint) == fingerprint, (
                    f"Seat {seat + 1} changed a published fingerprint at frame {index}")
                other = histories[1 - seat].get(index)
                assert other is None or other == fingerprint, f"PS1 RAM diverged at frame {index}"
            links[1 - seat].send(tick, received, [(index, mask) for index, mask in local if index < target])
        for seat, peer in enumerate(peers):
            acknowledged = -1
            arrived = {}
            for ack, samples in links[seat].receive(tick):
                acknowledged = max(acknowledged, ack)
                for index, mask in samples:
                    assert arrived.setdefault(index, mask) == mask
            states[seat] = peer.sync(acknowledged, sorted(arrived.items()))
        progress = min(state[0] for state in states)
        if progress >= next_report:
            print(f"Native rollback reached {progress}/{target} frames after {tick + 1} paced ticks", flush=True)
            next_report += 1800
        if progress == target and all(int(peer.send("confirmed")) == target - 1 for peer in peers):
            for seat, state in enumerate(states):
                histories[seat].update(state[3])
            break
    else:
        raise TimeoutError(f"Both native peers did not finish: {[state[:2] for state in states]}")

    expected = set(range(119, target, 120))
    assert set(histories[0]) == expected and histories[0] == histories[1], "Confirmed RAM differs or is incomplete"
    outcome = race_placements(peers) if args.sample_frames is None else "diagnostic prefix"
    deadline = monotonic() + 2
    while not all(int(peer.send("audio-drained")) == target - 1 for peer in peers):
        assert monotonic() < deadline, "Native workers did not drain audio through the last confirmed frame"
        sleep(0.002)
    pcm = [peer.send("audio").split() for peer in peers]
    assert pcm[0] == pcm[1] and int(pcm[0][0]) > 0, f"Confirmed PCM differs or is empty: {pcm}"
    print(f"Finished {target} frames in {perf_counter() - epoch:.2f}s: {outcome}; "
          f"{len(expected)} matching RAM fingerprints and {pcm[0][0]} stereo pairs.")
    for seat, (peer, link) in enumerate(zip(peers, links), 1):
        if seat == 2 and args.linux_probe:
            corrections = int(peer.send("corrections"))
            if args.network_profile != "clean" and args.sample_frames is None:
                assert corrections > 0, "The Linux worker never restored a rollback checkpoint"
            print(f"Seat {seat} (Linux): {link.dropped}/{link.sent} packets dropped, "
                  f"{link.reordered} reordered; {corrections} rollback corrections.")
            continue
        published, video_gap, long_gaps, audio_gap, work, overruns = map(int, peer.send("presentation").split())
        print(f"Seat {seat}: {link.dropped}/{link.sent} packets dropped, {link.reordered} reordered; "
              f"{published} video updates, max gap {video_gap / 1000:.3f} ms, "
              f"{long_gaps} gaps over two ticks; max confirmed PCM gap {audio_gap / 1000:.3f} ms; "
              f"max native work {work / 1000:.3f} ms, {overruns} iterations over 16.667 ms.")
        if args.work_breakdown:
            stages = ("audio", "restore", "save", "step", "fingerprint", "other")
            frame, total, *samples = map(int, peer.send("work-breakdown").split())
            assert len(samples) == 2 * len(stages), "Malformed native work timing"
            worst = samples[:len(stages)]
            dominant = samples[len(stages):]
            assert 0 <= total - sum(worst) <= len(stages), "Inconsistent native work timing"
            print(f"Seat {seat} worst work at frame {frame} ({total / 1000:.3f} ms): "
                  + ", ".join(f"{stage} {value / 1000:.3f} ms" for stage, value in zip(stages, worst)))
            print(f"Seat {seat} dominant overrun stages: "
                  + ", ".join(f"{stage} {count}" for stage, count in zip(stages, dominant)))
        if args.step_breakdown:
            frame, step_us, video_us, cycles, total_cycles, samples = map(int, peer.send("step-breakdown").split())
            assert 0 <= video_us <= step_us and -1 <= cycles <= total_cycles
            cycle_summary = f"{cycles} thread cycles" if cycles >= 0 else "thread cycles unavailable"
            if samples:
                cycle_summary += f" (mean {total_cycles // samples} across {samples} steps)"
            print(f"Seat {seat} slowest step at frame {frame}: {step_us / 1000:.3f} ms elapsed, "
                  f"{video_us / 1000:.3f} ms in video, {cycle_summary}.")
        waits, wait_us, late_wakes, peak_wake_us = map(int, peer.send("pacing").split())
        print(f"Seat {seat}: {waits} blocked polls totaling {wait_us / 1000:.3f} ms; "
              f"{late_wakes} deadline wakes over 2 ms, peak {peak_wake_us / 1000:.3f} ms late.")
        if args.presentation_core:
            steps, video, depth = map(int, peer.send("presentation-steps").split())
            if args.network_profile != "clean" and args.sample_frames is None:
                assert steps > 0 and video > 0, "The spare core never published video during the race"
            print(f"Seat {seat}: {video} speculative texture updates across {steps} presentation steps; "
                  f"peak primary prediction depth {depth} frames.")
            underruns, queue_peak = map(int, peer.send("audio-pacing").split())
            print(f"Seat {seat}: {underruns} recovered audio underruns; peak queue {queue_peak} frames.")


def traced_race(peers, frames, args, cleanup):
    assert frames[:3] == [[0, 0]] * 3, "The native delay starts with neutral input"
    cutover = args.cutover_frame
    if cutover is not None:
        assert 180 < cutover <= len(frames) - 120 and frames[cutover:cutover + 3] == [[0, 0]] * 3, (
            "The cutover must have 180 preceding frames and three neutral frames for the new input buffer")
    states = [peer.sync() for peer in peers]
    histories = [{}, {}]
    pads = [0, 0]
    joining = cleanup.enter_context(ExitStack()) if args.checkpoint_guest else None
    checkpoint = None
    epoch_start = 0
    checkpoint_frame = cutover - 180 if cutover is not None else 6600
    replay_frame = cutover if cutover is not None else 6780
    for frame in range(len(frames)):
        epoch_frame = frame - epoch_start
        # Frame f+2 is captured when the worker waits for frame f. Exchange
        # only frame f after setting the pad it must capture at frame f+1.
        upcoming = frames[frame + 3] if frame + 3 < len(frames) else [0, 0]
        for seat, mask in enumerate(upcoming):
            if pads[seat] != mask:
                assert peers[seat].send(f"pad {mask}") == "pad"
                pads[seat] = mask
        outputs = [dict(state[2])[epoch_frame] for state in states]
        assert outputs == frames[frame], f"Script input mismatch at frame {frame}"
        for seat, peer in enumerate(peers):
            states[seat] = peer.sync(epoch_frame - 1, [(epoch_frame, outputs[1 - seat])])
        deadline = monotonic() + 3
        for seat, peer in enumerate(peers):
            state = states[seat]
            while state[0] <= epoch_frame or epoch_frame + 3 not in dict(state[2]):
                if monotonic() > deadline:
                    raise TimeoutError(f"Seat {seat + 1} stopped advancing at frame {frame}")
                sleep(0.001)
                state = peer.sync(epoch_frame - 1)
            states[seat] = state
            histories[seat].update((epoch_start + cursor, digest) for cursor, digest in state[3].items())
        if (epoch_frame + 1) % 120 == 0:
            assert histories[0][frame] == histories[1][frame], f"PS1 RAM diverged at frame {frame}"
        if joining and frame + 1 == checkpoint_frame:
            checkpoint = capture_checkpoint(peers[0], joining)
        if joining and frame + 1 == replay_frame:
            assert checkpoint is not None
            if cutover is not None:
                peers[1].close()
                peers[1] = replay_checkpoint(args, peers[0], joining, checkpoint)
                epoch_start = cutover
                states = [peer.sync() for peer in peers]
                assert all(state[:2] == (0, -1) for state in states)
            else:
                replay_checkpoint(args, peers[0], joining, checkpoint)
                joining.close()
        if (frame + 1) % 1800 == 0:
            print(f"Verified {frame + 1}/{len(frames)} native frames", flush=True)
    placements = race_placements(peers)
    print(f"Both native peers finished after {len(frames)} frames with {len(histories[0])} matching "
          f"RAM fingerprints: {placements}")


def race_placements(peers):
    from determinism import ctr_finish

    placements = [ctr_finish(lambda address, size: bytes.fromhex(peer.send(f"read {address} {size}")))
                  for peer in peers]
    assert placements[0] == placements[1]
    return placements[0]


if __name__ == "__main__":
    main()
