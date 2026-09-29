#!/bin/sh
# Live smoke test of an extracted release archive (run on the target).
# Usage: pipeline-live.sh <instance dir>
# The instance needs etc/config.ini, the agents jarvis, oracle and builder,
# and the plugins weather_city.c and currency_exchange.c. Prints LIVE lines;
# fails when oracle does not answer or a compiled plugin does not work. The
# builder's own test run is reported, not required: it depends on the model.
cd "$1" || exit 1
./shclaw -d >/dev/null || exit 1
i=0
until ./shclaw status >/dev/null 2>&1 || [ $i -ge 90 ]; do sleep 1; i=$((i+1)); done
LOG=$(ls logs/*.log | head -1)

ends() { grep -c "\[$1\] === END" "$LOG"; }

# ask <agent> <message>: send, then wait until that agent ends one more session
ask() {
    n=$(ends "$1")
    ./shclaw msg "$1" "$2" >/dev/null
    i=0
    while [ "$(ends "$1")" -le "$n" ] && [ $i -lt 180 ]; do sleep 2; i=$((i+1)); done
}

ask oracle "What is the capital of Australia? One line."
ask jarvis "Call the weather_city tool for Paris and the currency_exchange tool from EUR to USD, then give me both results."
ask builder "Create a plugin named my_ip that returns the public IP address from https://api.ipify.org (plain text). Call create_plugin with test_input {}."
./shclaw stop >/dev/null
sleep 2

has() { grep -q -- "$1" data/sessions/*.json 2>/dev/null; }
ok=0
if has Canberra; then echo "LIVE oracle: ok"; else echo "LIVE oracle: FAIL"; ok=1; fi
if has "°C"; then w=ok; else w=FAIL; ok=1; fi
if has "1 EUR = "; then c=ok; else c=FAIL; ok=1; fi
echo "LIVE plugins: weather_city $w, currency_exchange $c (HTTPS, 301)"
if has "-> 200]"; then b="test run got HTTP 200"
elif has "compiled and loaded"; then
    b="plugin built, no successful test run"
    t=$(grep -ho '\[GET [^]]*\]' data/sessions/*.json | tail -1)
    [ -n "$t" ] && b="$b, last call $t"
    has "invalid value for 'test_input'" && b="$b, the model sent test_input as an object"
else b="no plugin built"; fi
echo "LIVE builder: $b"
echo "LIVE cache: $(grep -c 'from cache' "$LOG") calls, $(grep -Ec '\(([1-9][0-9]*) from cache' "$LOG") with cached tokens"
exit $ok
