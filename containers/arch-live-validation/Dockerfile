FROM archlinux:latest

LABEL org.opencontainers.image.title="Moguet Arch live provider validation" \
      org.opencontainers.image.source="https://github.com/seekerkrt/moguet" \
      org.opencontainers.image.base.name="docker.io/library/archlinux:latest"

# base-devel supplies the C++ toolchain, GNU Make, makepkg, gettext, and
# pkgconf.  The fixture's reviewed providers are deliberately absent so its
# virtual dependency remains unresolved until production Moguet discovers the
# current repository candidates at runtime.
RUN pacman-key --init \
    && pacman-key --populate archlinux \
    && pacman -Syu --needed --noconfirm \
        base-devel \
        cmake \
        curl \
        git \
        libarchive \
        nano \
        nlohmann-json \
        pacman \
        python \
        sudo \
        tomlplusplus \
        util-linux \
    && rm -f /var/cache/pacman/pkg/*

ARG VALIDATION_UID=1000
ARG VALIDATION_GID=1000

RUN groupadd --gid "${VALIDATION_GID}" moguet-validation \
    && useradd \
        --uid "${VALIDATION_UID}" \
        --gid "${VALIDATION_GID}" \
        --create-home \
        --shell /bin/bash \
        moguet-validation \
    && install -d -o moguet-validation -g moguet-validation -m 0700 \
        /home/moguet-validation/.cache \
        /home/moguet-validation/.config \
        /home/moguet-validation/.local/state \
        /home/moguet-validation/live-provider-cases \
        /home/moguet-validation/work/moguet

ENV HOME=/home/moguet-validation \
    XDG_CACHE_HOME=/home/moguet-validation/.cache \
    XDG_CONFIG_HOME=/home/moguet-validation/.config \
    XDG_STATE_HOME=/home/moguet-validation/.local/state \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    LANGUAGE=en

WORKDIR /home/moguet-validation/work/moguet

# .dockerignore excludes VCS metadata, agent/editor state, credentials, host
# XDG state, sockets, and host build artifacts from this standalone context.
COPY --chown=moguet-validation:moguet-validation . .

USER moguet-validation:moguet-validation

RUN env -u MAKEFLAGS -u MFLAGS make clean \
    && env -u MAKEFLAGS -u MFLAGS make -j8 --output-sync=target

USER root:root

# Runtime package mutation is made impossible after the image has been fully
# provisioned.  The real pacman is retained only as a root-executable image
# artifact for forensic distinction; production Moguet reaches the root-owned
# fail-closed sentinel at the canonical /usr/bin/pacman path.
RUN install -d -o root -g root -m 0711 \
        /usr/libexec/moguet-live-validation \
    && install -o root -g root -m 0700 \
        /usr/bin/pacman \
        /usr/libexec/moguet-live-validation/pacman.real \
    && install -o root -g root -m 0555 \
        containers/arch-live-validation/pacman-sentinel.sh \
        /usr/bin/pacman \
    && install -o root -g root -m 0444 \
        containers/arch-live-validation/fixtures/local-package/contract.env \
        /usr/libexec/moguet-live-validation/contract.env \
    && install -d -o root -g moguet-validation -m 0750 \
        /var/log/moguet-live-validation \
    && for case_name in \
        sentinel-accept-first-provider \
        sentinel-accept-second-provider-noconfirm \
        sentinel-reject-pacman-u \
        sentinel-reject-remove \
        sentinel-reject-syu \
        sentinel-reject-multiple \
        sentinel-reject-unqualified \
        sentinel-reject-option \
        sentinel-reject-unknown-target \
        provider-discovery \
        first-provider-selection \
        second-provider-selection \
        multiple-range \
        multiple-comma \
        multiple-space \
        exclude-only \
        include-exclude \
        invalid-retry \
        cancel-empty \
        cancel-q \
        provider-eof \
        non-tty-pipe \
        noconfirm-tty; do \
        install -d -o root -g moguet-validation -m 0750 \
            "/var/log/moguet-live-validation/${case_name}"; \
    done \
    && printf '%s\n' \
        'Defaults:moguet-validation env_keep += "MOGUET_LIVE_SENTINEL_CASE"' \
        'moguet-validation ALL=(root) NOPASSWD: /usr/bin/pacman *' \
        > /etc/sudoers.d/moguet-live-validation \
    && chown root:root /etc/sudoers.d/moguet-live-validation \
    && chmod 0440 /etc/sudoers.d/moguet-live-validation \
    && visudo -cf /etc/sudoers.d/moguet-live-validation

USER moguet-validation:moguet-validation

CMD ["containers/arch-live-validation/run-provider-selection.sh"]
