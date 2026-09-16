# Lily's Developer Log for the rbtree-lab

NOTE: This document's current additions (9/5) mark the transition from my handwritten notes started on 9/1 (either directly from the HW1 pdfs themselves or logs made overtime from the process of reading them).

## 2026-09-01 (evening 1)
STATE: Untouched code base
DID: read entire CS-370-HW1 document and started CS-370-HW1-HowTo as well.
DECIDED: For now, I am going to hold off on planning for the optional Reach task due to a late kick-off for this project at the moment.
LEARNED: While I am not unfamiliar with incremental work, having multiple projects ongoing at the same time may require a heftier start-time, but is significantly paid off later.
NEXT FIRST STEP: Extract notes and highlight important sections based on the extraction table from the HowTo document.
OPEN QUESTIONS: Should I have parent nodes or not? (This seems to be the starting design decision!)

## 2026-09-03 (evening 2)
STATE: github repository shell and CLAUDE setup.
DID: Completed M0 requirements and Git/CLAUDE intialization steps for commit 1. Added notes and highlighted important sections or reminders through the HW1 document.
DECIDED: I will have parent nodes.
LEARNED: The fuzzer's build will come later (M2), and is meant to create semi-random automated inputs to catch flaws in code. Whereas table-driven tests is the known specs and edge cases we will place in test_rbtree.c
NEXT FIRST STEP: Explore with CLAUDE 

## 2026-09-05 (evening 2)
STATE: Completed rb_destroy/rb_create with constructors necessary for rb-tree building (full battery ran in green)
DID: Exploration with CLAUDE planning and implementation mode
DECIDED: single shared NIL sentinel (not per-leaf) -- validate counts it once at the bottom of every path, per spec suggestion. 
LEARNED: Tried a first rough plan and implementation pattern with CLAUDE and learned more about how to plan effectively by utilizing; tips from Claude's website on phrasing common workflows, building small test and waiting for it to pass first before adding other methods, and how to prevent CLAUDE from just jumping into things.
NEXT FIRST STEP: Go back and revise plan, undo last commit and re-commit once satisfied.

## 2026-09-07 (evening 3)
STATE: Completed most Milestone 1 operations (excluding insert), wasn't able to get WSL to work on local devices
DID: Plan and implement appropriately with Claude to implement most of Milestone 1
DECIDED: N/A
LEARNED: Plan with specific details all in one = bigger prompts, plan with incremental = smaller prompts but room for error as code adapts to new changes
NEXT FIRST STEP: Will refactor test_rbtree.c into seperate files for each operation as needed

## 2026-09-08 (evening 3)
STATE: Refactored test_rbtree.c into seperate files for different operations
DID: Refactoring tests and ran until all green
DECIDED: N/A
NEXT FIRST STEP: Finish rb_insert implementation 

## 2026-09-09 (evening 4)
STATE: Complete Milestone 1 (excluding rb_foreach + early fuzzer)  
DID: Complete rb_insert implementation plus test cases (full green battery)
DECIDED: Holding off on fuzzer to create more table-driven tests first
NEXT FIRST STEP: Start rb_delete tests and rb_foreach

## 2026-09-10 (evening 4)
STATE: Unchanged code base since previous log
DID: Planned implementation and test cases for rb_delete
DECIDED: Held off on pushing plan through as I was exhausted and didn't want to commit from a state of uncertainty
NEXT FIRST STEP: Complete rb_foreach and fuzzer

## 2026-09-14 (evening 5)
STATE: All evening plans complete up until 5, missing fuzzer rb_deletion testing
DID: Complete rb_foreach, basic fuzzer (inserts, finds), and rb_delete implementations + full test cases and last suggested helper methods (full green battery)
LEARNED: I should for future projects break the evenings up further into more short concise prompts followed by short responses. I've had a hard time reading through massive amounts of implementation notes and specifications that I received (or even sent)
NEXT FIRST STEP: Finish fuzzer delete testing implementation and turn Claude into adversarial reviewer, follow specs notes for what to look for.

## 2026-09-15 (evening 6)
STATE:
DID: Added fuzzer deletion testing and utilized a fresh agent for adversarial reviewing of codebase. Then, wrote REFLECTION.md and updated PROMPTLOG.md with examples
LEARNED: I think its best to review Claude conversations either the following day or even the night of completing something, as this has helped me substantially remember to either write notes or bring up concerns again that were later cleared