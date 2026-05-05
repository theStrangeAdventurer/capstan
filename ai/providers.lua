return {
	deepseek = {
		api_key = os.getenv("DEEPSEEK_API_KEY"),
		endpoint = "https://api.deepseek.com/v1/chat/completions",
		model = "deepseek-chat",
	},
}
