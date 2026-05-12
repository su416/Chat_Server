---
name: feedback_file_by_file
description: User wants one .h/.cpp pair per conversation turn to understand project thoroughly
type: feedback
---

Implement code changes one .h file and one corresponding .cpp file per conversation turn. Do NOT batch multiple file pairs in one turn.

**Why:** User wants to understand the project thoroughly through small, digestible steps. This is a learning exercise for C++ backend development.

**How to apply:** When given a multi-file change list, present the plan upfront, then implement the first .h/.cpp pair and explicitly ask "ready for the next file pair?" before proceeding. Never implement more than one header + one implementation file per turn.
