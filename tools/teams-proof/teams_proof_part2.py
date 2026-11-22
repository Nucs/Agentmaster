"""
Teams proof, part 2: the checks that part 1 could not decide.
- Delete: which scope softDelete needs (both tokens, v1.0 and beta), and the edit-down fallback.
- Size: where the message limit is (80, 120, 200 KB), each accepted probe edited down to one line again.
- File card: the OneDrive upload with an encoded path.
- Push: a subscription with a resolvable address that cannot answer the validation.
- Activity feed: each token's refusal, word for word.
Inputs: the same environment as teams_proof_run.py, plus TP_ROOT1 (the first thread's root id).
"""
import json
import os
import time
import datetime
import urllib.error
import urllib.parse
import urllib.request

GRAPH = "https://graph.microsoft.com/v1.0"
BETA = "https://graph.microsoft.com/beta"
TOKENS = {"az": os.environ["TP_SEND"], "dm": os.environ["TP_READ"]}
TEAM, CH, OWNER, OUT = os.environ["TP_TEAM"], os.environ["TP_CH"], os.environ["TP_OWNER_ID"], os.environ["TP_OUT"]
R1 = os.environ["TP_ROOT1"]
CHAN = f"{GRAPH}/teams/{TEAM}/channels/{CH}"
MARK = '\U0001F916 <strong>Claude</strong> · <span style="color:#0a7">[teams-proof]</span>'
rep = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "checks": []}


def call(method, url, tok, body=None, raw=None, ctype="application/json"):
    data = raw if raw is not None else (json.dumps(body).encode("utf-8") if body is not None else None)
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + TOKENS[tok])
    if data is not None:
        req.add_header("Content-Type", ctype)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=90) as r:
            txt = r.read().decode("utf-8", "replace")
            return r.status, (json.loads(txt) if txt.strip() else None), int((time.time() - t0) * 1000), ""
    except urllib.error.HTTPError as e:
        txt = e.read().decode("utf-8", "replace")
        try:
            err = json.loads(txt).get("error", {})
            msg = f"{err.get('code', '')}: {err.get('message', '')}"
        except Exception:
            msg = txt
        return e.code, None, int((time.time() - t0) * 1000), msg[:600]
    except Exception as e:
        return 0, None, int((time.time() - t0) * 1000), f"{type(e).__name__}: {e}"[:600]


def record(name, status, ms, ok, detail="", error="", tok=""):
    rep["checks"].append({"check": name, "status": status, "ms": ms, "ok": ok, "token": tok, "detail": detail,
                          "error": error})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: HTTP {status} in {ms} ms{' via ' + tok if tok else ''}"
          f"{(' - ' + detail) if detail else ''}{(' - ' + error[:300]) if error else ''}")


def html(c):
    return {"body": {"contentType": "html", "content": c}}


# A. Delete: the exact requirement, per token and per API version, on a fresh throw-away reply.
s, j, ms, err = call("POST", f"{CHAN}/messages/{R1}/replies", "az", html(MARK + "<br>delete probe"))
probe = (j or {}).get("id")
for base in (GRAPH, BETA):
    for tok in ("az", "dm"):
        s, _, ms, err = call("POST", f"{base}/teams/{TEAM}/channels/{CH}/messages/{R1}/replies/{probe}/softDelete", tok, {})
        record(f"A softDelete ({base.rsplit('/', 1)[1]}, {tok})", s, ms, s in (200, 204), "", err, tok)
        if s in (200, 204):
            break
# The fallback the owner's tokens allow: edit the message down to one short line.
s, _, ms, err = call("PATCH", f"{CHAN}/messages/{R1}/replies/{probe}", "az", html("·"))
record("A2 edit-down fallback for a delete (PATCH body to one dot)", s, ms, s == 204, "", err, "az")

# B. Shorten part 1's two size probes, found by their text.
s, j, ms, err = call("GET", f"{CHAN}/messages/{R1}/replies?$top=50", "dm")
shortened = 0
for m in (j or {}).get("value", []):
    c = m.get("body", {}).get("content", "")
    if "size probe" in c and len(c) > 5000:
        s2, _, _, err2 = call("PATCH", f"{CHAN}/messages/{R1}/replies/{m['id']}", "az",
                              html(MARK + f"<br>size probe of {len(c.encode('utf-8'))} bytes: accepted (edited down after the test)"))
        shortened += s2 == 204
record("B part-1 size probes edited down", 204 if shortened else 0, 0, shortened == 2, f"edited={shortened}")

# C. Where is the size limit?
for target in (80_000, 120_000, 200_000):
    rows, h = 0, MARK + f"<br>size probe ~{target // 1000} KB<table><tr><th>#</th><th>text</th></tr>"
    while len(h.encode("utf-8")) < target:
        rows += 1
        h += f"<tr><td>{rows}</td><td>line {rows} of the size probe for the Teams message limit</td></tr>"
    h += "</table>"
    size = len(h.encode("utf-8"))
    s, j, ms, err = call("POST", f"{CHAN}/messages/{R1}/replies", "az", html(h))
    record(f"C message of {size} bytes", s, ms, True, "accepted" if s == 201 else "refused", err, "az")
    if j and j.get("id"):
        call("PATCH", f"{CHAN}/messages/{R1}/replies/{j['id']}", "az",
             html(MARK + f"<br>size probe of {size} bytes: accepted (edited down after the test)"))
    else:
        break

# D. File card with an encoded OneDrive path.
fname = f"teams-proof-{int(time.time())}.txt"
path = urllib.parse.quote(f"/Microsoft Teams Chat Files/{fname}")
s, item, ms, err = call("PUT", f"{GRAPH}/me/drive/root:{path}:/content", "dm",
                        raw="teams-proof file attachment test\n".encode("utf-8"), ctype="text/plain")
record("D1 upload to the owner's OneDrive", s, ms, s in (200, 201), (item or {}).get("name", ""), err, "dm")
if item:
    guid = item.get("eTag", "").strip('"').split(",")[0].strip("{}").lower()
    s2, j, ms2, err2 = call("POST", f"{CHAN}/messages/{R1}/replies", "az", {
        "body": {"contentType": "html", "content": MARK + f'<br>a file: <attachment id="{guid}"></attachment>'},
        "attachments": [{"id": guid, "contentType": "reference", "contentUrl": item.get("webUrl"), "name": item.get("name")}]})
    record("D2 file card in the thread (reference attachment)", s2, ms2, s2 == 201, f"id={(j or {}).get('id')}", err2, "az")

# E. Push: a resolvable address that cannot answer the validation (example.com).
exp = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(minutes=45)).strftime("%Y-%m-%dT%H:%M:%SZ")
for tok in ("dm", "az"):
    s, j, ms, err = call("POST", f"{GRAPH}/subscriptions", tok, {
        "changeType": "created,updated,deleted", "notificationUrl": "https://example.com/teams-proof",
        "resource": f"/teams/{TEAM}/channels/{CH}/messages", "expirationDateTime": exp, "clientState": "teams-proof"})
    record(f"E subscription to the channel's messages ({tok})", s, ms, s == 400 and "valid" in err.lower(),
           "permission passed; only the endpoint's validation failed" if (s == 400 and "valid" in err.lower()) else "", err, tok)
    if s == 201 and j:
        call("DELETE", f"{GRAPH}/subscriptions/{j['id']}", tok)

# F. Activity feed: each token's refusal, word for word.
for tok in ("dm", "az"):
    s, j, ms, err = call("POST", f"{GRAPH}/users/{OWNER}/teamwork/sendActivityNotification", tok, {
        "topic": {"source": "text", "value": "teams-proof", "webUrl": "https://teams.microsoft.com"},
        "activityType": "systemDefault", "previewText": {"content": "teams-proof: the turn finished"},
        "templateParameters": [{"name": "systemDefaultText", "value": "teams-proof activity feed test"}]})
    record(f"F activity-feed notification ({tok})", s, ms, s in (200, 204), "", err, tok)

rep["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
with open(os.path.join(OUT, "report2.json"), "w", encoding="utf-8") as f:
    json.dump(rep, f, ensure_ascii=False, indent=1)
print("report:", os.path.join(OUT, "report2.json"))
