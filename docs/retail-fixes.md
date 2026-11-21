# Retail fixes

OpenBF2 follows the shipped behavior of Battlefield 2 1.50, quirks and documented defects
included ([Development guide](development.md)). This guide lists every place where it
deliberately corrects behavior that is wrong in retail, and states the rule for adding one.

## What counts as a retail fix

Retail behavior is incorrect when the shipped game does something its own design evidently did
not intend:

- a crash, a hang, a memory error or a leak;
- a security defect, such as trusting an unchecked length from the network;
- a wrong result, or a wrong or misleading message to the player or the operator;
- lost input, lost data, or a setting that does not take effect the way the game says.

A retail fix corrects that behavior and changes nothing else. The following are not retail
fixes and follow the issue workflow in the [Development guide](development.md):

- improvements and features, including support for systems retail never ran on, such as other
  operating systems, high-DPI or wide displays and newer hardware;
- different defaults and changes of taste;
- every change to an ABI, protocol or file-format contract.

## Rule

- **Client fix.** It needs no separate issue.
- **Server fix.** It needs no separate issue when it stays backwards compatible for retail
  clients: an unmodified retail 1.50 client connects, loads, joins, plays, disconnects and
  reconnects as it does against a retail server, and nothing it sends, receives or relies on
  changes. Any other server fix is discussed in an issue first.
- **Shared code.** A fix in code that the server also runs is a server fix for this rule.
- The pull request that makes the fix adds its entry to this guide.
- Beside the code, mark the fix `[CHOSEN, not recovered]` and name its entry here in place of an
  issue.

## Entry format

Each entry is a heading that names the fix, followed by these fields:

- **Retail:** what 1.50 does, with the evidence: the image and address, or how to reproduce it on
  retail.
- **OpenBF2:** what it does instead.
- **Applies to:** client, server or both, and the platforms.
- **Retail clients:** for a server or shared fix, why an unmodified retail client is unaffected.
- **Source:** the files, and the issue where one exists.

## Client

## Server

## Client and server
