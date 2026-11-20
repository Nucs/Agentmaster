You are making artwork for the GitHub README of Agentmaster, an open-source Windows app (a fork of
Windows Terminal) that manages a fleet of Claude Code and Codex AI coding sessions in one terminal window.

The attached image (ref-icon-green-1024.png, also in the current folder) is the app's own icon: a dark
charcoal terminal window (body #3D3D3D with a gentle gradient), three grey title-bar tabs going light to
dark (#CCCCCC, #999999, #666666), a beveled light-grey ">" chevron, and a lime "DEV" pill (#9BF00B).

TASK: call your image generation tool exactly ONCE to create ONE sprite sheet of 12 matching section
icons for the README's headings. Requirements:

- Landscape canvas 1536x1024, flat pure white (#FFFFFF) background, a strict 4-column x 3-row grid. Every
  icon is centered in its own cell, all twelve the SAME size (about 260 px square), with wide, clean white
  gutters between them. Nothing touches another icon or the canvas edge.
- NO text anywhere: no letters, words, numbers, labels, captions, logos or watermark.
- One style for all twelve, matching the attached icon: each icon is a rounded-square app tile (corner
  radius about 22% of its side) in dark charcoal (#2E2E2E at the bottom to #3F3F3F at the top), with a thin
  lighter inner edge highlight, carrying ONE bold, simple, centered glyph drawn in the Windows 11 Fluent
  style of the attached icon: soft gradients, a slight bevel like its ">" chevron, clean geometric
  shapes, no outlines, no clutter, readable when shown at 28 px.
- Accent colors only from this palette (the app's session-state colors): DodgerBlue #1E90FF, Goldenrod
  #DAA520, OrangeRed #FF4500, Crimson #DC143C, MediumSeaGreen #3CB371, Gray #808080, amber #FFC107,
  lime #9BF00B, light grey #D0D0D0. Each glyph uses one to three of them plus light grey.
- The twelve glyphs, row by row, left to right:
  Row 1: (1) a ">" terminal chevron with a small amber four-point spark above it;
         (2) a kanban board: three columns of small rounded cards, one column blue, one goldenrod, one green;
         (3) a small terminal window with a circular "restore" arrow wrapped around it, in green;
         (4) an eye whose pupil is a ">" chevron, the iris DodgerBlue.
  Row 2: (5) a vertical stack of four round status dots, top to bottom blue, goldenrod, orange-red, green;
         (6) a magnifying glass over a short row of three browser-like tabs, each tab with a colored dot;
         (7) a smartphone whose screen shows a chat bubble with a ">" chevron;
         (8) one MediumSeaGreen status dot with a soft pulse ring around it.
  Row 3: (9) a bold downward arrow into an open tray, the arrow lime;
         (10) an open book with a small pair of code brackets "</>" floating above it, in blue;
         (11) two interlocked chain links, one light grey and one lime;
         (12) a plus sign built from four small rounded squares in blue, goldenrod, green and orange-red.

When the image exists, copy the generated PNG into the current folder as sheet.png (it is saved under
%USERPROFILE%\.codex\generated_images\<this session id>\). Then reply with one line: DONE and the path of
the generated PNG. Do not create or change any other file.
