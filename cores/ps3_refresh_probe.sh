#!/usr/bin/env bash
set -euo pipefail

# Boot the real Refresh backend in a disposable loopback-only namespace.
# PostgreSQL is supplied from an extracted Ubuntu 22.04 package tree.
if [[ $# -ne 2 ]]; then
  echo "Usage: bash $0 <Refresh_for_Linux_x64.zip> <extracted PostgreSQL 14 root>" >&2
  exit 2
fi
archive=$(realpath -- "$1")
postgres=$(realpath -- "$2")
pin=972d7fd30e4a0f656ed63f9a57f5b28fe8bc7e1e6d4b821f7909d64322efdb5e
matches_archive() {
  [[ $(sha256sum < "$1") == "$pin  -" ]]
}
if ! matches_archive "$archive"; then
  echo "The archive did not match the pinned Refresh v3.7.2 Linux x64 release." >&2
  exit 2
fi
pgbin="$postgres/usr/lib/postgresql/14/bin"
for program in initdb pg_ctl createdb; do
  if [[ ! -x "$pgbin/$program" ]]; then
    echo "Missing PostgreSQL fixture tool: $pgbin/$program" >&2
    exit 2
  fi
done
if [[ ${ANYPS5_REFRESH_PROBE_INNER:-} != 1 ]]; then
  export ANYPS5_REFRESH_PROBE_INNER=1
  exec unshare --map-current-user --net --keep-caps bash "$(realpath -- "$0")" "$archive" "$postgres"
fi
if [[ $(ip -j link | python3 -c 'import json,sys; print(" ".join(i["ifname"] for i in json.load(sys.stdin)))') != lo ]]; then
  echo "Refresh qualification requires a loopback-only network namespace." >&2
  exit 2
fi
ip link set lo up

root=$(mktemp -d /tmp/anyps5-refresh-probe-XXXXXX)
echo "Evidence: $root"
cd "$root"
export LD_LIBRARY_PATH="$postgres/usr/lib/x86_64-linux-gnu"
export BUNKUM_DATA_FOLDER="$root/data" DOTNET_EnableDiagnostics=0
mkdir release data
mkdir -m 700 pg-socket
cp -- "$archive" verified-refresh.zip
if ! matches_archive verified-refresh.zip; then
  echo "The copied Refresh archive failed verification." >&2
  exit 2
fi
python3 -m zipfile -e verified-refresh.zip release
chmod +x release/Refresh.GameServer
cat > data/bunkum.json <<'JSON'
{"ListenHost":"127.0.0.1","ListenPort":10061,"ExternalUrl":"http://127.0.0.1:10061"}
JSON
cat > data/refreshGameServer.json <<'JSON'
{
  "RegistrationEnabled": false,
  "PermitAllLogins": false,
  "UseTicketVerification": true,
  "EnforcePatchwork": true,
  "PrintAuthenticationData": false,
  "WebExternalUrl": "http://127.0.0.1:10061",
  "GameConfigStorageUrl": "http://127.0.0.1:10061/lbp"
}
JSON
cat > data/db.json <<'JSON'
{
  "ConnectionString": "Database=refresh;Username=refresh_fixture;Host=127.0.0.1;Port=15432",
  "PreferConnectionStringEnvironmentVariable": false
}
JSON

"$pgbin/initdb" -D "$root/pg-data" -L "$postgres/usr/share/postgresql/14" \
  -U refresh_fixture -A trust --no-locale -E UTF8 > initdb.log
refresh_pid=
cleanup() {
  if [[ -n "$refresh_pid" ]]; then
    if jobs -p | grep -Fxq "$refresh_pid"; then
      kill "$refresh_pid" 2>/dev/null || true
      for (( check = 0; check < 40; check++ )); do
        if ! jobs -p | grep -Fxq "$refresh_pid"; then break; fi
        sleep .1
      done
      if jobs -p | grep -Fxq "$refresh_pid"; then
        kill -KILL "$refresh_pid" 2>/dev/null || true
      fi
    fi
    wait "$refresh_pid" 2>/dev/null || true
  fi
  if "$pgbin/pg_ctl" -D "$root/pg-data" status >/dev/null; then
    "$pgbin/pg_ctl" -D "$root/pg-data" -m fast -w stop
  fi
  ss -H -ltunp > listeners-after.txt
  if [[ -s listeners-after.txt ]]; then
    echo "A qualification listener remained after shutdown." >&2
    exit 1
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$pgbin/pg_ctl" -D "$root/pg-data" -l postgres.log \
  -o "-h 127.0.0.1 -p 15432 -k $root/pg-socket" -w start
"$pgbin/createdb" -h 127.0.0.1 -p 15432 -U refresh_fixture refresh
nice -n 19 ./release/Refresh.GameServer > refresh.log 2>&1 &
refresh_pid=$!
echo "Refresh PID: $refresh_pid"

base=http://127.0.0.1:10061
for (( attempt = 0; attempt < 80; attempt++ )); do
  if ! kill -0 "$refresh_pid" 2>/dev/null; then
    echo "Refresh stopped during startup. See $root/refresh.log." >&2
    exit 1
  fi
  if curl --noproxy '*' -fsS --max-time 2 "$base/_health" -o health.json 2>/dev/null; then
    break
  fi
  sleep .25
done
python3 -c 'import json,sys; sys.exit(json.load(open("health.json"))["StatusType"] != "Healthy")'
ss -H -ltunp | tee listeners.txt
awk '$1 != "tcp" || $5 !~ /^127[.]0[.]0[.]1:(10061|15432)$/ {bad=1}
     END {exit (bad || NR != 2)}' listeners.txt
grep -q "pid=$refresh_pid," listeners.txt
status=$(curl --noproxy '*' -sS --max-time 3 -o login-response.txt -w '%{http_code}' \
  --data-binary 'fixture' "$base/lbp/login")
test "$status" = 403
echo "Refresh reported Healthy and denied the synthetic game login with HTTP $status."
