set positional-arguments

mode := "debug"
debug-buildtype := "debugoptimized"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"
nythera-lib-pkgconfig := env("HOME") + "/.local/opt/nythera-lib/lib/pkgconfig"

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

_resolve-pkg-config-path local_pc_dir=nythera-lib-pkgconfig:
    #!/usr/bin/env bash
    set -euo pipefail
    if pkg-config --exists nythera-lib 2>/dev/null; then
        printf '%s\n' "${PKG_CONFIG_PATH:-}"
        exit 0
    fi
    if [[ -f "{{local_pc_dir}}/nythera-lib.pc" ]]; then
        if [[ -n "${PKG_CONFIG_PATH:-}" ]]; then
            printf '%s:%s\n' "${PKG_CONFIG_PATH}" "{{local_pc_dir}}"
        else
            printf '%s\n' "{{local_pc_dir}}"
        fi
        exit 0
    fi
    printf '%s\n' "${PKG_CONFIG_PATH:-}"

configure m=mode install_prefix=prefix: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    resolved_pkg_config_path="$(just _resolve-pkg-config-path "{{nythera-lib-pkgconfig}}")"
    if [[ -n "$resolved_pkg_config_path" ]]; then
        export PKG_CONFIG_PATH="$resolved_pkg_config_path"
    fi
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
    #!/usr/bin/env bash
    set -euo pipefail
    resolved_pkg_config_path="$(just _resolve-pkg-config-path "{{nythera-lib-pkgconfig}}")"
    if [[ -n "$resolved_pkg_config_path" ]]; then
        export PKG_CONFIG_PATH="$resolved_pkg_config_path"
    fi
    meson compile -C "build-{{m}}" nythera-file-manager nythera-properties-helper nythera-admin-helper

_ensure-configured m=mode: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    resolved_pkg_config_path="$(just _resolve-pkg-config-path "{{nythera-lib-pkgconfig}}")"
    if [[ -n "$resolved_pkg_config_path" ]]; then
        export PKG_CONFIG_PATH="$resolved_pkg_config_path"
    fi
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
    resolved_pkg_config_path="$(just _resolve-pkg-config-path "{{nythera-lib-pkgconfig}}")"
    if [[ -n "$resolved_pkg_config_path" ]]; then
        export PKG_CONFIG_PATH="$resolved_pkg_config_path"
    fi
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

_check_formatter:
    #!/usr/bin/env bash
    set -euo pipefail
    version="$(clang-format --version)"
    echo "$version"
    if [[ ! "$version" =~ version\ ([0-9]+)\. ]] || [[ "${BASH_REMATCH[1]}" -lt 22 ]]; then
        echo "error: formatting requires clang-format 22 or newer; put its bin directory on PATH" >&2
        exit 1
    fi

format: format-code

format-check: format-code-check

format-code: _check_formatter
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

format-code-check: _check_formatter
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run -Werror

_clang_tidy m=mode *args: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    meson compile -C "build-{{m}}" generated-headers
    src_root="$(realpath src)"
    run-clang-tidy -quiet -use-color -p "build-{{m}}" -j "$(nproc)" -header-filter='\.\./src/.*' {{args}} "^${src_root}/.*"

lint m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} '-warnings-as-errors=*'

fix m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} -fix
    just format-code

clean m=mode: (_validate-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
