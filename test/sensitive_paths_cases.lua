-- Shared native/fallback policy fixtures. These contain names, never secrets.
return function(classify)
    for _, path in ipairs({"agent/tokens.lua", "test/test_tokens.c", "src/tokenizer.py",
        "design_tokens.ts", "Tokens.HPP", "docs/token-counting.md", "src/usage.c",
        "tokenize.rs", "token.go", "tokens.lua/ordinary.txt"}) do
        assert(not classify(path), "false positive: " .. path)
    end
    for _, path in ipairs({".env", ".env.lua", ".ENV.local", "secrets.lua", "client_secret.ts",
        "credentials.py", "credentials.json", "tokens.json", "access_token.txt", "refresh_token",
        "tokens.lua.bak", "tokens.pem", "TOKEN.KEY", "tokens.yaml", "token", "tokens"}) do
        assert(classify(path), "missing protection: " .. path)
    end
end
