import lib_events
e = lib_events.all_episodes()
e.to_csv("episodes.csv", index=False)
print(len(e))
print(e.head(3).to_string())
