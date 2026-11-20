# README art

The repository README's logo (`doc/agentmaster/img/logo.png`) and its twelve heading icons
(`doc/agentmaster/img/icons/*.png`) were drawn by **Codex CLI's built-in `image_gen` tool** (model `gpt-6-astra`,
reasoning effort `xhigh`), with the icon the release identity wears (`res/terminal/images-Dev`) as the reference
image, then cut into transparent PNGs by [`readme_art.py`](readme_art.py).

| Asset | Prompt | Source picked |
|---|---|---|
| `logo.png` (512 px, shown at 168) | [`prompts/logo.md`](prompts/logo.md) | variant 2, "STATE TABS": the title-bar tabs in the Running blue, Waiting goldenrod and Done green of `AgentStatusColors.h` |
| `icons/<slug>.png` (128 px, shown at 30 / 24 / 20 beside an h2 / h3 / h4) | [`prompts/icons.md`](prompts/icons.md) | the one sheet it drew: twelve charcoal tiles in a 4 x 3 grid |

`logo.png` is also the production install's icon: [`tools/release-icon/release_icon.py`](../release-icon/release_icon.py)
`gen` draws the release identity's whole icon set (`res/terminal/images-Agentmaster`) from it, so after a new logo run
that too (and its `check`).

The icons' names follow the README headings they stand before (`readme_art.py`'s `SLUGS`, in the sheet's reading
order): features · fleet · restore · observer · attention · lens · pocket · status · install · docs · upstream · more.

## Regenerating

1. The reference image: `python tools/readme-art/readme_art.py ref <scratch>/ref-icon-green-1024.png`.
2. Codex, one run per prompt, from a scratch folder holding the reference (the desktop app's `codex.exe`: the npm
   copy may be too old for the model):

   ```bash
   codex exec --json --skip-git-repo-check -m gpt-6-astra -c model_reasoning_effort=xhigh \
     -s workspace-write -C <scratch> --image <scratch>/ref-icon-green-1024.png \
     < tools/readme-art/prompts/icons.md > events.jsonl
   ```

   `image_gen` saves each picture as `%USERPROFILE%\.codex\generated_images\<session id>\*.png` (the session id is
   the first event's `thread_id`; the logo prompt's three variants are in generation order). The agent may fail to
   copy them out (its sandboxed file tools can time out): take them from there.
3. Cut: `python tools/readme-art/readme_art.py tiles <sheet.png>` writes `doc/agentmaster/img/icons/`, and
   `python tools/readme-art/readme_art.py logo <variant.png>` writes `doc/agentmaster/img/logo.png`. `tiles` refuses
   a sheet that does not hold exactly twelve tiles.

A new icon in the same style: add its glyph to `prompts/icons.md` (keep the grid a strict rows x columns layout) and
its slug to `SLUGS` in the same position, and regenerate the whole sheet, so every tile keeps one style.
