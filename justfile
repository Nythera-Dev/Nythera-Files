set positional-arguments

mode := "debug"
debug-buildtype := "debugoptimized"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

_validate-mode m:
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{m}}" in
        debug|release|asan) ;;
        *)
            echo "error: invalid build mode '{{m}}' (expected: debug, release, or asan)" >&2
            exit 1
            ;;
    esac

configure m=mode install_prefix=prefix: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    args=(--buildtype={{ if m == "release" { "release" } else if m == "asan" { "debug" } else { debug-buildtype } }} -Dcpp_std={{cpp-std}} -Dtests=auto)
    [[ "{{m}}" == "release" ]] && args+=(-Db_lto=true)
    [[ "{{m}}" == "asan"    ]] && args+=(-Db_sanitize=address,undefined)
    if [[ -d "build-{{m}}" ]]; then
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}" --reconfigure
    else
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

build m=mode: (_ensure-configured m)
    meson compile -C build-{{m}} nythera-file-manager nythera-properties-helper nythera-admin-helper

_ensure-configured m=mode: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
        exit 0
    fi
    configure_output="$(meson configure "build-{{m}}")"
    current_cpp_std="$(awk '$1 == "cpp_std" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    current_tests="$(awk '$1 == "tests" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    current_buildtype="$(awk '$1 == "buildtype" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    desired_buildtype="{{ if m == "release" { "release" } else if m == "asan" { "debug" } else { debug-buildtype } }}"
    args=()
    [[ "$current_buildtype" != "$desired_buildtype" ]] && args+=(-Dbuildtype="$desired_buildtype")
    [[ "$current_cpp_std" != "{{cpp-std}}" ]] && args+=(-Dcpp_std={{cpp-std}})
    [[ "$current_tests" != "auto" ]] && args+=(-Dtests=auto)
    if (( ${#args[@]} > 0 )); then
        meson configure "build-{{m}}" "${args[@]}"
    fi

run m=mode: (build m)
    ./build-{{m}}/nythera-file-manager

test m=mode *args: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{m}}" == "release" || "{{m}}" == "asan" ]]; then
        meson setup "build-{{m}}" -Dtests=enabled --reconfigure >/dev/null
    fi
    meson test -C build-{{m}} {{args}}

install m: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -x "build-{{m}}/nythera-file-manager" ]]; then
        echo "error: build-{{m}}/nythera-file-manager is missing; run 'just build {{m}}' before installing" >&2
        exit 1
    fi
    meson install --no-rebuild -C build-{{m}}

uninstall m: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        echo "error: build-{{m}} is missing or was not configured with the Ninja backend; nothing to uninstall" >&2
        exit 1
    fi
    ninja -C build-{{m}} uninstall

clean m=mode: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
