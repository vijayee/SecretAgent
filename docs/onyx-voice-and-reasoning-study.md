# How Onyx Speaks and Thinks — a codebase study

**Date:** 2026-11-05. **Source:** the Onyx codebase (`~/Workspace/src/github.com/vijayee/onyx`,
read-only) — everything that defines how the assistant speaks to the user and how it thinks about
its prompts/problems. Quotes are VERBATIM from the code; file:line refs included. Complements
`docs/architecture-research.html`'s Onyx section (that study verified the loop's mechanics; this
one extracts the language and the judgment).

---

## Part 1 — How Onyx speaks to the user

### 1.1 The base voice is a small, editable paragraph

`backend/onyx/prompts/chat_prompts.py:13-28` — `DEFAULT_SYSTEM_PROMPT` (the comment says the first
line "is intended to help guide the general feel/behavior of the system"; admins can edit it — it
is persona 0's record, seeded by a migration, `get_default_base_system_prompt`):

> You are an expert assistant who is truthful, nuanced, insightful, and efficient. Your goal is to
> deeply understand the user's intent, think step-by-step through complex problems, provide clear
> and accurate answers, and proactively anticipate helpful follow-up information. Whenever there is
> any ambiguity around the user's query (or more information would be helpful), you use available
> tools (if any) to get more context.

That one paragraph carries the whole stance: **truthful** (never fabricate), **nuanced** (no false
binaries), **efficient**, **deeply understand the intent**, **think step-by-step**,
**anticipate follow-ups** — the exact axes the hammer later formalized as checkable principles
(truth first / read intent / anticipate / signal density). The continuity is real: the hammer's
spec was distilled from this assistant's actual behavior, and the code shows the same values in
the same order.

Then the formatting contract:

> # Response Style
> You use different text styles, bolding, emojis (sparingly), block quotes, and other formatting
> to make your responses more readable and engaging. You use proper Markdown and LaTeX to format
> your responses for math, scientific, and chemical formulas... For code you prefer to use
> Markdown and specify the language.

Format as a tool, with an explicit ceiling on ornament ("sparingly").

### 1.2 The assistant never names itself

`DEFAULT_SYSTEM_PROMPT` says "You are an expert assistant" — never "You are Onyx." An older
variant ("You are a highly capable, thoughtful, and precise assistant...") also goes unnamed.
The only named self-references are surface-specific: the Discord bot says "You are a Discord bot
named OnyxBot" (`onyxbot/discord/handle_message.py:402`). Identity belongs to the SURFACE, not
the voice. The product name (`ONYX_DEFAULT_APPLICATION_NAME`) is branding the prompt never uses.

### 1.3 Citations: inline, numbered, no link soup

`chat_prompts.py:50-54` — the guidance appended via `{{CITATION_GUIDANCE}}`:

> CRITICAL: If referencing knowledge from searches, cite relevant statements INLINE using the
> format [1], [2], [3], etc. to reference the "document" field. DO NOT provide any links following
> the citations. Cite inline as opposed to leaving all citations until the very end of the response.

Three rules in one breath: cite INLINE (not a bibliography), use numbers (the wire is JSON
`{"document": 1, ...}` so the model cites a single integer), and never append raw links. The docs
are packed with the key deliberately NAMED `document` — `chat/README.md:153-155`:

> It's called 'document' so that the LLM does not create weird artifacts in reasoning like "I
> should reference citation_id: 5 for..."

Naming is prompt engineering: the shortest possible referent is the one the model uses.

### 1.4 Reminders say what the turn failed to — then correct course

Per-turn corrections arrive in a `<system-reminder>` message, and the model is TOLD what those
tags are (`prompts/constants.py:8-11`):

> User messages may include <system-reminder> and </system-reminder> tags. These
> <system-reminder> tags contain useful information and reminders. They are automatically added
> by the system and are not actual user inputs. Behave in accordance to these instructions if
> relevant, and continue normally if they are not.

The reminders themselves (`chat_prompts.py:44-70`):

- **Completeness** (appended whenever searches ran): "Address every part of the question
  explicitly. When the source documents state specific values, limits, dates, names, commands, or
  steps relevant to the question, include them exactly rather than summarizing them away...
  If the documents do not contain the requested information, say so directly rather than answering
  with adjacent information that does not address the question."
- **Coverage**: "When the answer has several components, lay them out as a list rather than
  compressing them into one sentence... Check that the passage you rely on matches the full
  context of the question - a near-miss document will not contain the details it asks for."
- **Last cycle**: "You are on your last cycle and no longer have any tool calls available. You
  must answer the query now to the best of your ability."

The uncertainty doctrine is inside the completeness reminder: **say so directly rather than
answering with adjacent information** — a near-miss document is not an answer, and saying "the
documents don't say" beats an adjacent-sounding reply. (Slack even declines silently rather than
post a hedge: "Found no citations or quotes when trying to answer.")

### 1.5 Asking the user: conversational, numbered, capped

Deep research's clarification step is the most explicit voice direction in the codebase
(`deep_research/orchestration_layer.py:9-25`):

> Be conversational and friendly, prefer saying "could you" rather than "I need" etc.
>
> If you need to ask questions, follow these guidelines:
> - Be concise and do not ask more than 5 questions.
> - If there are ambiguous terms or questions, ask the user to clarify.
> - Your questions should be a numbered list for clarity.
> ...
> - Wrap up with a quick sentence on what the clarification will help with, it's ok to reference
>   the user query closely here.

Asking is a craft with rules: ≤5 questions, numbered, "could you" not "I need," and every question
is justified — the wrap-up sentence says what the answer will unblock. Note the posture: it asks
only when ambiguity is real (the base prompt says "whenever there is any ambiguity... use
available tools to get more context" — ask the TOOLS first, the user second).

### 1.6 The user's own context shapes the voice, and the model never has to notice

- Language is chosen IN CODE (`prompts/user_info.py:35-44` + `chat/prompt_utils.py:168-175`): one
  branch reads "The user's interface language is {language}. Reply in {language}. If the user
  explicitly asks for another language, use that one." — the other reads "Reply in the language
  the user writes in." The code picks; the model never has to distinguish.
- A `# User Information` block rides every system prompt (name/email/role, org profile, team,
  preferences, memories) — with the instruction to "Rely on it when the answer depends on the
  user's location or position."
- Memory text is stored name-free on purpose (`prompts/basic_memory.py:9`): "a memory like
  'Yuhong prefers dark mode.' should be modified to 'Prefers dark mode.'" — the memory is a FACT
  to render into the user block, not a quotation. This is the persona/context separation
  (the hammer's doctrine) implemented in a codebase: the voice never welds the user in.
- Author placeholders resolve in code (`{{user.name}}`-style): recognized keys substitute "",
  typos STAY LITERAL "so the agent author notices" — errors are visible, never silently absorbed.

### 1.7 Thinking is shown, narrated by the system, not performed by the voice

The loop streams reasoning as packets (`reasoning_start/delta/done`,
`server/query_and_chat/streaming_models.py:106-118`). Content typed BEFORE a forced tool call is
RECLASSIFIED as reasoning (`chat/llm_step.py:905-921`): "When tool_choice is REQUIRED, content
before tool calls is reasoning/thinking... Treat this content as reasoning instead of answer."
User-visible progress labels are UI strings ("Searching the web", "Researching"), never model
tokens. The deep-research orchestrator's `think` tool is piped INTO the visible reasoning channel
for models that lack native reasoning (`dr_loop.py:537-539`) — narration is plumbing, not
personality.

---

## Part 2 — How Onyx thinks about its prompts and problems

### 2.1 A bounded cycle budget with a forced answer

`chat/llm_loop.py:944-960` — the turn is `for llm_cycle_count in range(MAX_LLM_CYCLES)` (6, and
the comment defends the number: "covers the common search → open_url pattern... Cycle 6: No more
tools available, forced to answer"). Two laws:

- **When no tool calls arrive, the model has answered — wrap up** (loop exit by observed
  behavior, which SecretAgent also landed).
- **Stopping tools end the loop**: certain tools (image generation) set flags that force the NEXT
  cycle to `tool_choice = NONE` — the forced-answer cycle, with the last-cycle reminder quoted
  above ("You must answer the query now to the best of your ability").
- A regex fallback claws tool calls out of low-quality models' prose ONCE only ("to prevent the
  case of excessive looping with bad models") — recovery is bounded, never unbounded.

### 2.2 Prompt placement is believed to be a law, with numbers on it

`chat/README.md` — the design notes are explicit:

> Reminder messages are placed at the end of the prompt because all model fine tuning approaches
> cause the LLMs to attend very strongly to the tokens at the very back of the context closest to
> generation. This is the only way to get the LLMs to not miss critical information.

> The custom agent is inserted as a user message above the most recent user message, it is
> dynamically moved in the history as the user sends more messages. ... Having the Custom Agent
> instructions not move means it fades more as the chat gets long which is also not ok from a UX
> perspective.

And the measured law:

> having this in the Tool section of the System prompt makes all the LLMs follow it well but if
> it's even just a paragraph away... it is often ignored. The difference is as drastic as a 30%
> follow rate to a 90% follow rate by even just moving the same statement a few sentences.

The exact layout (`llm_loop.py:597-626`): `[system] → older history → custom-agent block
(movable) → context files → forgotten-files notice → last user message → newer messages →
reminder`. Instructions are not just written — they are PLACED: the movable block keeps
agent instructions near the generation point forever.

### 2.3 Eviction with a trail, never silent

The budget (`chat/token_budget.py`): subtract tools first, then the fixed sections, then truncate
the oldest history "from the top" until it fits — "This message and everything older is dropped."
What leaves the prompt leaves a TRACE: dropped files become a `forgotten-files` metadata message
telling the model exactly how to get them back ("You have access to the following files. Use the
read_file tool... You MUST pass the file_id UUID (not the filename)"), or, when nothing can read
them: "Do not guess their contents or search the web for them — say they are too large to read in
this conversation." Old TOOL RESPONSES are replaced with a stub: "This tool call completed but
the results are no longer accessible." The model is never left to imagine what it once had.

### 2.4 Deep research: plan → parallel delegate → think between → forced report

The planner prompt verbatim (`deep_research/orchestration_layer.py:34-50`):

> You are a research planner agent that generates the high level approach for deep research on a
> user query. Analyze the query carefully and break it down into main concepts and areas of
> exploration. Stick closely to the user query and stay on topic but be curious and avoid
> duplicate or overlapping exploration directions. Be sure to take into account the time
> sensitive aspects... Focus on providing thorough research of the user's query over being
> helpful.
>
> Do not worry about the feasibility of the plan or access to data or tools, a different deep
> research flow will handle that.

That last line exists because of an observed model failure — the code comment above it: "Sometimes
the model will just apologize and claim the task is not possible." The planner output is bounded
("a numbered list of steps and have 6 or less individual steps", each "standalone... but may build
on previous steps") and a reminder rides as a USER-type message because "specifically for some
models, it really struggles to not just answer the user."

The orchestrator runs at most 8 cycles (4 for reasoning models), 30-minute forced-report timeout,
with the termination conditions spelled out: report when "You have researched all of the relevant
topics" / "You have shifted away from the original research plan and believe that you are done" /
"You have all of the information needed" / "The last research cycle yielded minimal new
information and future cycles are unlikely to yield more information." Parallelism is capped
("NEVER call more than 3 [research_agent] in parallel") and delegation is context-free BY LAW:
"the research_agent only receives the task and has no additional context... You absolutely must
provide all of the context needed to complete the task in the argument" — children are sealed,
parents own the whole picture (the same shape SecretAgent's spawn/report follows).

Between calls it must think — and in PROSE: "use the think tool to reason between every call...
Be curious, identify knowledge gaps and consider new potential directions of research. Use
paragraph format, do not use bullet points or lists." The sub-agent's report faces ANOTHER AGENT,
so it inverts the user-facing style entirely: "The report will be seen by another agent instead of
a user so keep it free of formatting or commentary... It cannot be stressed enough that this
report must be EXTREMELY THOROUGH and COMPREHENSIVE." User-facing answer: "You get straight to
the point, never providing a title and avoiding lengthy introductions/preambles." Two audiences,
two registers, never mixed.

### 2.5 The search instructions sit exactly where compliance is highest

`prompts/tool_prompts.py:7-19` (this text lives in the `# Tools` prompt section — per §2.2's law):

> For questions that can be answered from existing knowledge, answer the user directly without
> using any tools. If you suspect your knowledge is outdated or for topics where things are
> rapidly changing, use search tools to get more context. For statements that may be describing or
> referring to a document, run a search for the document. In ambiguous cases, favor searching to
> get more context.
>
> When using any search type tool, do not make any assumptions and stay as faithful to the user's
> query as possible. Between internal and web search (if both are available), think about if the
> user's query is likely better answered by team internal sources or online web pages. When
> searching for information, if the initial results cannot fully answer the user's query, try again
> with different tools or arguments. Do not repeat the same or very similar queries if it already
> has been run in the chat history.
>
> If it is unclear which tool to use, consider using multiple in parallel to be efficient with
> time.

The decision tree: knowledge-first, staleness triggers search, ambiguity triggers search, retry
with DIFFERENT tools/arguments (never repeat), parallelize when unsure. Query REFORMULATION is its
own small LLM step with worked examples (`search_prompts.py:34-51`): insert chat context
("How do I set it up?" → "How do I set up software Y?"), strip non-searchable requests ("Can you
find me the document that goes over..." → "all of the software to set up on an engineer's first
day"), fill in the user ("What document did I write last week?" → "...John Doe wrote..."), strip
source scoping ("Search Google Drive for the SLA doc" → "SLA doc") because scoping is applied
automatically downstream — "CRITICAL: ONLY provide the standalone query and nothing else."
Expansion is skipped for repeat searches (`skip_search_query_expansion=has_called_search_tool`) —
rewrite once, then let the agent iterate.

### 2.6 Machine-internal prompts hug the training distribution

`prompts/search_prompts.py:1-12` — the stated philosophy:

> The system prompt is kept simple and as similar to typical system prompts as possible to stay
> within training distribution. The last message is dynamically created and has a detailed
> description of the actual task. This is based on the assumption that users give much more varied
> requests in their prompts.

Structure over cleverness: the system prompt is GENERIC, the task lives in the final user message
where variation belongs. Read-classification prompts are "intentionally lenient since there are
additional filters downstream" — precision is layered in code, the prompt just avoids
false-negatives; the anti-confusion rule is a worked example ("the document is related to the
query but not about the correct subject" — the ACME case).

### 2.7 What Onyx believes about models (the meta-principles)

Everything above reduces to five beliefs the codebase acts on:

1. **Placement is power** — instructions must live where attention concentrates (end of context,
   above the last user message); a paragraph of drift costs 30→90%.
2. **Reminders beat reasoning** — per-turn corrections arrive as system-reminders, not as clever
   wording in the base prompt; the model is TOLD what the reminder tags are.
3. **Bounded loops, forced answers** — cycles are budgeted, stopping tools force the answer, and
   the last-cycle reminder tells the model the constraint honestly.
4. **Two registers, never mixed** — user-facing prose is warm-terse (straight to the point,
   conversational questions, inline citations); agent-facing reports are exhaustive-formless
   (no formatting, maximally thorough).
5. **Say "don't have it" directly** — adjacent information is not an answer; over-refusal about
   FEASIBILITY is the opposite failure and gets specifically countered ("do not worry about
   feasibility").

---

## Part 3 — What SecretAgent already owns, and what this study feeds

The overlap is not accidental: the hammer's spec was distilled from this assistant's behavior, and
the loop's steal-list came from this study's earlier pass. What the codebase shows NOW:

- **Owned:** loop-exit-by-behavior (the forced-answer cycle maps to our turn-end reasons); the
  agent-instructions-near-the-user placement (the derive's first-block persona injection, the
  below-placement); the two-register rule (child reports are agent-facing; the canvas is
  user-facing — the ask card carries the question verbatim, the report carries the substance);
  memory-stored-name-free (the user-context record's third-person rule); bounded retries.
- **Worth taking later:** the `<system-reminder>` convention (turn-level corrections as tagged
  non-user messages — our `control.*` events are adjacent but not model-tagged yet); the
  forgotten-trail eviction (our budget table truncates at source but nothing tells the model
  HOW to get the truncated thing back); the ask craft's rules (≤5 questions, numbered,
  justify-the-ask) as guidance the persona can carry as checkable principles; the feasibility
  anti-refusal counter if the planner ever drives deep research.
