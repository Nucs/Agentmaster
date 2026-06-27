# Contributing to Agentmaster

> Agentmaster is a fork of [`microsoft/terminal`](https://github.com/microsoft/terminal);
> the upstream Windows Terminal project's own contributor guide lives in that repository.

Thanks for your interest in Agentmaster! Agentmaster is a fork of Microsoft's
Windows Terminal that turns it into a manager for multiple Claude Code / Codex
sessions.

## The CLA is required — read this first

Agentmaster is **dual-licensed**: the public **AGPL-3.0-or-later**
([`LICENSE`](LICENSE)) plus a **commercial license**
([`LICENSE-COMMERCIAL.md`](LICENSE-COMMERCIAL.md)). Offering a commercial license
is only possible if the project can relicense the code it ships — so:

> **Every contribution requires signing the Contributor License Agreement
> ([`CLA.md`](CLA.md)). No pull request is merged until the CLA check passes.**

- A bot will prompt you to sign on your first pull request; your agreement is
  recorded against your GitHub identity.
- The CLA grants the project the right to license your contribution under the
  AGPL **and** under commercial/proprietary terms. You **keep your copyright**
  (it's a license, not an assignment).
- If you contribute on behalf of an employer, ensure you have permission, or
  arrange corporate coverage with us, before you submit.

We **cannot merge any external contribution before the CLA is signed** — doing
so would compromise the project's ability to offer the commercial license.

## How to contribute

1. **File an issue first** for anything non-trivial, so we can agree on the
   approach before you invest time.
2. **Fork** the repo and create a topic branch off `agentmaster`.
3. Make your change. Keep the diff against upstream minimal where practical
   (prefer additive files + small touches at integration points), and **mark new
   Agentmaster code with `Agentmaster`** so it stays distinguishable from the
   upstream MIT base.
4. **Build and test** — see [`CLAUDE.md`](CLAUDE.md) (Building FAST / Deploy &
   run) and run the engine harness under `src/cascadia/TerminalApp/AgentMaster/tests/`.
5. Open a **pull request** against `agentmaster`, describe the change, link the
   issue, and **sign the CLA** when prompted.

## Licensing & notices hygiene (don't regress)

- Do **not** remove or alter the upstream MIT license
  ([`LICENSE-MIT`](LICENSE-MIT)) or third-party attributions
  ([`NOTICE.md`](NOTICE.md) / [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt)).
- New code you author is contributed under the CLA and shipped under the project
  license(s); new **third-party** code you bring in must be permissively
  licensed (MIT / BSD / Apache-2.0 / public-domain — AGPL-compatible) and its
  notice added. Flag anything copyleft (GPL/LGPL/MPL) **before** submitting.

## Reporting security issues

Please do **not** open a public issue for security vulnerabilities. Email
**elibelash@gmail.com** with the details instead.

## Code of conduct

Be respectful and constructive. Harassment or abuse isn't tolerated.
