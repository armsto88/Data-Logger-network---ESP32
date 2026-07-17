# Agent Model Setup — Ollama Routing

This repo's custom agent stack is declared in `.github/agents/`. The `model:` field in each agent's frontmatter is a **hint to the Copilot model picker**, not a binding route. To actually stop the stack from falling back to Claude/GPT from your GitHub business-plan entitlement, the routing must be done outside the agent files.

> Read this once before changing models. The goal is: all chat traffic goes through your Ollama account, nothing quietly slips through to a Copilot-hosted model.

---

## 1. What is in the repo right now

`.github/agents/*.agent.md` now declare a `model:` fallback chain pointing only at Ollama `:cloud` models, with `minimax-m3:cloud` as the final fallback:

| Agent | Primary | Fallback 1 | Fallback 2 |
|---|---|---|---|
| `orchestrator` | `kimi-k2.6:cloud` | `nemotron-3-super:cloud` | `minimax-m3:cloud` |
| `Planner` | `nemotron-3-super:cloud` | `kimi-k2.6:cloud` | `minimax-m3:cloud` |
| `Explore Repo` | `qwen3.5:cloud` | `minimax-m3:cloud` | — |
| `Implement Change` | `glm-5.1:cloud` | `qwen3.5:cloud` | `minimax-m3:cloud` |
| `Review Changes` | `gemma4:31b-cloud` | `nemotron-3-super:cloud` | `minimax-m3:cloud` |

The former Positron role files were consolidated on 2026-07-17 into `docs/archive/LEGACY_AGENT_WORKFLOW_2026-05-24.md`. They are not invoked by the active workflow; current agent definitions live under `.github/agents/`.

---

## 2. What the `model:` field actually does

VS Code Copilot resolves `model:` against the **Copilot picker** — the list of model IDs your GitHub Copilot account exposes. Models that are not in that picker are ignored and the picker default is used.

Your `ollama list` currently shows only `:cloud` models:

```
nemotron-3-super:cloud
qwen3.5:cloud
glm-5.1:cloud
kimi-k2.6:cloud
minimax-m3:cloud
gemma4:31b-cloud
```

These are not in the Copilot picker. So the `model:` line on its own will **not** keep you off Claude/GPT — Copilot will fall through to your business-plan default.

The three ways to actually route through Ollama are below. Pick one.

---

## 3. Option A — Ollama VS Code extension (simplest, recommended)

This is the cleanest path. The Ollama extension registers Ollama as a chat model provider, and you can pick Ollama models in the Copilot model picker just like Claude or GPT.

1. Install [Ollama for Windows](https://ollama.com/download/windows) and confirm `ollama --version` works in PowerShell.
2. Sign in to Ollama and confirm `ollama list` returns your `:cloud` models.
3. Install the **Ollama** VS Code extension (`ollama.ollama`, publisher `ollama`) from the marketplace. This repo recommends it in [`.vscode/extensions.json`](../.vscode/extensions.json) — VS Code will offer to install it on next launch if you haven't already.
4. In VS Code, open Settings and search for `Ollama`. Set the default model to one of your `:cloud` models (e.g. `minimax-m3:cloud`).
5. In the Copilot chat, the model picker should now list your Ollama models. Pick one.
6. The `model:` hints in `.github/agents/*.agent.md` are now first-choice in the picker; if Ollama is the picker default they will resolve to Ollama models.

Verification: open a new chat, type `/agents`, pick `orchestrator`, send a one-line task. In the response, the model label should show an Ollama model name, not "Claude Sonnet" or "GPT".

> Note: this repo's `.vscode/extensions.json` lists `saoudrizwan.claude-dev` (Cline) as an **unwanted** recommendation. Cline can talk to Ollama, but it has its own Anthropic-first default and is easy to leave routed to a paid hosted model. If you intentionally want Cline + Ollama, remove it from `unwantedRecommendations` and configure Cline's API provider separately — see [Option C-prime](#9-c-line--ollama-alternative) below.

---

## 4. Option B — Continue extension (full IDE-level control)

Continue is an open-source VS Code AI extension that talks to Ollama directly. It can replace Copilot Chat for most workflows and exposes Ollama models as first-class.

1. Install Ollama for Windows (same as above).
2. Install the **Continue** extension (`Continue.continue`).
3. Open the Continue config (`~/.continue/config.json` on Windows: `%USERPROFILE%\.continue\config.json`) and add models:

   ```json
   {
     "models": [
       { "title": "Ollama: kimi-k2.6", "provider": "ollama", "model": "kimi-k2.6:cloud" },
       { "title": "Ollama: minimax-m3", "provider": "ollama", "model": "minimax-m3:cloud" }
     ]
   }
   ```

4. Use Continue's chat panel instead of Copilot Chat for the agent workflow. Continue does not read `.github/agents/*.agent.md`, so treat the agent files as documentation of the intended roles, and replicate the persona discipline in Continue's `config.json` (system prompts per "agent").

This option does not give you VS Code's `runSubagent` against the custom-agent files, but it gives you the most predictable Ollama-only behaviour.

---

## 5. Option C — `copilot-ollama-proxy` (keeps Copilot Chat as the UI)

A small proxy that translates Copilot's OpenAI/Anthropic-format requests into Ollama's API. The model picker still shows Claude/GPT, but every call is silently forwarded to Ollama. This is the only option where the `model:` field in `.github/agents/*.agent.md` is meaningful as a routing hint.

1. Install Ollama for Windows.
2. Install the proxy. There are several implementations; pick one and follow its README:
   - `bernardx0/copilot-ollama-proxy` (Node, simple)
   - `ddPn08/copilot-ollama-proxy` (more options, multiple model mapping)
3. Configure the proxy to map Copilot model IDs to your Ollama `:cloud` models. Example mapping:

   | Copilot model ID | Ollama target |
   |---|---|
   | `claude-sonnet-4` | `kimi-k2.6:cloud` |
   | `claude-opus-4` | `nemotron-3-super:cloud` |
   | `gpt-5` | `glm-5.1:cloud` |
   | `gpt-5-mini` | `qwen3.5:cloud` |

4. In VS Code, point Copilot at the proxy by setting the API base in `settings.json`:

   ```json
   {
     "github.copilot.advanced": {
       "debug.overrideChatAdapterUrl": "http://localhost:11434"
     }
   }
   ```

   (Exact key depends on proxy — use whatever it documents.)
5. Restart VS Code. The `model:` chains in the agent files will now resolve against the proxy, and Claude/GPT will never be hit.

> The Copilot business-plan entitlement is still signed in, but if every call is intercepted at the proxy the upstream is never reached. If you want belt-and-braces, sign out of the Copilot business plan and rely on the proxy alone.

---

## 6. Local vs cloud Ollama models

The models in your `ollama list` are all `:cloud`. They are hosted by Ollama, billed through your Ollama account, and do **not** run on your GPU. The `model:` field in this repo is configured for them.

If you later want true local execution, pull local quantisations and add them to the chains:

```powershell
ollama pull llama3.1:8b
ollama pull qwen2.5-coder:14b
ollama pull deepseek-r1:8b
ollama pull nomic-embed-text
```

Then update the `model:` lines in `.github/agents/*.agent.md` so the local models come first in the fallback chain (e.g. `model: ["qwen2.5-coder:14b", "qwen3.5:cloud", "minimax-m3:cloud"]`).

---

## 7. Sanity check after setup

After picking Option A, B, or C, run this checklist:

1. Open a new Copilot/Continue chat.
2. Confirm the model picker shows an Ollama model, not Claude or GPT.
3. Run `/agents` and pick `orchestrator`. Send: "Summarise the active agent stack."
4. Confirm the response header shows the Ollama model you expect.
5. If a subagent call still resolves to a hosted model, the `model:` field is being ignored — that is a Copilot build limitation, not a config bug. Switch to Option B (Continue) for predictable behaviour.

---

## 8. If a model name in the repo is wrong

The model strings in `.github/agents/*.agent.md` must match what `ollama list` returns exactly. To swap a model:

1. Edit the `model:` line in the affected `.agent.md` file.
2. Update the table in section 1 of this doc to match.
3. If you are on Option C (proxy), update the proxy's mapping table.

Do not edit the archived legacy files — they are reference material and the `ARCHIVED` placeholder is intentional.

---

## 9. C-line + Ollama alternative

If you prefer to use **Cline** (`saoudrizwan.claude-dev`) as your primary agent loop instead of the Copilot Chat custom-agent stack, you can route Cline to your local Ollama. Cline does not read `.github/agents/*.agent.md`, so treat those as a design document and replicate the per-agent persona discipline inside Cline's config.

1. Confirm Ollama is running locally and `ollama list` shows your `:cloud` models.
2. In VS Code, open the Cline panel and click the settings gear.
3. Set:
   - **API Provider**: `OLLAMA`
   - **Base URL**: `http://localhost:11434`
   - **Model ID**: one of your `:cloud` models (e.g. `kimi-k2.6:cloud`)
4. Save. Cline now resolves every prompt against Ollama, not Anthropic.
5. Optionally remove `saoudrizwan.claude-dev` from `unwantedRecommendations` in `.vscode/extensions.json` if you want it explicitly recommended to other contributors.

Verification: in Cline, send a one-line task. The response header should show the Ollama model name. Cline does not surface in the Copilot Chat `/agents` picker, so the `orchestrator`/`Planner`/etc. agents are dormant on this path — they only run if a Cline prompt is structured to do so.

> Cline and Copilot Chat custom agents are independent. You can keep both installed: Cline for free-form agent work on Ollama, Copilot Chat for the `.github/agents/*.agent.md` workflow (which requires Option A, B, or the proxy to be Ollama-routed).
