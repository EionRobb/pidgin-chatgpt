# ChatGPT & AI Multi-Provider Plugin for Pidgin

Chat with AI models directly inside Pidgin / libpurple, just like on the web!

Connect separate accounts for any provider:
- **OpenAI (ChatGPT)** (`gpt-4o`, `gpt-4o-mini`, `o1`, `o3-mini`, etc.)
- **Google Gemini** (`gemini-2.5-flash`, `gemini-1.5-pro`, etc.)
- **Anthropic Claude** (`claude-3-5-sonnet`, `claude-3-5-haiku`, `claude-3-7-sonnet`, etc.)
- **Custom / Local Endpoints** (Ollama, OpenRouter, LocalAI, vLLM, LM Studio, etc.)

## Features

- **Multi-Provider**: Add distinct accounts in Pidgin for ChatGPT, Gemini, Claude, or custom local models.
- **Dynamic Model Discovery**: Automatically downloads available models from the provider's API.
- **Model Selection UI**: Pick your model from a list dialog via `Accounts -> [Account] -> Select Model...` or switch models on the fly with `/model <name>`.
- **Conversational Context**: Retains natural back-and-forth conversation history during your chat session.
- **Markdown Rendering**: Formats code blocks, bold, italics, and lists for Pidgin's IM window.
- **Typing Indicators**: Displays when the bot is generating a response.

## In-Chat Commands

- `/models` — List all available models downloaded from your provider.
- `/model` — Show the currently selected model for the chat.
- `/model <name>` — Switch to a different model for this conversation.
- `/clear` or `/reset` — Clear conversation history to start a new chat thread.
- `/system <prompt>` — View or update the system prompt/instructions for this chat.
- `/history` — Show how many messages are in the current context.
- `/help` — Display command help.

## Setup

1. Add a new account in Pidgin and select protocol **ChatGPT**.
2. Set your provider under account options (**OpenAI**, **Google Gemini**, **Anthropic Claude**, or **Custom**).
3. Put your API key in the account password field or in the "API Key / Token" setting.
4. (Optional) For Ollama or custom endpoints, set the API Base URL (e.g. `http://localhost:11434/v1`).
5. Connect! A default AI buddy will appear on your buddy list ready to chat.

---

[Zawinski's Law](https://en.wikipedia.org/wiki/Jamie_Zawinski#Zawinski's_Law) says "Every program attempts to expand until it can read mail."

These days it should be "Every program attempts to expand until it can large language model."

## Endorsements

> (29/10/2024 4:40:43 pm) grim: EionRobb: i'd down vote this if i could :P
