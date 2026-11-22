"""
Teams proof, part 4: read the owner's reactions and act on them as Agentmaster would (TEAMS.md §15.4).
Run it after the owner reacted in the Teams client (🖼️ on a session card, a number on the question reply).
- Reads the reactions of both session cards and of the question reply: emoji, name, time, who.
- A 🖼️ on a card: removes the owner's emoji (Agentmaster signs in as the owner, so it is the caller's own reaction),
  adds 👌 and posts the screen picture (screen.png from part 1) in that card's thread.
- A number on the question: edits the question to "Answered: <number>". The edit sends only the body: a reply has no
  subject. (An edit of a ROOT must send its subject too: a body-only PATCH clears a root's subject.)
- Looks for a channel pin through Graph (there is none: the chat-only pinnedMessages answers 400 for a channel).
Inputs: the environment of teams_proof_run.py; reads the ids from report3.json in TP_OUT. Writes report4.json.
"""
import base64
import json
import os
import time
import urllib.error
import urllib.request

G = "https://graph.microsoft.com"
TOKENS = {"az": os.environ["TP_SEND"], "dm": os.environ["TP_READ"]}
CHP = f"teams/{os.environ['TP_TEAM']}/channels/{os.environ['TP_CH']}"
OWNER = os.environ["TP_OWNER_ID"]
OUT = os.environ["TP_OUT"]
ids = json.load(open(os.path.join(OUT, "report3.json"), encoding="utf-8"))["ids"]
MARK = '\U0001F916 <strong>Claude</strong> · <span style="color:#0a7">[teams-proof]</span>'
PICTURE = "\U0001F5BC"  # 🖼 without U+FE0F: compare emoji without the variation selector


def call(method, url, tok="dm", body=None):
    time.sleep(0.6)  # stay under 1 call per second (the per-user limit)
    req = urllib.request.Request(url, data=json.dumps(body).encode() if body is not None else None, method=method)
    req.add_header("Authorization", "Bearer " + TOKENS[tok])
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            txt = r.read().decode()
            return r.status, (json.loads(txt) if txt.strip() else None), ""
    except urllib.error.HTTPError as e:
        txt = e.read().decode()
        try:
            txt = json.loads(txt)["error"]["message"]
        except Exception:
            pass
        return e.code, None, txt[:200]


def reactions(msg):
    return [{"emoji": r.get("reactionType"), "name": r.get("displayName"), "at": r.get("createdDateTime"),
             "by_owner": ((r.get("user") or {}).get("user") or {}).get("id") == OWNER}
            for r in (msg or {}).get("reactions", [])]


out = {}
for key in ("root1", "root2"):
    _, msg, _ = call("GET", f"{G}/v1.0/{CHP}/messages/{ids[key]}")
    out[key] = reactions(msg)
    print(key, "reactions:", out[key])
    for r in out[key]:
        if r["by_owner"] and r["emoji"].replace("\ufe0f", "") == PICTURE:
            s1, _, e1 = call("POST", f"{G}/v1.0/{CHP}/messages/{ids[key]}/unsetReaction", "dm", {"reactionType": r["emoji"]})
            s2, _, e2 = call("POST", f"{G}/v1.0/{CHP}/messages/{ids[key]}/setReaction", "dm", {"reactionType": "\U0001F44C"})
            png = open(os.path.join(OUT, "screen.png"), "rb").read()
            s3, _, e3 = call("POST", f"{G}/v1.0/{CHP}/messages/{ids[key]}/replies", "dm", {"body": {
                "contentType": "html",
                "content": MARK + f"<br>\U0001F5BC️ the screen at {time.strftime('%H:%M:%S')} (asked by your reaction)"
                                  "<br><img src=\"../hostedContents/1/$value\">"},
                "hostedContents": [{"@microsoft.graph.temporaryId": "1", "contentBytes": base64.b64encode(png).decode(),
                                    "contentType": "image/png"}]})
            out[key + "_acted"] = [s1, s2, s3]
            print(f"  acted: unset the owner's emoji {s1} {e1} | 👌 {s2} {e2} | screen reply {s3} {e3}")

_, q, _ = call("GET", f"{G}/v1.0/{CHP}/messages/{ids['root1']}/replies/{ids['question']}")
out["question"] = reactions(q)
print("question reactions:", out["question"])
picks = [r["emoji"] for r in out["question"] if r["by_owner"] and r["emoji"][:1].isdigit()]
if picks:
    s, _, e = call("PATCH", f"{G}/v1.0/{CHP}/messages/{ids['root1']}/replies/{ids['question']}", "az", {"body": {
        "contentType": "html",
        "content": MARK + f"<br>❓ <b>Claude asks</b>: Which fix do you want?<br><b>✅ Answered: {picks[0]}</b> "
                          f"(by your reaction at {time.strftime('%H:%M')})"}})
    out["question_answered"] = s
    print("  question edited to Answered:", s, e)

s, _, e = call("GET", f"{G}/beta/{CHP}/pinnedMessages")
out["channel_pin_api"] = [s, e]
print("channel pinnedMessages (beta):", s, e[:100])
with open(os.path.join(OUT, "report4.json"), "w", encoding="utf-8") as f:
    json.dump(out, f, ensure_ascii=False, indent=1)
print("report:", os.path.join(OUT, "report4.json"))
