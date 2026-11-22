# teams-proof

The scripts of [`TEAMS.md`](../../doc/agentmaster/TEAMS.md) §15: they measure which Telegram bot features work in a
Teams private channel when Agentmaster writes AS THE OWNER through Microsoft Graph, with no bot.

| Script | What it does |
|---|---|
| `teams_proof_run.py` | Posts two session threads (a root with a subject, then replies) and checks each feature: the card edited in place, formatting, reactions, an Adaptive Card, an inline picture, message size, a soft delete, an @mention, a file card, the order of threads, deep links, the delta catch-up, a change-notification subscription and an activity feed notification. It ends with a checklist reply for the owner. It writes `report.json` and `screen.png`. |
| `teams_proof_part2.py` | The checks that part 1 cannot decide: the scope that a delete needs (two tokens, v1.0 and beta), the edit-down fallback, the size limit (80, 120, 200 KB), the file card, a subscription with an address that resolves, and each token's activity feed refusal. It writes `report2.json`. |
| `teams_proof_listener.py` | The receiver of the card's "Run on the PC" link button: it binds 127.0.0.1 only, logs the first request to `/a` and exits, or exits at its time limit. |
| `teams_proof_emoji.py` | Actions as reactions (TEAMS.md §15.4), part 1: posts the legend post (the owner pins it by hand), gives the first card its one-line legend, posts a question answered by a number emoji, and probes which emoji Graph accepts as reactions (with invalid controls). It writes `report3.json`. Needs `TP_ROOT1` and `TP_ROOT2`. |
| `teams_proof_react.py` | Part 2, after the owner reacted in the client: reads the reactions of both cards and of the question, acts on a 🖼️ (removes the owner's emoji, adds 👌, posts the screen) and on a number (marks the question answered), and looks for a channel pin in Graph. It writes `report4.json`. |

## Run

The scripts read everything from the environment and print no token.

| Variable | Value |
|---|---|
| `TP_SEND` | A delegated Graph token that may post and edit (the test used the Azure CLI's: `az account get-access-token --resource https://graph.microsoft.com --query accessToken -o tsv`) |
| `TP_READ` | A delegated Graph token of an app with `ChannelMessage.Read.All` and `ChannelMessage.Send` |
| `TP_TEAM`, `TP_CH` | The team id and the channel id |
| `TP_OWNER_ID`, `TP_OWNER_NAME` | The owner's Entra object id and display name (the @mention test) |
| `TP_OUT` | A folder for the reports and the picture |
| `TP_LOCAL_URL` | The listener's URL, `http://127.0.0.1:<port>/a` |
| `TP_ROOT1` | Part 2 only: the root id of the first thread that part 1 printed |

```bash
python teams_proof_listener.py 8765 "$TP_OUT/local_clicks.log" 10800 &   # optional: the link-button receiver
PYTHONIOENCODING=utf-8 python teams_proof_run.py
TP_ROOT1=<root id> PYTHONIOENCODING=utf-8 python teams_proof_part2.py
TP_ROOT1=<root id> TP_ROOT2=<root id> PYTHONIOENCODING=utf-8 python teams_proof_emoji.py
# the owner pins the legend post and reacts in the Teams client, then:
PYTHONIOENCODING=utf-8 python teams_proof_react.py
```

- Run them only in a channel where the owner is the only member: every check posts real messages. Each post starts
  with a robot-emoji marker, so another tool that polls the same channel for the owner's answers can tell a proof post
  from an answer.
- The scripts cannot delete what they post unless the token holds `ChannelMessage.ReadWrite`; part 2 edits its big
  probes down to one line.
- Graph allows 1 post per second per channel and per user. The scripts do not pace their calls: they run one call at a
  time, and each call took 0.4–2 s, so no call was throttled (no 429) in the measured run.
