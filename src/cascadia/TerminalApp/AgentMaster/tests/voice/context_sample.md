# Sample project document for the voice vocabulary goldens

This file is the "CLAUDE.md" the goldens mine for context terms (VoiceVocabulary.h ExtractContextTerms). It is written to
exercise every rule: hard names that repeat, code-only spellings that must be dropped, shouted English, and compounds of
everyday words.

## Hard names (kept)

- Agentmaster runs each Claude Code session on a ConPTY connection; ConPTY hosts survive a swap; the ConPTY host drains.
- The session database is DuckDB; DuckDB holds every transcript line. The MSIX package ships it; the MSIX is signed.
- XAML islands draw the UI; XAML popups. The UI thread is the UI lane; the UI never blocks. UI, UI, UI, UI, UI, UI.
- WinRT and C++/WinRT both appear; WinRT twice. The x64 build; x64 again. Codex runs too; Codex twice.
- A k3s cluster hosts the build server; k3s again.

## Code-only spellings (dropped)

- `_LaunchClaudeSession`, `std::wstring`, `Get-AppxPackage`, `Get-AppxPackage`, `Debug-B`, `Debug-B`, `am-update.ps1`,
  `am-update.ps1`, `Engine.cpp`, `Engine.cpp`, `Module.dll+0xRVA`, `Module.dll+0xRVA`, C2039, C2039, hooks.log, hooks.log.

## Shouted English and short acronyms (dropped)

- Default ON. Default ON. NOT this. NOT that. BEFORE the build; BEFORE the swap. USERPROFILE and USERPROFILE.
- A VM and a VM; GB and GB (two-letter acronyms seen fewer than eight times).

## Everyday compounds (dropped)

- SessionState flips; SessionState again. TerminalPage hosts it; TerminalPage again. The Scheduler; the Scheduler.

## One-off mentions (dropped)

- Toolhelp once. PEB once.
