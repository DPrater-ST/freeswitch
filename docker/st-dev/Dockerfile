# Local-dev FreeSWITCH image, built from the .deb bundle produced by
# .github/workflows/build-freeswitch-deb.yml. Base matches the VoiceFabric AMI.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

COPY debs/ /tmp/fs-debs/

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates tzdata; \
    cd /tmp/fs-debs; \
    sha256sum -c SHA256SUMS; \
    # Package selection mirrors telephony-iac roles/freeswitch/tasks/install.yml.
    debs="$(ls *.deb | grep -vE '^freeswitch-all_|^freeswitch-meta-|^freeswitch-conf-|^freeswitch-mod-hiredis_|^freeswitch-mod-vlc_|-dbg_' | sed 's,^,./,')"; \
    # The AMI's one omission we need back: it makes the image boot standalone.
    debs="$debs $(ls ./freeswitch-conf-vanilla_*.deb)"; \
    apt-get install -y --no-install-recommends $debs; \
    [ -f /etc/freeswitch/freeswitch.xml ] || cp -a /usr/share/freeswitch/conf/vanilla/. /etc/freeswitch/; \
    cfg=/etc/freeswitch/autoload_configs; \
    # Vanilla binds ESL to "::", which leaves 8021 unbound on an IPv6-less bridge.
    sed -i 's|"listen-ip" value="::"|"listen-ip" value="0.0.0.0"|' "$cfg/event_socket.conf.xml"; \
    # Vanilla also autoloads modules this build's avoid_mods excludes.
    for m in $(grep -v '<!--' "$cfg/modules.conf.xml" | sed -n 's/.*<load module="\([^"]*\)".*/\1/p'); do \
      [ -f "/usr/lib/freeswitch/mod/$m.so" ] || sed -i "/<load module=\"$m\"\/>/d" "$cfg/modules.conf.xml"; \
    done; \
    chown -R freeswitch:freeswitch /etc/freeswitch /var/lib/freeswitch /var/log/freeswitch /var/run/freeswitch; \
    rm -rf /tmp/fs-debs /var/lib/apt/lists/*

EXPOSE 5060/udp 5060/tcp 5080/udp 5080/tcp 8021/tcp 16384-32768/udp

HEALTHCHECK --interval=15s --timeout=5s --start-period=30s \
    CMD fs_cli -x status | grep -q '^UP' || exit 1

CMD ["freeswitch", "-u", "freeswitch", "-g", "freeswitch", "-nonat", "-nf", "-c"]
