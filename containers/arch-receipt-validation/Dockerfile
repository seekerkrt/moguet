FROM moguet-arch-validation:local

LABEL org.opencontainers.image.title="Moguet trusted ALPM receipt validation" \
      org.opencontainers.image.source="https://github.com/seekerkrt/moguet"

USER root:root

RUN install -d -o moguet-validation -g moguet-validation -m 0755 \
        /home/moguet-validation/work/moguet-receipt \
        /home/moguet-validation/receipt-packages \
        /home/moguet-validation/receipt-packages-alternate

WORKDIR /home/moguet-validation/work/moguet-receipt

# The standalone Docker build context is filtered by .dockerignore. No host
# worktree mount, Git metadata, credential, XDG data, or package database is
# available to the runtime validation container.
COPY --chown=moguet-validation:moguet-validation . .

USER moguet-validation:moguet-validation

RUN env -u MAKEFLAGS -u MFLAGS make clean \
    && env -u MAKEFLAGS -u MFLAGS make \
        PREFIX=/usr -j8 --output-sync=target

# Build the owner-specific installed fixture with the same compiled helper
# path as the package payload. It remains uninstalled and is run only as the
# unprivileged validation user.
RUN cmake -S . -B build/cmake-receipt-testing \
        -DBUILD_TESTING=ON \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DMOGUET_INSTALL_INTERNAL_EXECUTABLE_DIRECTORY=/usr/libexec/moguet \
    && cmake --build build/cmake-receipt-testing \
        --target moguet-source-artifact-install-installed-fixture \
                 evaluated-devel-source-artifact-transport-test \
        --parallel 8

RUN for fixture in \
        dependency-v1 \
        dependency-v2 \
        target \
        source-v1 \
        source-v2 \
        source-failure \
        source-multi; do \
        ( \
            cd "containers/arch-receipt-validation/fixtures/$fixture" \
            && PKGDEST=/home/moguet-validation/receipt-packages \
                makepkg --nodeps --noconfirm \
        ) || exit $?; \
    done

# Same package identity/version as source-v2, but different package bytes.
# A separate destination preserves both artifacts for the Slice 1 reinstall
# characterization without changing the existing receipt fixture inventory.
RUN ( \
        cd containers/arch-receipt-validation/fixtures/source-v2-different \
        && PKGDEST=/home/moguet-validation/receipt-packages-alternate \
            makepkg --nodeps --noconfirm \
    )

USER root:root

# This authority exists only inside the disposable networkless fixture. The
# production transport itself still supplies fixed helper/pacman argv.
RUN printf '%s\n' \
        'moguet-validation ALL=(root) NOPASSWD: /usr/libexec/moguet/moguet-source-artifact-install-helper *, /usr/bin/pacman -U *' \
        > /etc/sudoers.d/moguet-source-artifact-receipt \
    && chmod 0440 /etc/sudoers.d/moguet-source-artifact-receipt \
    && visudo -cf /etc/sudoers.d/moguet-source-artifact-receipt

# Install through the configured CMake graph. The hook will execute only this
# root-owned installed payload; the build-tree helper remains unexecuted.
RUN cmake --install build/cmake-production \
    && test "$(stat -c '%U:%G:%a:%F' \
        /usr/libexec/moguet/moguet-alpm-receipt-helper)" = \
        'root:root:755:regular file' \
    && test "$(stat -c '%U:%G:%a:%F' \
        /usr/libexec/moguet/moguet-source-artifact-install-helper)" = \
        'root:root:755:regular file'

CMD ["/bin/sh", "containers/arch-receipt-validation/run-installed-receipt.sh"]
