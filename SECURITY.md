# Security Policy

Agentmaster is an independent fork of
[Microsoft Windows Terminal](https://github.com/microsoft/terminal) and is **not**
maintained by or affiliated with Microsoft. Please do **not** report Agentmaster
issues to Microsoft or the MSRC.

## Reporting a vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

Instead, report privately via either:

- **GitHub Security Advisories** (preferred) — open a private report at
  <https://github.com/Nucs/Agentmaster/security/advisories/new>, or
- **Email** — [elibelash@gmail.com](mailto:elibelash@gmail.com).

Please include as much of the following as you can, to help triage:

  * Type of issue (e.g. buffer overflow, injection, privilege escalation, etc.)
  * Full path(s) of the source file(s) involved
  * The affected version / commit (release tag, branch, or commit SHA)
  * Any special configuration required to reproduce
  * Step-by-step instructions to reproduce
  * Proof-of-concept or exploit code, if available
  * The impact, and how an attacker might exploit it

You can expect an initial response within a few days. As a small project there is
no paid bug-bounty program; credit is gladly given for responsibly-disclosed reports.

## Scope

This policy covers the **Agentmaster fork's own code** (the `Agentmaster`-marked
additions). Report vulnerabilities in:

- **upstream Windows Terminal** to the
  [Windows Terminal project](https://github.com/microsoft/terminal/security/policy);
- **Claude Code** or **Codex** to their respective vendors (Anthropic / OpenAI).

## Supported versions

Security fixes target the latest
[release](https://github.com/Nucs/Agentmaster/releases) and the `agentmaster` branch.
