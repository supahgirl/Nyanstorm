# Nyanstorm — Firestorm viewer fork

Firestorm fork with AI agent chat, Discord integration, AO enhancements, avatar height detection, gender-based avatar coloring, and RLV (nostrip) link protection. Based on Firestorm 7.2.4.

---

### AI Agent Chat (LLM Integration)

Two built-in AI agent sessions let you chat with large language models directly from the IM window. The viewer connects directly to your LLM API endpoint — no intermediate server required.

**Supported providers:** Ollama, llama.cpp, OpenRouter, and custom OpenAI-compatible endpoints. Ollama and llama.cpp are auto-configured with their default local ports and OpenAI-compatible `/v1/chat/completions` streaming format — no API key needed.

#### Getting started

1. **Run your LLM backend** — start Ollama, llama.cpp server, or use a cloud provider like OpenRouter.
2. **Open the IM window** — two AI Agent sessions appear in your contacts list and tab bar (Agent 1 and Agent 2). They work like regular IM conversations but everything stays local — nothing is sent to Second Life.
3. **Configure your LLM provider** — click the **AI** button in the IM session toolbar, then **Configure > LLM**. This opens the *Model Configurations* panel where you manage your LLM backends.

#### Model Configurations panel (AI button → Configure → LLM)

This is where you tell the viewer how to reach your LLM. It holds a list of provider entries, each with its own model name, endpoint URL, and optional API key. All entries are saved and persist across restarts.

- **Provider list** (top) — shows all your configured providers in a table with columns for type, model name, and endpoint. Select a row to edit it below.
- **+ / Trash / Apply buttons** — add a new provider, delete the selected one, or apply the current selection as the active model.
- **Provider dropdown** — choose the provider type:
  - `ollama` — auto-fills `http://localhost:11434/v1/chat/completions`, no API key needed
  - `llamacpp` — auto-fills `http://localhost:8080/v1/chat/completions`, no API key needed
  - `openrouter` — auto-fills `https://openrouter.ai/api/v1/chat/completions`, API key required
  - `custom` — blank fields, configure any OpenAI-compatible endpoint
- **Model** — the model name as the API expects it (e.g. `llama3`, `mistral`, `gpt-4o`).
- **Endpoint** — the full chat completions URL. Auto-filled when you select a provider type, but you can edit it for custom ports or remote servers.
- **API Key** — optional. Only enabled for openrouter and custom providers. The field is masked.
- **Max chars** — context length in characters (1000–100000). Controls how much conversation history is sent with each request.
- **Save / Close** — Save writes the current list to persistent settings. Close discards unsaved changes.

#### Agent configuration (AI button → Configure → Agent)

Each of the two AI agents has its own persona and instructions. This panel lets you define who the agent is and how it should behave.

- **Agent dropdown** — switch between Agent 1 and Agent 2. Each has independent settings and conversation history.
- **Config name** — a label for this configuration preset (e.g. "Assistant", "Roleplay GM").
- **Persona** — the system prompt that defines the agent's personality, tone, and role. Up to 4096 characters. Some users may be tempted to wrap content in XML-style tags (`<persona>…</persona>`) or use Markdown formatting, but testing shows modern LLMs are smart enough that such markup is not required.
- **Instructions** — additional rules, constraints, or behavioral guidelines. Up to 8192 characters.
- **Load / Save** — import or export configuration presets as files, so you can share or back up agent definitions.
- **OK** — apply the current settings and close. Settings persist across viewer restarts.

#### Chatting with an AI agent

Each agent behaves like a chatbot inside its own IM session. Open Agent 1 or Agent 2 from your contacts list and type to it just like you would with any other contact — the agent replies directly in the conversation.

- **Streaming** — the agent's response appears token-by-token with a typing indicator. The IM window flashes when the response completes.
- **Markdown** — headings, bold, italic, and code blocks are rendered in chat.

#### Notecard context discussion

Open any notecard and select an AI agent from the dropdown at the bottom. The notecard's full contents are injected as context into the conversation, allowing the LLM to discuss, summarize, translate, or answer questions about the document. Useful for analyzing notecards, brainstorming based on reference material, or having the agent assist with document content.

#### Chat redirection

The **→** button next to the AI button in the IM toolbar redirects every message from the current conversation to the selected AI agent (Agent 1 or Agent 2). This works with any chat — direct IM, group chat, or Discord — forwarding each incoming message to the agent for processing or reply suggestions.

#### Conversation management (AI button → Conversations)

- **Save / Restore** — save the current conversation to a file and restore it later. Conversations survive viewer restarts.
- **History** — browse past conversation logs.
- **Reset context** — clear the conversation history so the agent starts fresh.

### Discord Integration

A built-in Discord chat bridge replaces the old external Python relay. It uses the Discord Gateway API directly from C++.

**Features:**
- **Chat bridge** — send and receive Discord channel messages from within Firestorm IM windows, using a built-in C++ Gateway (no external Python relay needed)
- **Contacts tab** — browse Discord friends and servers in the Contacts floater
- **Typing indicators** — see when Discord users are typing, and vice versa
- **Status menu** — set your Discord status from the main menu bar: **Discord > Online / Inactive / Invisible**
- **Channel messages with author names** — messages from Discord channels show the author's display name

**How to use:**
1. Create a Discord bot at https://discord.com/developers/applications and copy its token
2. Open the Debug Settings floater (**Ctrl+Shift+D**) and set `FSDiscordBotToken` to your bot token
3. Restart the viewer — the Gateway connects automatically on login
4. Discord contacts appear in the Contacts floater under the Discord tab

### Animation Overrider Enhancements

The built-in client-side AO panel gets several quality-of-life improvements:

- **Alpha/Num Sort** — sort animations alphabetically within a state
- **Date Sort** — sort animations by inventory acquisition date (newest first)
- **Local Preview** — preview animations locally without changing them in-world; uncheck to promote the preview to your active animation. Previewed animations are highlighted in **red**, while the currently playing in-world animation is highlighted in **green** with a bold font.
- Sort order is never persisted to the notecard — manual reordering (Move Up/Down) is the only way to change the stored order

All controls are in the full AO panel (click the wrench icon to expand from the compact view).

### Height Detection in Nearby People Panel

Avatars in the Nearby radar list show a **Height** column displaying their avatar height in centimetres, calculated from the avatar's body mesh size.

- The column is toggleable via the radar options menu (right-click the column header)
- Height is displayed as a rounded integer in cm (e.g. `172`)
- Shows `?` when the avatar mesh is not yet loaded or out of draw distance

### RLV (nostrip) Protection via Links

The standard RLV `(nostrip)` folder protection is extended to work correctly with inventory links.

**The problem:** When an attachment is worn, the server resolves any inventory link to the actual item UUID. RLV `@remattach:<UUID>=force` commands use this resolved UUID, so the viewer's nostrip check looked at the actual item's parent folder — never at the link inside `#RLV/BASE (nostrip)/`. This meant a link in a `(nostrip)` folder provided no protection against force-detach commands.

**The fix:** The viewer now scans the `#RLV` shared folder tree for any link pointing to the item being stripped. If a matching link is found inside a folder whose name contains `(nostrip)`, the item is treated as protected and the force-detach is blocked.

**How to use:**
1. Create a folder named anything ending in `(nostrip)` inside `#RLV` (e.g. `#RLV/BASE (nostrip)`)
2. Place **links** to your worn attachments inside that folder
3. RLV `@remattach=force` and `@detach=force` commands will no longer be able to remove those attachments

This matches the expected behaviour described in the RLV specification.

### Gender-Based Avatar Coloring

Avatars in the Radar, Minimap, and Nearby list are color-coded by detected gender:
- **Pink** for female avatars
- **Orange** for male avatars

Toggle this in the Debug Settings: `FSColorAvatarsByGender` (Boolean). Contact-set colors take precedence when enabled.

---

## Debug Settings Reference

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `FSAIAgentName1` | String | *empty* | Display name for AI Agent 1 |
| `FSAIAgentName2` | String | *empty* | Display name for AI Agent 2 |
| `FSAIContextSizeK` | S32 | `0` | LLM context size in kilo-tokens (0 = model default) |
| `FSModelProviderConfigs` | LLSD | *empty* | Saved provider list (managed via **Configure > LLM**) |
| `FSDiscordBotToken` | String | *empty* | Discord bot token for the built-in chat Gateway |
| `FSEnableDiscordIntegration` | Boolean | — | Per-account toggle for Discord Gateway (set automatically) |
| `FSColorAvatarsByGender` | Boolean | `1` | Color avatars by gender in Radar/Minimap/Nearby |

> Open the Debug Settings floater with **Ctrl+Shift+D** to view or edit any setting.
