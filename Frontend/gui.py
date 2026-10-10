import argparse
import collections
import getpass
import itertools
import queue
import shutil
import subprocess
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

MAX_TEXT_BYTES = 2047
MAX_NAME_BYTES = 49
ONLINE_WINDOW_S = 10
COLOR_ONLINE = "#2e7d32"
COLOR_OFFLINE = "#9ca3af"

TICKS = {"QUEUED": "…", "SENT": "✓", "DELIVERED": "✓✓", "FAILED": "✗ failed"}

def esc(s: str) -> str:
    return (s.replace("\\", "\\\\").replace("\n", "\\n")
             .replace("\t", "\\t").replace("\r", "\\r"))

def unesc(s: str) -> str:
    out, i = [], 0
    table = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\"}
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append(table.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)

def find_backend(explicit):
    if explicit:
        return Path(explicit)
    here = Path(__file__).resolve().parent
    for cand in (here.parent / "Backend" / "offchat", here / "offchat"):
        if cand.is_file():
            return cand
    found = shutil.which("offchat")
    return Path(found) if found else here.parent / "Backend" / "offchat"

class Backend:
    def __init__(self, exe, name, port, events):
        self.events = events
        self.stderr_tail = collections.deque(maxlen=8)
        self.proc = subprocess.Popen(
            [str(exe), name, str(port)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8",
            errors="replace", bufsize=1)
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line:
                self.events.put(("evt", self, [unesc(f) for f in line.split("\t")]))
        self.events.put(("exit", self, self.proc.wait()))

    def _read_stderr(self):
        for line in self.proc.stderr:
            self.stderr_tail.append(line.rstrip())

    def _write(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
            return True
        except (BrokenPipeError, ValueError, OSError):
            return False

    def send(self, tag, peer, text):
        return self._write(
            f"SEND\t{tag}\t{peer.ip}\t{peer.port}\t{esc(peer.name)}\t{esc(text)}")

    def stop(self):
        self._write("QUIT")
        try:
            self.proc.wait(timeout=1.5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

class Peer:
    def __init__(self, name, ip, port):
        self.name, self.ip, self.port = name, ip, port
        self.last_seen = 0.0
        self.unread = 0
        self.history = []

    @property
    def key(self):
        return (self.ip, self.port)

    @property
    def online(self):
        return time.time() - self.last_seen < ONLINE_WINDOW_S

    def label(self):
        dot = "●" if self.online else "○"
        badge = f"  ({self.unread})" if self.unread else ""
        return f"{dot} {self.name}{badge}"

class AddPeerDialog(tk.Toplevel):
    def __init__(self, parent):
        super().__init__(parent)
        self.title("Add peer")
        self.resizable(False, False)
        self.transient(parent)
        self.result = None

        frm = ttk.Frame(self, padding=14)
        frm.grid()
        self.vars = {k: tk.StringVar() for k in ("name", "ip", "port")}
        self.vars["port"].set("5000")
        for row, (key, label) in enumerate(
                (("name", "Name"), ("ip", "IP address"), ("port", "Port"))):
            ttk.Label(frm, text=label).grid(row=row, column=0, sticky="w", pady=4)
            ent = ttk.Entry(frm, textvariable=self.vars[key], width=24)
            ent.grid(row=row, column=1, padx=(10, 0), pady=4)
            if row == 0:
                ent.focus_set()

        btns = ttk.Frame(frm)
        btns.grid(row=3, column=0, columnspan=2, sticky="e", pady=(10, 0))
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="right")
        ttk.Button(btns, text="Add", command=self._ok).pack(side="right", padx=6)
        self.bind("<Return>", lambda e: self._ok())
        self.bind("<Escape>", lambda e: self.destroy())
        self.grab_set()
        self.wait_window()

    def _ok(self):
        name = self.vars["name"].get().strip()
        ip = self.vars["ip"].get().strip()
        try:
            port = int(self.vars["port"].get())
            assert 1 <= port <= 65535
        except (ValueError, AssertionError):
            messagebox.showerror("Add peer", "Port must be 1-65535.", parent=self)
            return
        if not ip:
            messagebox.showerror("Add peer", "IP address is required.", parent=self)
            return
        self.result = (name or ip, ip, port)
        self.destroy()

class App(tk.Tk):
    def __init__(self, args):
        super().__init__()
        self.title("OffChat")
        self.geometry("860x580")
        self.minsize(640, 420)

        self.exe = find_backend(args.backend)
        self.events = queue.Queue()
        self.backend = None
        self.peers = {}
        self.order = []
        self.by_tag = {}
        self.tags = itertools.count(1)
        self.listbox_cache = None
        self.current = None
        self._bubbles = []
        self._chat_w = 0
        self._resize_job = None

        style = ttk.Style(self)
        if "clam" in style.theme_names():
            style.theme_use("clam")

        self._build_topbar(args)
        self._build_body()
        self._set_running(False)

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(80, self._pump)
        self.after(1000, self._tick)
        if args.name:
            self.after(100, self._start)

    def _build_topbar(self, args):
        bar = ttk.Frame(self, padding=(10, 8))
        bar.pack(fill="x")
        self.name_var = tk.StringVar(value=args.name or _default_name())
        self.port_var = tk.StringVar(value=str(args.port))

        ttk.Label(bar, text="Name").pack(side="left")
        self.name_entry = ttk.Entry(bar, textvariable=self.name_var, width=16)
        self.name_entry.pack(side="left", padx=(6, 14))
        ttk.Label(bar, text="Port").pack(side="left")
        self.port_entry = ttk.Entry(bar, textvariable=self.port_var, width=7)
        self.port_entry.pack(side="left", padx=(6, 14))
        self.start_btn = ttk.Button(bar, text="Start", command=self._toggle)
        self.start_btn.pack(side="left")
        self.status_var = tk.StringVar(value="Not running")
        ttk.Label(bar, textvariable=self.status_var,
                  foreground="#555").pack(side="left", padx=14)
        ttk.Separator(self).pack(fill="x")

    def _build_body(self):
        paned = ttk.PanedWindow(self, orient="horizontal")
        paned.pack(fill="both", expand=True)

        left = ttk.Frame(paned, padding=(8, 8, 4, 8))
        paned.add(left, weight=1)
        ttk.Label(left, text="Peers", font=("TkDefaultFont", 10, "bold")).pack(anchor="w")
        lb_frame = ttk.Frame(left)
        lb_frame.pack(fill="both", expand=True, pady=6)
        self.listbox = tk.Text(
            lb_frame, wrap="none", state="disabled", cursor="arrow",
            borderwidth=1, relief="solid", highlightthickness=0,
            padx=6, pady=4, spacing1=3, spacing3=3, width=22,
            font=("TkDefaultFont", 10))
        self.listbox.tag_configure("dot_on", foreground=COLOR_ONLINE)
        self.listbox.tag_configure("dot_off", foreground=COLOR_OFFLINE)
        self.listbox.tag_configure("name_off", foreground=COLOR_OFFLINE)
        self.listbox.tag_configure("badge", foreground="#c62828")
        self.listbox.tag_configure("row_sel", background="#cfe3ff")
        sb = ttk.Scrollbar(lb_frame, command=self.listbox.yview)
        self.listbox.config(yscrollcommand=sb.set)
        self.listbox.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.listbox.bind("<Button-1>", self._on_click)
        self.add_btn = ttk.Button(left, text="Add peer…", command=self._add_peer)
        self.add_btn.pack(fill="x")

        right = ttk.Frame(paned, padding=(4, 8, 8, 8))
        paned.add(right, weight=3)
        self.header_var = tk.StringVar(value="Select a peer to start chatting")
        ttk.Label(right, textvariable=self.header_var,
                  font=("TkDefaultFont", 11, "bold")).pack(anchor="w")

        chat_frame = ttk.Frame(right)
        chat_frame.pack(fill="both", expand=True, pady=6)
        self.chat = tk.Text(
            chat_frame, wrap="word", state="disabled", borderwidth=1,
            relief="solid", highlightthickness=0, padx=8, pady=8,
            background="#f7f7f8", font=("TkDefaultFont", 10), cursor="arrow")
        csb = ttk.Scrollbar(chat_frame, command=self.chat.yview)
        self.chat.config(yscrollcommand=csb.set)
        self.chat.pack(side="left", fill="both", expand=True)
        csb.pack(side="right", fill="y")
        self._configure_tags()
        self.chat.bind("<Configure>", self._on_chat_resize)

        send_row = ttk.Frame(right)
        send_row.pack(fill="x")
        self.input = tk.Text(send_row, height=3, wrap="word", borderwidth=1,
                             relief="solid", highlightthickness=0,
                             padx=6, pady=4, font=("TkDefaultFont", 10))
        self.input.pack(side="left", fill="x", expand=True)
        self.input.bind("<Return>", self._on_enter)
        self.send_btn = ttk.Button(send_row, text="Send", command=self._send)
        self.send_btn.pack(side="left", padx=(8, 0), fill="y")
        self.hint_var = tk.StringVar(value="Enter to send · Shift+Enter for a new line")
        ttk.Label(right, textvariable=self.hint_var, foreground="#888",
                  font=("TkDefaultFont", 8)).pack(anchor="w", pady=(4, 0))

    def _configure_tags(self):
        c = self.chat
        c.tag_configure("meta_me", justify="right", foreground="#6b7280",
                        font=("TkDefaultFont", 8), rmargin=8, spacing1=8)
        c.tag_configure("meta_them", justify="left", foreground="#6b7280",
                        font=("TkDefaultFont", 8), lmargin1=8, spacing1=8)
        c.tag_configure("line_me", justify="right", rmargin=8, spacing3=2)
        c.tag_configure("line_them", justify="left", lmargin1=8, lmargin2=8, spacing3=2)
        c.tag_configure("failed", foreground="#c62828")
        c.tag_configure("delivered", foreground="#2e7d32")
        c.tag_configure("empty", foreground="#999", justify="center", spacing1=60)

    def _toggle(self):
        if self.backend:
            self._stop()
        else:
            self._start()

    def _start(self):
        name = self.name_var.get().strip()
        if not name or len(name.encode()) > MAX_NAME_BYTES or any(
                ch in name for ch in "|\t\n\r"):
            messagebox.showerror(
                "OffChat", f"Name must be 1-{MAX_NAME_BYTES} bytes and can't "
                          "contain '|' or control characters.")
            return
        try:
            port = int(self.port_var.get())
            assert 1 <= port <= 65535
        except (ValueError, AssertionError):
            messagebox.showerror("OffChat", "Port must be a number from 1 to 65535.")
            return
        if not self.exe.is_file():
            messagebox.showerror(
                "OffChat", f"Backend not found:\n{self.exe}\n\n"
                          "Build it first:\n    cd Backend && make")
            return
        try:
            self.backend = Backend(self.exe, name, port, self.events)
        except OSError as e:
            messagebox.showerror("OffChat", f"Could not start backend:\n{e}")
            return
        self.status_var.set("Starting…")
        self._set_running(True)

    def _stop(self):
        if self.backend:
            self.backend.stop()
        self.backend = None
        for p in self.peers.values():
            p.last_seen = 0.0
        self.status_var.set("Not running")
        self._set_running(False)
        self._refresh_peers(force=True)

    def _set_running(self, running):
        self.start_btn.config(text="Stop" if running else "Start")
        for w in (self.name_entry, self.port_entry):
            w.config(state="disabled" if running else "normal")
        self._update_input_state()

    def _update_input_state(self):
        ok = bool(self.backend and self.current)
        self.input.config(state="normal" if ok else "disabled")
        self.send_btn.config(state="normal" if ok else "disabled")

    def _on_close(self):
        if self.backend:
            self.backend.stop()
        self.destroy()

    def _pump(self):
        try:
            while True:
                kind, source, payload = self.events.get_nowait()
                if source is not self.backend:
                    continue
                if kind == "evt":
                    self._handle_event(payload)
                elif kind == "exit":
                    self._backend_exited(payload)
        except queue.Empty:
            pass
        self.after(80, self._pump)

    def _handle_event(self, f):
        ev, args = f[0], f[1:]
        if ev == "READY" and len(args) >= 2:
            self.status_var.set(f"Online as {args[0]} on port {args[1]}")
        elif ev == "PEER" and len(args) >= 3:
            self._on_peer(args[0], args[1], args[2])
        elif ev == "MSG" and len(args) >= 5:
            self._on_message(*args[:5])
        elif ev == "STATUS" and len(args) >= 4:
            self._on_status(args[0], args[2])
        elif ev == "WARN" and args:
            self.hint_var.set(f"⚠ {args[0]}")
        elif ev == "ERROR" and args:
            messagebox.showerror("OffChat", args[0])

    def _backend_exited(self, code):
        tail = "\n".join(self.backend.stderr_tail)
        self.backend = None
        self.status_var.set("Backend stopped")
        self._set_running(False)
        if code != 0:
            messagebox.showerror(
                "OffChat", f"Backend exited with code {code}."
                           + (f"\n\n{tail}" if tail else ""))

    def _upsert_peer(self, name, ip, port):
        key = (ip, port)
        peer = self.peers.get(key)
        if peer is None:
            peer = self.peers[key] = Peer(name, ip, port)
            self._refresh_peers(force=True)
        elif name and peer.name != name and peer.last_seen == 0.0:
            peer.name = name
        return peer

    def _same_peer(self, name, port):
        hits = [p for p in self.peers.values()
                if p.name == name and p.port == port]
        return hits[0] if len(hits) == 1 else None

    def _on_peer(self, name, ip, port):
        try:
            port = int(port)
        except ValueError:
            return
        peer = self._upsert_peer(name, ip, port)
        if peer.name != name:
            peer.name = name
        peer.last_seen = time.time()

    def _on_message(self, mid, name, ip, port, text):
        try:
            port = int(port)
        except ValueError:
            return
        peer = self.peers.get((ip, port)) or self._same_peer(name, port)
        if peer is None:
            peer = self._upsert_peer(name, ip, port)
        peer.history.append({"dir": "in", "text": text, "time": time.time(),
                             "status": None, "tag": None})
        if self.current == peer.key:
            self._render()
        else:
            peer.unread += 1
            self.bell()
        self._refresh_peers(force=True)

    def _on_status(self, tag, state):
        msg = self.by_tag.get(tag)
        if msg is None:
            return
        if msg["status"] in ("DELIVERED", "FAILED") and state == "SENT":
            return
        msg["status"] = state
        if msg.get("peer_key") == self.current:
            self._render()

    def _tick(self):
        self._refresh_peers()
        self.after(1000, self._tick)

    def _refresh_peers(self, force=False):
        keys = sorted(self.peers, key=lambda k: (not self.peers[k].online,
                                                 self.peers[k].name.lower(), k))
        labels = [self.peers[k].label() for k in keys]
        if not force and (keys, labels) == self.listbox_cache:
            return
        self.listbox_cache = (keys, labels)
        self.order = keys
        lb = self.listbox
        lb.config(state="normal")
        lb.delete("1.0", "end")
        for k in keys:
            p = self.peers[k]
            lb.insert("end", "●" if p.online else "○", "dot_on" if p.online else "dot_off")
            lb.insert("end", f" {p.name}", () if p.online else ("name_off",))
            if p.unread:
                lb.insert("end", f"  ({p.unread})", "badge")
            lb.insert("end", "\n")
        if self.current in keys:
            idx = keys.index(self.current)
            lb.tag_add("row_sel", f"{idx + 1}.0", f"{idx + 1}.0 lineend +1c")
            lb.see(f"{idx + 1}.0")
        lb.config(state="disabled")

    def _on_click(self, event):
        idx = int(self.listbox.index(f"@{event.x},{event.y}").split(".")[0]) - 1
        if 0 <= idx < len(self.order):
            self._select_index(idx)
        return "break"

    def _select_index(self, idx):
        self.current = self.order[idx]
        peer = self.peers[self.current]
        peer.unread = 0
        self.header_var.set(f"{peer.name}   ·   {peer.ip}:{peer.port}")
        self._update_input_state()
        self._refresh_peers(force=True)
        self._render()
        self.input.focus_set()

    def _add_peer(self):
        dlg = AddPeerDialog(self)
        if dlg.result:
            name, ip, port = dlg.result
            peer = self._upsert_peer(name, ip, port)
            peer.name = name
            self.current = peer.key
            self._refresh_peers(force=True)
            self.header_var.set(f"{peer.name}   ·   {peer.ip}:{peer.port}")
            self._update_input_state()
            self._render()

    def _on_chat_resize(self, event):
        if abs(event.width - self._chat_w) < 40:
            return
        self._chat_w = event.width
        if self._resize_job:
            self.after_cancel(self._resize_job)
        self._resize_job = self.after(150, self._rerender)

    def _rerender(self):
        self._resize_job = None
        self._render()

    def _bubble(self, text, side):
        c = self.chat
        mine = side == "me"
        lbl = tk.Label(
            c, text=text, bg="#d9f2d0" if mine else "#e6e8ec", fg="#000",
            justify="left", anchor="w", padx=10, pady=5, borderwidth=0,
            wraplength=max(160, int(c.winfo_width() * 0.6)),
            font=("TkDefaultFont", 10))
        lbl.bind("<MouseWheel>",
                 lambda e: c.yview_scroll(-1 if e.delta > 0 else 1, "units"))
        lbl.bind("<Button-4>", lambda e: c.yview_scroll(-1, "units"))
        lbl.bind("<Button-5>", lambda e: c.yview_scroll(1, "units"))
        self._bubbles.append(lbl)
        start = c.index("end-1c")
        c.window_create("end", window=lbl)
        c.insert("end", "\n")
        c.tag_add("line_me" if mine else "line_them", start, "end-1c")

    def _render(self):
        c = self.chat
        c.config(state="normal")
        c.delete("1.0", "end")
        for lbl in self._bubbles:
            lbl.destroy()
        self._bubbles = []
        peer = self.peers.get(self.current)
        if peer is None or not peer.history:
            c.insert("end", "No messages yet.", "empty")
        else:
            for m in peer.history:
                stamp = time.strftime("%H:%M", time.localtime(m["time"]))
                if m["dir"] == "out":
                    st = m["status"] or "QUEUED"
                    tick_tag = {"FAILED": "failed", "DELIVERED": "delivered"}.get(st)
                    c.insert("end", f"You  {stamp}  ", "meta_me")
                    c.insert("end", TICKS[st], ("meta_me", tick_tag) if tick_tag
                             else ("meta_me",))
                    c.insert("end", "\n", "meta_me")
                    self._bubble(m["text"], "me")
                else:
                    c.insert("end", f"{peer.name}  {stamp}\n", "meta_them")
                    self._bubble(m["text"], "them")
        c.config(state="disabled")
        c.see("end")

    def _on_enter(self, event):
        if event.state & 0x1:
            return None
        self._send()
        return "break"

    def _send(self):
        if not (self.backend and self.current):
            return
        text = self.input.get("1.0", "end").rstrip("\n")
        if not text.strip():
            return
        if len(text.encode("utf-8")) > MAX_TEXT_BYTES:
            self.hint_var.set(f"⚠ Message too long (max {MAX_TEXT_BYTES} bytes)")
            return
        peer = self.peers[self.current]
        tag = str(next(self.tags))
        msg = {"dir": "out", "text": text, "time": time.time(),
               "status": "QUEUED", "tag": tag, "peer_key": peer.key}
        peer.history.append(msg)
        self.by_tag[tag] = msg
        if not self.backend.send(tag, peer, text):
            msg["status"] = "FAILED"
        self.input.delete("1.0", "end")
        self.hint_var.set("Enter to send · Shift+Enter for a new line")
        self._render()

def _default_name():
    try:
        return getpass.getuser()[:MAX_NAME_BYTES]
    except Exception:
        return "me"

def main():
    ap = argparse.ArgumentParser(description="OffChat GUI")
    ap.add_argument("--name", help="display name (starts immediately if given)")
    ap.add_argument("--port", type=int, default=5000, help="TCP port to listen on")
    ap.add_argument("--backend", help="path to the compiled offchat binary")
    App(ap.parse_args()).mainloop()

if __name__ == "__main__":
    main()