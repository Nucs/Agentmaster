"""
Teams proof run: what the Telegram bot's features become in the owner's private channel when
Agentmaster acts through Microsoft Graph AS THE OWNER (no bot). Each check sends, edits, reacts, deletes or
reads, and records the HTTP status, the time it took and what Teams stored. Nothing here prints a token.

Inputs (environment):
  TP_SEND - a delegated Graph token of the Azure CLI (Group.ReadWrite.All: post and edit; it cannot read or react).
  TP_READ - a delegated Graph token of an app the tenant already allows, with ChannelMessage.Read.All,
            ChannelMessage.Send and Files.ReadWrite (it cannot edit).
  TP_TEAM, TP_CH, TP_OWNER_ID, TP_OWNER_NAME - the team, the channel, the owner's Entra object id and name.
  TP_OUT  - the folder for report.json and the rendered screen picture.
  TP_LOCAL_URL - the URL of the local button listener (http://127.0.0.1:<port>/a).

Every post starts with a robot-emoji marker, so another tool that polls the same channel for the owner's answers can
tell a proof post from an answer.
"""
import base64
import datetime
import json
import os
import time
import urllib.error
import urllib.request

GRAPH = "https://graph.microsoft.com/v1.0"
TOKENS = {"az": os.environ["TP_SEND"], "dm": os.environ["TP_READ"]}
TEAM = os.environ["TP_TEAM"]
CH = os.environ["TP_CH"]
OWNER = os.environ["TP_OWNER_ID"]
OUT = os.environ["TP_OUT"]
LOCAL_URL = os.environ.get("TP_LOCAL_URL", "http://127.0.0.1:8765/a")
OWNER_NAME = os.environ.get("TP_OWNER_NAME", "Owner")
CHAN = f"{GRAPH}/teams/{TEAM}/channels/{CH}"
MARK = '\U0001F916 <strong>Claude</strong> \u00b7 <span style="color:#0a7">[teams-proof]</span>'

report = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "checks": [], "tokens": {}}


def token_facts(tok):
    """The app name and the delegated scopes of a JWT, read locally. The token itself is never printed."""
    try:
        part = tok.split(".")[1]
        part += "=" * (-len(part) % 4)
        claims = json.loads(base64.urlsafe_b64decode(part.encode()))
        return {"app": claims.get("app_displayname"), "scp": sorted((claims.get("scp") or "").split())}
    except Exception as e:
        return {"error": f"{type(e).__name__}"}


def call(method, url, tok_name, body=None, raw=None, ctype="application/json"):
    """One Graph call with one token. Returns (status, json-or-None, ms, error text); never raises for HTTP."""
    data = raw if raw is not None else (json.dumps(body).encode("utf-8") if body is not None else None)
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + TOKENS[tok_name])
    if data is not None:
        req.add_header("Content-Type", ctype)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            txt = r.read().decode("utf-8", "replace")
            return r.status, (json.loads(txt) if txt.strip() else None), int((time.time() - t0) * 1000), ""
    except urllib.error.HTTPError as e:
        ms = int((time.time() - t0) * 1000)
        txt = e.read().decode("utf-8", "replace")
        try:
            err = json.loads(txt).get("error", {})
            msg = f"{err.get('code', '')}: {err.get('message', '')}"[:400]
        except Exception:
            msg = txt[:400]
        return e.code, None, ms, msg
    except Exception as e:  # transport failure
        return 0, None, int((time.time() - t0) * 1000), f"{type(e).__name__}: {e}"[:400]


def call_any(method, url, body=None, order=("az", "dm"), **kw):
    """Try each token in order; a 401/403 moves to the next. Returns (status, json, ms, error, token used)."""
    last = None
    for name in order:
        s, j, ms, err = call(method, url, name, body, **kw)
        last = (s, j, ms, err, name)
        if s not in (401, 403):
            return last
    return last


def record(name, status, ms, ok, detail="", error="", tok=""):
    report["checks"].append({"check": name, "status": status, "ms": ms, "ok": ok, "token": tok,
                             "detail": detail, "error": error})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: HTTP {status} in {ms} ms"
          f"{' via ' + tok if tok else ''}{(' - ' + detail) if detail else ''}{(' - ' + error) if error else ''}")


def html(content):
    return {"body": {"contentType": "html", "content": content}}


def get_root(mid):
    return call("GET", f"{CHAN}/messages/{mid}", "dm")


def get_reply(root, rid):
    return call("GET", f"{CHAN}/messages/{root}/replies/{rid}", "dm")


def save():
    report["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(os.path.join(OUT, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)


for n in TOKENS:
    report["tokens"][n] = token_facts(TOKENS[n])
    print(f"token {n}: app={report['tokens'][n].get('app')} scopes={' '.join(report['tokens'][n].get('scp', []))}")

# 1. The session card as a thread root: a post with a subject (the topic's name).
card_html = (MARK + "<br><b>\U0001F535 Running since 14:02</b><br>\U0001F4CC <b>\U0001F7E6 drag crash fix</b><br>"
             "<code>/source/Agentmaster \u00b7 main \u00b7 opus-5-5 \u00b7 max \u00b7 182K</code><br>"
             "<i>Runner Auto \u00b7 queue 0</i>")
s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages",
                             {"subject": "\U0001F535 teams-proof \u00b7 drag crash fix", **html(card_html)})
R1 = (j or {}).get("id")
record("01 thread root = session card (post with subject)", s, ms, s == 201, f"id={R1}", err, tk)
if not R1:
    save()
    raise SystemExit("no root: stop")

# 2. A reply: the prompt line as the mirror posts it.
s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies",
                             html(MARK + "<br><b>\U0001F464 You</b> \u00b7 14:02 \u00b7 typed at the PC<br>fix the drag crash"))
PROMPT = (j or {}).get("id")
record("02 reply in the thread (prompt line)", s, ms, s == 201, f"id={PROMPT}", err, tk)

# 3. Rich formatting: what Teams keeps of a final reply rendered from Markdown to HTML.
rich = (MARK + "<h2>Final reply</h2><p><b>bold</b> <i>italic</i> <s>strike</s> <u>underline</u> "
        "<code>inline code</code> <a href=\"https://learn.microsoft.com\">a link</a> \u2705 \U0001F680</p>"
        "<blockquote>a quoted line</blockquote><ul><li>bullet one</li><li>bullet two</li></ul>"
        "<ol><li>step one</li><li>step two</li></ol>"
        "<pre><code class=\"language-cpp\">int main() {\n    return 0; // a code block\n}</code></pre>"
        "<table><tr><th>File</th><th>Lines</th><th>Change</th></tr>"
        "<tr><td>TabDragMath.h</td><td>12</td><td>clamp the slot</td></tr>"
        "<tr><td>TerminalPage.cpp</td><td>3</td><td>null check</td></tr></table>")
s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies", html(rich))
FINAL = (j or {}).get("id")
kept = {}
if FINAL:
    _, g, _, _ = get_reply(R1, FINAL)
    body = (g or {}).get("body", {}).get("content", "")
    for tag in ("<h2", "<b>", "<i>", "<s>", "<u>", "<code", "<a ", "<blockquote", "<ul", "<ol", "<pre", "<table"):
        kept[tag] = tag in body
    report["stored_rich_html"] = body[:6000]
lost = [k.strip("<") for k, v in kept.items() if not v]
record("03 rich formatting kept (h2 b i s u code link quote lists codeblock table)", s, ms,
       s == 201 and not lost, "kept=" + ",".join(k.strip("<").strip() for k, v in kept.items() if v)
       + (f"; changed or lost={','.join(lost)}" if lost else ""), err, tk)

# 4. Editing the card (the root): the state changes to Waiting for you, and the subject follows.
new_card = card_html.replace("\U0001F535 Running since 14:02", "\U0001F7E1 Waiting for you since 14:09")
s, _, ms, err, tk = call_any("PATCH", f"{CHAN}/messages/{R1}",
                             {"subject": "\U0001F7E1 teams-proof \u00b7 drag crash fix", **html(new_card)})
_, g, _, _ = get_root(R1)
edited = bool(g and "Waiting for you" in g.get("body", {}).get("content", ""))
record("04 edit the card in place (PATCH root body + subject)", s, ms, s in (200, 204) and edited,
       f"subject now={(g or {}).get('subject')!r} lastEditedDateTime={(g or {}).get('lastEditedDateTime')}", err, tk)
ROOT_URL = (g or {}).get("webUrl", "")

# 5. Reactions as the prompt's status: 👀 queued, 👨‍💻 running, 🫡 sent from the queue, 👌 answered.
if PROMPT:
    url = f"{CHAN}/messages/{R1}/replies/{PROMPT}"
    seq, total, fails, used = [], 0, [], ""
    for verb, emo in (("setReaction", "\U0001F440"), ("unsetReaction", "\U0001F440"),
                      ("setReaction", "\U0001F468\u200D\U0001F4BB"), ("unsetReaction", "\U0001F468\u200D\U0001F4BB"),
                      ("setReaction", "\U0001FAE1"), ("unsetReaction", "\U0001FAE1"),
                      ("setReaction", "\U0001F44C")):
        s, _, ms, err, tk = call_any("POST", f"{url}/{verb}", {"reactionType": emo}, order=("dm", "az"))
        seq.append(f"{verb}({emo})={s}")
        total += ms
        used = tk
        if s not in (200, 204):
            fails.append(f"{verb}({emo}): {err}")
    _, g, _, _ = get_reply(R1, PROMPT)
    rx = [r.get("reactionType") for r in (g or {}).get("reactions", [])]
    record("05 reaction status on the prompt (4 emoji set and unset)", "/".join(x.split("=")[1] for x in seq), total,
           not fails, f"reactions stored now={rx}", "; ".join(fails), used)

# 6. An Adaptive Card: facts, a deep-link button, an imBack button and a button that opens a local URL.
card = {
    "type": "AdaptiveCard", "$schema": "http://adaptivecards.io/schemas/adaptive-card.json", "version": "1.4",
    "body": [
        {"type": "TextBlock", "text": "\U0001F7E1 Waiting for you since 14:09", "weight": "Bolder", "wrap": True},
        {"type": "TextBlock", "text": "\U0001F7E6 drag crash fix", "size": "Large", "wrap": True},
        {"type": "FactSet", "facts": [{"title": "Folder", "value": "/source/Agentmaster"},
                                      {"title": "Branch", "value": "main"},
                                      {"title": "Model", "value": "opus-5-5 \u00b7 max \u00b7 182K"},
                                      {"title": "Runner", "value": "Auto \u00b7 queue 0"}]},
        {"type": "TextBlock", "text": "Which fix do you want? (a question card)", "wrap": True},
        {"type": "Input.ChoiceSet", "id": "choice", "style": "expanded",
         "choices": [{"title": "1. Clamp the slot", "value": "1"}, {"title": "2. Reject the drop", "value": "2"}]}],
    "actions": [
        {"type": "Action.OpenUrl", "title": "\U0001F517 Open this thread", "url": ROOT_URL or "https://teams.microsoft.com"},
        {"type": "Action.Submit", "title": "/status (imBack)", "data": {"msteams": {"type": "imBack", "value": "/status"}}},
        {"type": "Action.Submit", "title": "Send the choice (Submit)", "data": {"am": "answer"}},
        {"type": "Action.OpenUrl", "title": "\u25B6 Run on the PC (local URL)", "url": LOCAL_URL + "?s=teams-proof&choice=status"}],
}
s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies", {
    "body": {"contentType": "html", "content": MARK + '<attachment id="card1"></attachment>'},
    "attachments": [{"id": "card1", "contentType": "application/vnd.microsoft.card.adaptive",
                     "content": json.dumps(card, ensure_ascii=False)}]})
CARD = (j or {}).get("id")
record("06 Adaptive Card reply (FactSet, choice set, OpenUrl, imBack, Submit, local URL)", s, ms, s == 201,
       f"id={CARD}", err, tk)

# 7. An inline picture of the terminal screen (hostedContents), as the card's screenshot.
png_path = os.path.join(OUT, "screen.png")
try:
    from PIL import Image, ImageDraw, ImageFont
    W, H = 1024, 320
    im = Image.new("RGB", (W, H), (12, 12, 12))
    d = ImageDraw.Draw(im)
    font = None
    for cand in ("C:/Windows/Fonts/CascadiaMono.ttf", "C:/Windows/Fonts/consola.ttf"):
        try:
            font = ImageFont.truetype(cand, 17)
            break
        except Exception:
            pass
    font = font or ImageFont.load_default()
    lines = [("\u25cf Read(TabDragMath.h)", (120, 200, 120)), ("  \u23bf  Read 214 lines", (150, 150, 150)),
             ("\u25cf Edit(TabDragMath.h)", (120, 200, 120)), ("  \u23bf  Updated with 3 additions", (150, 150, 150)),
             ("\u25cf The slot is clamped now; the drag no longer reads past the strip.", (230, 230, 230)),
             ("\u2500" * 96, (90, 90, 90)), ("\u276f fix the next one too", (230, 230, 230)),
             ("\u2500" * 96, (90, 90, 90)), ("  bypass permissions on \u00b7 opus-5-5 \u00b7 182K", (170, 120, 220))]
    y = 16
    for t, c in lines:
        d.text((16, y), t, font=font, fill=c)
        y += 32
    im.save(png_path, optimize=True)
    png = open(png_path, "rb").read()
    s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies", {
        "body": {"contentType": "html",
                 "content": MARK + '<br>\U0001F5A5 the screen at 14:09:12<br><img src="../hostedContents/1/$value">'},
        "hostedContents": [{"@microsoft.graph.temporaryId": "1", "contentBytes": base64.b64encode(png).decode(),
                            "contentType": "image/png"}]})
    record("07 inline screen picture (hostedContents PNG 1024x320)", s, ms, s == 201,
           f"png={len(png)} bytes id={(j or {}).get('id')}", err, tk)
except Exception as e:
    record("07 inline screen picture", 0, 0, False, "", f"{type(e).__name__}: {e}")

# 8. Message size: a ~20 KB table and a ~40 KB table. Accepted probes are deleted again right away.
for target in (20_000, 40_000):
    rows, h = 0, MARK + f"<br>size probe ~{target // 1000} KB<table><tr><th>#</th><th>text</th></tr>"
    while len(h.encode("utf-8")) < target:
        rows += 1
        h += f"<tr><td>{rows}</td><td>line {rows} of the size probe for the Teams message limit</td></tr>"
    h += "</table>"
    size = len(h.encode("utf-8"))
    s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies", html(h))
    record(f"08 message of {size} bytes ({rows} table rows)", s, ms, True,
           "accepted" if s == 201 else "refused", err, tk)
    if j and j.get("id"):
        ds, _, dms, derr, dtk = call_any("POST", f"{CHAN}/messages/{R1}/replies/{j['id']}/softDelete", {})
        _, g, _, _ = get_reply(R1, j["id"])
        record(f"09 soft delete of the {size}-byte reply", ds, dms,
               ds in (200, 204) and bool((g or {}).get("deletedDateTime")),
               f"deletedDateTime={(g or {}).get('deletedDateTime')}", derr, dtk)

# 10. A mention of the owner (the only way a post can ask for attention in a channel).
s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies", {
    "body": {"contentType": "html", "content": MARK + '<br><at id="0">' + OWNER_NAME + '</at> the turn finished (mention test)'},
    "mentions": [{"id": 0, "mentionText": OWNER_NAME,
                  "mentioned": {"user": {"id": OWNER, "displayName": OWNER_NAME, "userIdentityType": "aadUser"}}}]})
MENTION = (j or {}).get("id")
stored = []
if MENTION:
    _, g, _, _ = get_reply(R1, MENTION)
    stored = [m.get("mentionText") for m in (g or {}).get("mentions", [])]
record("10 @mention of the owner", s, ms, s == 201 and bool(stored), f"mentions stored={stored}", err, tk)

# 11. A file: uploaded to the owner's OneDrive and attached as a file card (the /download "as file" case).
fname = f"teams-proof-{int(time.time())}.txt"
s, item, ms, err = call("PUT", f"{GRAPH}/me/drive/root:/Microsoft Teams Chat Files/{fname}:/content", "dm",
                        raw="teams-proof file attachment test\n".encode("utf-8"), ctype="text/plain")
if item:
    guid = item.get("eTag", "").strip('"').split(",")[0].strip("{}").lower()
    s2, j, ms2, err2, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies", {
        "body": {"contentType": "html", "content": MARK + f'<br>a file: <attachment id="{guid}"></attachment>'},
        "attachments": [{"id": guid, "contentType": "reference", "contentUrl": item.get("webUrl"), "name": item.get("name")}]})
    record("11 file card (OneDrive upload + reference attachment)", s2, ms + ms2, s2 == 201,
           f"file={item.get('name')} id={(j or {}).get('id')}", err2, tk)
else:
    record("11 file card (OneDrive upload)", s, ms, False, "", err, "dm")

# 12. A second session thread, then a reply in the FIRST one: what order does the API give?
s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages",
                             {"subject": "\U0001F7E9 teams-proof \u00b7 second session",
                              **html(MARK + "<br><b>\U0001F7E1 Waiting for you</b><br>\U0001F4CC <b>\U0001F7E9 second session</b>")})
R2 = (j or {}).get("id")
record("12 a second session thread (root)", s, ms, s == 201, f"id={R2}", err, tk)
time.sleep(2)
s, _, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies",
                             html(MARK + "<br>a new reply in the FIRST session's thread (order test)"))
ls, lst, lms, lerr = call("GET", f"{CHAN}/messages?$top=6", "dm")
order = [(m.get("subject"), m.get("createdDateTime"), m.get("lastModifiedDateTime")) for m in (lst or {}).get("value", [])]
report["api_order"] = order
record("13 API order of threads after a reply to the older one", ls, lms, ls == 200,
       "subjects newest first: " + " | ".join(str(o[0]) for o in order[:4]), lerr, "dm")

# 14. Deep links Graph gives for each root.
_, g1, _, _ = get_root(R1)
_, g2, _, _ = get_root(R2) if R2 else (0, None, 0, "")
report["deep_links"] = {"root1": (g1 or {}).get("webUrl"), "root2": (g2 or {}).get("webUrl")}
record("14 deep link per thread (webUrl)", 200, 0, bool((g1 or {}).get("webUrl")),
       f"root1 lastModified={(g1 or {}).get('lastModifiedDateTime')}")

# 15. Catch-up after a downtime: the delta query of the channel (one call, not polling).
s, j, ms, err = call("GET", f"{CHAN}/messages/delta?$top=5", "dm")
record("15 catch-up read (messages/delta, roots only)", s, ms, s == 200,
       f"items={len((j or {}).get('value', []))} deltaLink={'@odata.deltaLink' in (j or {})} nextLink={'@odata.nextLink' in (j or {})}",
       err, "dm")

# 16. Push for inbound: may the owner subscribe to this channel's messages? (An address that cannot answer:
#     a permission refusal is 403; an allowed request fails only the endpoint validation.)
exp = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(minutes=45)).strftime("%Y-%m-%dT%H:%M:%SZ")
s, j, ms, err, tk = call_any("POST", f"{GRAPH}/subscriptions", {
    "changeType": "created,updated,deleted", "notificationUrl": "https://teams-proof.invalid/notify",
    "resource": f"/teams/{TEAM}/channels/{CH}/messages", "expirationDateTime": exp, "clientState": "teams-proof"},
    order=("dm", "az"))
allowed = s == 400 and "validation" in err.lower()
record("16 change-notification subscription as the owner (endpoint deliberately unreachable)", s, ms, allowed,
       "permission OK, only the endpoint failed" if allowed else ("CREATED (unexpected)" if s == 201 else "see error"),
       err, tk)
if s == 201 and j and j.get("id"):
    call("DELETE", f"{GRAPH}/subscriptions/{j['id']}", tk)

# 17. One sound per turn without a bot: an activity-feed notification to the owner.
s, j, ms, err, tk = call_any("POST", f"{GRAPH}/users/{OWNER}/teamwork/sendActivityNotification", {
    "topic": {"source": "text", "value": "teams-proof", "webUrl": ROOT_URL or "https://teams.microsoft.com"},
    "activityType": "systemDefault", "previewText": {"content": "teams-proof: the turn finished"},
    "templateParameters": [{"name": "systemDefaultText", "value": "teams-proof activity feed test"}]},
    order=("dm", "az"))
record("17 activity-feed notification to the owner (systemDefault)", s, ms, s in (200, 204),
       "SENT - check the activity feed" if s in (200, 204) else "refused", err, tk)

# 18. The owner's turn: what only a person can check, written in the thread itself.
turn = (MARK + "<br><b>Your turn</b> (4 checks, about 2 minutes):<ol>"
        "<li>Reply in THIS thread with one word: <b>seen</b></li>"
        "<li>On the card above, select <b>/status (imBack)</b> and <b>Send the choice (Submit)</b>. Note what Teams shows.</li>"
        "<li>On the card above, select <b>Run on the PC (local URL)</b>. Note what opens.</li>"
        "<li>Did Teams notify you (PC or phone) for any teams-proof post, or show the channel as unread?</li></ol>")
s, j, ms, err, tk = call_any("POST", f"{CHAN}/messages/{R1}/replies", html(turn))
record("18 the owner's checklist reply", s, ms, s == 201, f"id={(j or {}).get('id')}", err, tk)

report["ids"] = {"team": TEAM, "channel": CH, "root1": R1, "root2": R2, "prompt": PROMPT, "final": FINAL,
                 "card": CARD, "mention": MENTION}
save()
print("report:", os.path.join(OUT, "report.json"))
print("root1 webUrl:", report["deep_links"]["root1"])
