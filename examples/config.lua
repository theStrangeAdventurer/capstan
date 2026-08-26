-- Copy this file to ~/.config/capstan/config.lua and adjust it as needed.
-- Keep credentials in environment variables; never commit API keys here.

return {
  -- Built-in providers: "deepseek" and "openrouter".
  provider = "deepseek",

  providers = {
    -- These entries override Capstan's built-in provider defaults.
    deepseek = {
      api_key = os.getenv("DEEPSEEK_API_KEY"),
      model = "deepseek-chat",
    },
    openrouter = {
      api_key = os.getenv("OPENROUTER_API_KEY"),
      model = "anthropic/claude-sonnet-4",
    },

    -- Example local OpenAI-compatible provider. Select it by changing
    -- provider above to "ollama" or setting CAPSTAN_PROVIDER=ollama.
    ollama = {
      endpoint = "http://127.0.0.1:11434/v1/chat/completions",
      models_endpoint = "http://127.0.0.1:11434/v1/models",
      model = "gemma4:latest",
      context_limit = 32768,
    },
  },

  agent = {
    profile = "implement", -- "fast", "implement", or "plan"
    reasoning_effort = "medium",
    max_turns = 80,
    max_duration_sec = 2700,
    stream_timeout_sec = 300,
    max_stream_retries = 1,
    completion_review = true,
    auto_compact_percent = 80, -- set to 0 to disable automatic compaction
  },

  capabilities = {
    subagents = true,
    self_improvement = false, -- enable only if Capstan may create user extensions
  },

  subagents = {
    max_concurrent = 3,
    max_concurrent_cap = 8,
    max_tasks = 8,
    max_attempts = 3,
    max_turns = 6,
    max_turns_cap = 200,
    max_result_bytes = 16384,
  },

  tool_output = {
    max_bytes = 50 * 1024,
    max_lines = 2000,
  },

  -- Later matching rules win. Replace ~/code/my-project with your workspace.
  permissions = {
    { tool = "shell",     pattern = "~/code/my-project/*", allow = true },
    { tool = "file_read", pattern = "~/code/my-project/*", allow = true },
    { tool = "file_write", pattern = "~/code/my-project/*", allow = true },

    -- Keep sensitive files blocked even when --yolo is used.
    { tool = "file_read",  pattern = "*/.env*", allow = false },
    { tool = "file_write", pattern = "*/.env*", allow = false },
  },

  finder = {
    ignore_files = { ".gitignore", ".ignore" },
    ignore_patterns = { "vendor/**", "build/**", "*.o" },
  },

  wiki = {
    path = "~/.local/state/capstan/wiki",
  },

  -- MCP is opt-in. Add trusted servers here, then set enabled = true.
  mcp = {
    enabled = false,
    servers = {
      {
        name = "browser",
        enabled = false,
        transport = "stdio",
        command = "npx",
        args = { "-y", "@playwright/mcp", "--headless" },
        timeout = 30000,
      },
    },
  },
}
