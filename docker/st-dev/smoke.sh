#!/usr/bin/env bash
# Boot an rtc-freeswitch-dev image and assert the VoiceFabric module set loads.
set -euo pipefail

IMAGE="${1:?usage: smoke.sh <image>}"
CONTAINER="rtc-fs-dev-smoke-$$"

# Mirrors telephony-iac ansible/roles/freeswitch/defaults/main.yml fs_modules_*.
MODULES=(
  mod_console mod_logfile
  mod_xml_curl
  mod_json_cdr mod_event_socket
  mod_sofia mod_loopback
  mod_commands mod_dptools mod_expr mod_fifo mod_hash mod_httapi mod_dialplan_xml mod_conference
  mod_spandsp mod_g723_1 mod_g729 mod_b64
  mod_sndfile mod_native_file mod_local_stream mod_tone_stream
  mod_flite mod_azure_tts mod_azure_transcribe mod_deepgram_transcribe
  mod_say_en
  mod_lua
)
# These two have no source anywhere in this tree, so no .deb ever carries them.
EXPECTED_ABSENT=(mod_azure_tts mod_azure_transcribe)

cleanup() { docker rm -f "$CONTAINER" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker run -d --name "$CONTAINER" "$IMAGE" >/dev/null

ready=
for _ in $(seq 1 60); do
  if docker exec "$CONTAINER" fs_cli -x status 2>/dev/null | grep -q '^UP'; then ready=1; break; fi
  sleep 2
done
if [ -z "$ready" ]; then
  echo "FAIL: FreeSWITCH did not reach UP"
  docker logs "$CONTAINER" 2>&1 | tail -60
  exit 1
fi

docker exec "$CONTAINER" freeswitch -version
docker exec "$CONTAINER" fs_cli -x status

failed=()
absent=()
for m in "${MODULES[@]}"; do
  if ! docker exec "$CONTAINER" test -f "/usr/lib/freeswitch/mod/$m.so" 2>/dev/null; then
    absent+=("$m")
    continue
  fi
  out=$(docker exec "$CONTAINER" fs_cli -x "load $m" 2>&1 || true)
  case "$out" in
    *+OK*|*already\ loaded*) ;;
    *) failed+=("$m -> $out") ;;
  esac
done

unexpected=()
for m in "${absent[@]}"; do
  case " ${EXPECTED_ABSENT[*]} " in *" $m "*) ;; *) unexpected+=("$m") ;; esac
done

echo "modules loaded: $(( ${#MODULES[@]} - ${#absent[@]} - ${#failed[@]} ))/${#MODULES[@]}"
[ ${#absent[@]} -eq 0 ] || echo "absent from artifact: ${absent[*]}"

rc=0
if [ ${#unexpected[@]} -gt 0 ]; then echo "FAIL: unexpectedly absent: ${unexpected[*]}"; rc=1; fi
if [ ${#failed[@]} -gt 0 ]; then printf 'FAIL: module load failed: %s\n' "${failed[@]}"; rc=1; fi
[ $rc -eq 0 ] && echo "smoke OK"
exit $rc
