# IRC screenshots

The images in `doc/images/irc-*.png` are real sessions, recorded and rendered by these scripts.

1. Run an IRC server on 127.0.0.1:6667, e.g. `docker run -d -p 127.0.0.1:6667:6667 inspircd/inspircd-docker`.
2. Start an instance with the example agents and this `[irc]` section, then `./shclaw -d`:

   ```ini
   [irc]
   server      = 127.0.0.1
   port        = 6667
   nick        = shclaw
   channel     = #shclaw
   channel_key = crabs
   owner       = govlog
   ```

3. Play a scenario as the owner and record the channel: `./irc_owner.py builder.json builder.jsonl`.
   Each step sends a line, then waits until the bot is quiet (`quiet`, `max`) or says `until`.
4. Render it: `./render_irc.py builder.jsonl "govlog@shclaw — weechat — #shclaw" builder.html`,
   then `./shot.sh builder.html raw.png 2000 1600` (headless Firefox) and cut out the window:
   `convert raw.png -alpha set -fuzz 3% -fill none -draw "color 0,0 floodfill" -trim +repage out.png`.
