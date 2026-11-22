"""
Teams proof, part 3: actions as emoji reactions, with a legend post that the owner pins.
- Posts the legend post (a new thread root): what each emoji does on which message, and a small fleet board.
- Edits the proof card (thread 1's root) so its last line is the card's own legend.
- Posts a question reply in thread 1, answered by a number emoji.
- Probes which emoji Graph accepts as reactions (set, then unset), with two invalid controls.
- Reads the probe message's reactions and messageHistory (reactionAdded / reactionRemoved with user and time).
Inputs: the environment of teams_proof_run.py, plus TP_ROOT1 and TP_ROOT2 (the two proof threads).
Writes report3.json. Prints no token.
"""
import json
import os
import time
import urllib.error
import urllib.request

GRAPH = "https://graph.microsoft.com/v1.0"
TOKENS = {"az": os.environ["TP_SEND"], "dm": os.environ["TP_READ"]}
TEAM, CH, OUT = os.environ["TP_TEAM"], os.environ["TP_CH"], os.environ["TP_OUT"]
R1, R2 = os.environ["TP_ROOT1"], os.environ["TP_ROOT2"]
CHAN = f"{GRAPH}/teams/{TEAM}/channels/{CH}"
MARK = '\U0001F916 <strong>Claude</strong> · <span style="color:#0a7">[teams-proof]</span>'
rep = {"started": time.strftime("%Y-%m-%d %H:%M:%S")}
_last = [0.0]


def call(method, url, tok, body=None):
    """One Graph call, at most one per 0.6 s (the per-user limit is 1 per second); a 429 waits its Retry-After."""
    for attempt in range(4):
        wait = 0.6 - (time.time() - _last[0])
        if wait > 0:
            time.sleep(wait)
        _last[0] = time.time()
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + TOKENS[tok])
        if data is not None:
            req.add_header("Content-Type", "application/json")
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                txt = r.read().decode("utf-8", "replace")
                return r.status, (json.loads(txt) if txt.strip() else None), int((time.time() - t0) * 1000), ""
        except urllib.error.HTTPError as e:
            txt = e.read().decode("utf-8", "replace")
            if e.code == 429 and attempt < 3:
                time.sleep(float(e.headers.get("Retry-After") or 2))
                continue
            try:
                err = json.loads(txt).get("error", {})
                msg = f"{err.get('code', '')}: {err.get('message', '')}"
            except Exception:
                msg = txt
            return e.code, None, int((time.time() - t0) * 1000), msg[:300]
    return 429, None, 0, "throttled 4 times"


def html(c):
    return {"body": {"contentType": "html", "content": c}}


links = json.load(open(os.path.join(OUT, "report.json"), encoding="utf-8")).get("deep_links", {})

# 1. The legend post: a new root whose subject marks it; the owner pins it by hand (Graph cannot pin in a channel).
legend = (MARK + "<br><b>\U0001F4CC Agentmaster — legend</b> (react on the right message; I remove your emoji when I act)"
          "<table><tr><th>On</th><th>Emoji</th><th>Action</th></tr>"
          "<tr><td>a session card</td><td>\U0001F5BC️</td><td>send the screen now</td></tr>"
          "<tr><td></td><td>\U0001F4CA</td><td>status (model, context, usage)</td></tr>"
          "<tr><td></td><td>\U0001F6D1</td><td>Esc: stop the running turn</td></tr>"
          "<tr><td></td><td>↩️</td><td>Enter: send the prompt waiting in the box</td></tr>"
          "<tr><td></td><td>\U0001F4DC</td><td>the whole conversation as pictures</td></tr>"
          "<tr><td></td><td>\U0001F500</td><td>fork the session</td></tr>"
          "<tr><td></td><td>\U0001F319</td><td>park the tab (asks first during a turn)</td></tr>"
          "<tr><td></td><td>☀️</td><td>wake a parked tab</td></tr>"
          "<tr><td></td><td>\U0001F504</td><td>restart (always asks first)</td></tr>"
          "<tr><td>a question</td><td>1️⃣ … \U0001F51F</td><td>pick that option (several for a multi-pick)</td></tr>"
          "<tr><td></td><td>✅ / ❌</td><td>send the picks / cancel; a text reply is \"Other\"</td></tr>"
          "<tr><td>a confirm prompt</td><td>✅</td><td>yes, within 60 s</td></tr>"
          "<tr><td>your queued prompt</td><td>❌</td><td>take it out of the queue</td></tr>"
          "<tr><td>this post</td><td>➕</td><td>start a new session (I reply with numbered folders)</td></tr>"
          "</table><b>Fleet</b><ul>"
          f"<li>\U0001F7E1 <a href=\"{links.get('root1', '')}\">drag crash fix</a> · Waiting for you</li>"
          f"<li>\U0001F7E1 <a href=\"{links.get('root2', '')}\">second session</a> · Waiting for you</li></ul>")
s, j, ms, err = call("POST", f"{CHAN}/messages", "az",
                     {"subject": "\U0001F4CC Agentmaster · legend and fleet", **html(legend)})
LEGEND = (j or {}).get("id")
print(f"legend post: HTTP {s} in {ms} ms id={LEGEND} {err}")

# 2. The proof card gets its own one-line legend (only the actions that fit the current state).
_, card, _, _ = call("GET", f"{CHAN}/messages/{R1}", "dm")
body = (card or {}).get("body", {}).get("content", "")
line = ('<br><span style="color:#888">\U0001F5BC️ screen · \U0001F4CA status · ↩️ Enter · '
        '\U0001F4DC all · \U0001F500 fork · \U0001F319 park · \U0001F504 restart</span>')
if body and "screen ·" not in body:
    s, _, ms, err = call("PATCH", f"{CHAN}/messages/{R1}", "az", html(body + line))
    print(f"card legend line: HTTP {s} in {ms} ms {err}")

# 3. A question, answered by a number emoji.
q = (MARK + "<br>❓ <b>Claude asks</b> (drag crash fix): Which fix do you want?<br>"
     "1️⃣ Clamp the slot<br>2️⃣ Reject the drop<br>"
     "<i>React with the number. A text reply is \"Other\".</i>")
s, j, ms, err = call("POST", f"{CHAN}/messages/{R1}/replies", "dm", html(q))
QUESTION = (j or {}).get("id")
print(f"question reply: HTTP {s} in {ms} ms id={QUESTION} {err}")

# 4. Which emoji does Graph take as a reaction? Each candidate is set, then unset, on one probe reply.
s, j, ms, err = call("POST", f"{CHAN}/messages/{R1}/replies", "dm",
                     html(MARK + "<br>emoji probe: which reactions Teams accepts (each is removed again)"))
PROBE = (j or {}).get("id")
cands = ["\U0001F5BC️", "\U0001F5BC", "\U0001F4CA", "\U0001F6D1", "↩️", "\U0001F4DC", "\U0001F500",
         "\U0001F319", "☀️", "\U0001F504", "➕", "\U0001F503", "✅", "❌",
         "1️⃣", "2️⃣", "3️⃣", "9️⃣", "\U0001F51F", "⏹️",
         "▶️", "⏸️", "\U0001F5D1️", "⚙️", "\U0001F4CC", "\U0001F50E", "⏎",
         "\U0001F44D", "A", "notanemoji"]
probe = []
for e in cands:
    s1, _, ms1, e1 = call("POST", f"{CHAN}/messages/{R1}/replies/{PROBE}/setReaction", "dm", {"reactionType": e})
    s2 = 0
    if s1 in (200, 204):
        s2, _, _, _ = call("POST", f"{CHAN}/messages/{R1}/replies/{PROBE}/unsetReaction", "dm", {"reactionType": e})
    probe.append({"emoji": e, "codepoints": " ".join(f"U+{ord(c):04X}" for c in e), "set": s1, "unset": s2,
                  "error": e1[:160]})
    print(f"  {e!s:<12} {probe[-1]['codepoints']:<28} set={s1} unset={s2} {e1[:90]}")
rep["probe"] = probe

# 5. What Agentmaster can read back: the reactions now and the full add/remove history with user and time.
s, j, ms, err = call("GET", f"{CHAN}/messages/{R1}/replies/{PROBE}", "dm")
hist = (j or {}).get("messageHistory", [])
rep["probe_history_len"] = len(hist)
rep["probe_history_sample"] = hist[:6]
print(f"probe read-back: HTTP {s}; reactions now={len((j or {}).get('reactions', []))}; "
      f"messageHistory entries={len(hist)}")
for h in hist[:4]:
    r = h.get("reaction") or {}
    print("  ", h.get("actions"), h.get("modifiedDateTime"), r.get("reactionType"), r.get("displayName"),
          ((r.get("user") or {}).get("user") or {}).get("id", "")[:8] + "…")

rep["ids"] = {"legend": LEGEND, "question": QUESTION, "probe": PROBE, "root1": R1, "root2": R2}
rep["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
with open(os.path.join(OUT, "report3.json"), "w", encoding="utf-8") as f:
    json.dump(rep, f, ensure_ascii=False, indent=1)
print("report:", os.path.join(OUT, "report3.json"))
