# pi-lite

`pi-lite` is a tiny coding agent inspired by `badlogic/pi-mono` except it got a long-term local memory inspired from mem0ai/mem0.

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/mouadse/pi-lite)

It keeps only the learning-sized core:

- an OpenAI-compatible chat completions client
- SSE streaming responses
- lightweight ANSI Markdown rendering for terminal output
- a small in-memory agent loop
- tool calling
- workspace-safe file tools
- capped tool output
- lightweight context compaction
- optional local long-term memory
- approximate token/cost stats
- optional guarded shell execution

It intentionally omits the larger `pi-mono` systems: TUI, extensions, skills, session trees, model catalog, web UI, and Slack bot.

## Build

Requirements:

- C++20 compiler
- CMake
- libcurl
- SQLite
- nlohmann-json
- `patch` command for the `apply_patch` tool

```bash
cmake -S . -B build
cmake --build build
```

## Configure

The binary reads real environment variables first, then `.env` in the workspace.

OpenRouter example:

```bash
OPENROUTER_API_KEY=...
PI_LITE_BASE_URL=https://openrouter.ai/api/v1
PI_LITE_MODEL=deepseek/deepseek-v4-flash
```

OpenAI example:

```bash
OPENAI_API_KEY=...
PI_LITE_MODEL=gpt-4.1-mini
```

Local OpenAI-compatible server example:

```bash
PI_LITE_BASE_URL=http://localhost:11434/v1
PI_LITE_MODEL=qwen2.5-coder
```

Enable local long-term memory when you want it:

```bash
PI_LITE_MEMORY=1
PI_LITE_MEMORY_USER_ID=mouad
```

By default, memory is stored in `.pi-lite/memory.sqlite3` in the workspace. The store is fully local: SQLite rows, deterministic hashed embeddings, and in-process cosine search.

Automatic memory capture is separate and opt-in:

```bash
./build/pi-lite --memory --memory-auto-capture
```

Without `--memory-auto-capture`, the model can still save durable facts with the memory tools.

## Run

One-shot prompt:

```bash
./build/pi-lite "explain this project"
```

Interactive mode:

```bash
./build/pi-lite
```

Interactive shortcuts:

- Type `/` at the start of the prompt to open slash command autocomplete
- `↑` / `↓`: move through autocomplete suggestions
- `Tab`: insert the selected autocomplete suggestion
- `Enter`: submit, or insert+submit the selected slash command when suggestions are open
- `Esc`: close autocomplete suggestions
- `Ctrl+L`: clear the screen and redraw the current prompt
- `Ctrl+C`: clear the current input line
- `Ctrl+D`: exit when the input line is empty
- `/clear`: clear the screen
- `/reset`: clear conversation history
- `/history`: print the stored conversation/tool history
- `/edit`: open `$EDITOR` for a multi-line prompt
- `/memory list`: print stored long-term memories
- `/memory search <query>`: search long-term memories
- `/memory delete <id>`: delete a memory
- `/help`: show interactive commands
- `!<command>`: run a local shell command directly, without asking the LLM

Local shell shortcuts always require the `!` prefix. Bare inputs like `ls` or `pwd` are sent to the agent as normal prompts. Local shell shortcuts are separate from the model-facing `bash` tool; the model still cannot run shell commands unless `--allow-bash` is provided.

Enable shell execution only when you want it:

```bash
./build/pi-lite --allow-bash "run the tests and fix failures"
```

Pipe stdin as a one-shot prompt:

```bash
echo "explain this project" | ./build/pi-lite
```

You can combine a CLI prompt with piped stdin:

```bash
git diff | ./build/pi-lite "review this diff"
```

Run a local smoke test without calling an LLM:

```bash
./build/pi-lite --self-test
```

## Tools

The agent exposes these tools to the model:

- `read_file`: line-numbered file reads with `offset` and `limit`
- `write_file`: create or overwrite a non-secret workspace file
- `list_files`: directory listing, git-aware when possible
- `grep_files`: recursive regex search over text files, gitignore-aware when possible
- `apply_patch`: standard unified diff application through `patch`
- `bash`: shell execution, disabled unless `--allow-bash` is provided
- `memory_save`: save a durable, non-secret long-term memory when memory is enabled
- `memory_search`: search long-term memory
- `memory_list`: list recent long-term memories
- `memory_update`: update a memory by ID
- `memory_delete`: delete a memory by ID

Tool output is capped to roughly 50 KB or 2000 lines, following the same idea used by `pi-coding-agent` to avoid flooding model context. Long-lived chats also compact older turns and oversized tool outputs when the approximate context budget is exceeded. Use `--max-context-tokens 0` to disable compaction.

Secret-like files such as `.env`, credentials files, and SSH keys are blocked from `read_file` and `write_file`, skipped by `grep_files`, and rejected by `apply_patch`. The runtime can still load `.env` for configuration.

## Memory

Memory is disabled by default. Enable it with `--memory` or `PI_LITE_MEMORY=1`.

Useful options:

```bash
./build/pi-lite --memory \
  --memory-user-id mouad \
  --memory-agent-id pi-lite \
  --memory-path .pi-lite/memory.sqlite3
```

Environment variables:

```bash
PI_LITE_MEMORY=1
PI_LITE_MEMORY_PATH=.pi-lite/memory.sqlite3
PI_LITE_MEMORY_USER_ID=mouad
PI_LITE_MEMORY_AGENT_ID=pi-lite
PI_LITE_MEMORY_RUN_ID=session-1
PI_LITE_MEMORY_AUTO_CAPTURE=0
```

When memory is enabled, `pi-lite` searches relevant memories before each model call and injects a short `Relevant long-term memory` section into the system prompt.

Auto-capture follows mem0's 80/20 shape: extract durable facts with the configured chat model, reject unsafe or noisy facts, dedupe locally, then persist facts with categories and timestamps. It never stores raw tool output automatically.

## Streaming UX

`pi-lite` sends `stream: true` to the OpenAI-compatible chat completions API.

While waiting for the model, interactive terminals show an animated status line with elapsed time and the selected model. It also sends an OSC `9;4` terminal progress sequence, which can light up supported terminal tab bars.

Captured or piped output gets a simple status line instead:

```text
[thinking] deepseek/deepseek-v4-flash...
```

When text deltas arrive, the status is cleared and assistant output streams to stdout. Tool progress is written to stderr so response text remains clean if stdout is redirected. After each model call, pi-lite prints approximate token stats and a cost estimate for known OpenAI mini/nano models.

Tool status uses state badges in interactive terminals:

```text
[tool] read_file running
[ok] read_file 1ms
```

For streamed tool calls, `pi-lite` accumulates function names and JSON argument fragments by tool-call index. It executes tools only after the assistant stream finishes and the final arguments can be parsed.

## Terminal Markdown

LLMs usually answer in Markdown. Terminals do not render Markdown by themselves, so `pi-lite` includes a tiny line-buffered renderer inspired by `pi-mono`'s TUI Markdown component and semantic theme tokens.

It handles the common response shapes:

- gold headings
- teal list bullets and ordered-list numbers
- bold and italic inline spans
- highlighted inline code
- bordered fenced code blocks
- muted blockquotes and simple tables

When stdout is an interactive terminal, these elements get ANSI truecolor and emphasis. When output is piped or captured, Markdown markers are still cleaned up without ANSI escape codes. Set `NO_COLOR=1` to disable styling.

## Architecture

The core loop is deliberately small:

```cpp
messages.push_back(user_message(prompt));

while (true) {
  auto assistant = llm.complete(system_prompt, messages, tools.schemas(), on_text_delta);
  messages.push_back(assistant);

  if (assistant.tool_calls.empty()) break;

  for (const auto& call : assistant.tool_calls) {
    auto result = tools.execute(call.name, call.arguments);
    messages.push_back(tool_message(call, result.content, result.is_error));
  }
}
```

That is the useful educational center of a coding agent: LLM response, tool execution, tool result, repeat.
