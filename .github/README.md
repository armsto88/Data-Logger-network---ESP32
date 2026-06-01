# Active Agent Workflow

`.github/` contains the active repo-scoped VS Code/Copilot workflow for this project.

- User-facing entrypoint: `orchestrator`
- Subagents: `Planner`
- Subagents: `Explore Repo`
- Subagents: `Implement Change`
- Subagents: `Review Changes`
- Skills: `mothership-planning`
- Skills: `easyeda-outline`
- Skills: `firmware-workflow`
- Skills: `platformio-validation`
- Skills: `espnow-sync-debug`
- Skills: `esp32-bringup-debug`

## Model Routing

All active agents declare an Ollama-only `model:` fallback chain (`:cloud` models). To actually keep the stack off Claude/GPT, follow [docs/AGENT_MODEL_SETUP.md](../docs/AGENT_MODEL_SETUP.md) — pick one of the three routing options (Ollama extension, Continue, or `copilot-ollama-proxy`) before assuming the frontmatter is doing the work.

Legacy Positron-era reference files are archived under `docs/legacy-agent-workflow/`, not in the active stack.