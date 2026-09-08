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
    profile = "implement", -- Built-in or ~/.config/capstan/profiles/*.lua name
    system_prompt_append = nil, -- String or ordered array of additional instructions
    profiles = {
      -- Patch a file-defined profile without replacing unspecified fields:
      -- implement = { prompt_append = "Always run focused tests." },
    },
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

  -- Native OTel is opt-in; OTEL_* settings alone never enable it.
  -- OTEL_SDK_DISABLED=true forces it off even when enabled = true.
  -- See specs/config.md and examples/otel-collector.yaml.
  observability = {
    enabled = false, -- Set true to export traces and lifecycle logs (not raw logs)
    endpoint = "http://127.0.0.1:4318", -- Local Collector; recommend HTTPS remotely
    protocol = "http/protobuf", -- Only supported protocol
    traces_exporter = "otlp", -- Or "none"
    logs_exporter = "otlp", -- Or "none"
    service_name = "capstan",
    -- service_version defaults to the compiled app version.
    headers = {}, -- String map, e.g. { ["x-tenant"] = "example" }; no credentials here
    resource_attributes = {}, -- Explicit export data; no session IDs or secrets
    -- traces_endpoint / logs_endpoint are full URLs, not base URLs.
    -- traces_headers / logs_headers replace headers rather than merge.
    -- Precedence: signal env > generic env > signal config > generic config.
    -- Keep transport credentials in OTEL_EXPORTER_OTLP_HEADERS, not this file.
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
