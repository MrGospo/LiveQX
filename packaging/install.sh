#!/usr/bin/env bash
#
# LiveQX Engine — установщик для Ubuntu/Debian.
#
# Режимы:
#   install    — установить (по умолчанию)
#   uninstall  — удалить ВСЁ (бинари, юниты, БД, конфиги, пользователь)
#   check      — проверить текущее состояние установки
#
# Запуск: sudo ./packaging/install.sh [install|uninstall|check] [-y]
#
set -euo pipefail
IFS=$'\n\t'

# На минимальных Debian /usr/sbin и /sbin отсутствуют в PATH для обычных
# пользователей — из-за этого падают groupadd/useradd/nologin, даже под sudo.
export PATH="/usr/local/sbin:/usr/sbin:/sbin:${PATH}"

# ── Константы путей ─────────────────────────────────────────────────────────
LIVEQX_USER="liveqx"
LIVEQX_GROUP="liveqx"
BIN_DIR="/usr/local/bin"
STATE_DIR="/var/lib/liveqx"
CONF_DIR="/etc/liveqx"
CONF_FILE="${CONF_DIR}/config.json"
UI_DIR="/usr/share/liveqx/ui"
DOC_DIR="/usr/share/doc/liveqx"
SYSTEMD_DIR="/etc/systemd/system"
MOUNT_ROOT="/mnt/liveqx"
RUNTIME_DIR="/run/liveqx"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ── Цветной вывод ───────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_RED=$'\033[0;31m'; C_GRN=$'\033[0;32m'; C_YEL=$'\033[1;33m'
    C_BLU=$'\033[0;34m'; C_BLD=$'\033[1m';     C_RST=$'\033[0m'
else
    C_RED=''; C_GRN=''; C_YEL=''; C_BLU=''; C_BLD=''; C_RST=''
fi
info(){ echo "${C_BLU}[i]${C_RST} $*"; }
ok()  { echo "${C_GRN}[✓]${C_RST} $*"; }
warn(){ echo "${C_YEL}[!]${C_RST} $*"; }
err() { echo "${C_RED}[✗]${C_RST} $*" >&2; }
die() { err "$*"; exit 1; }
step(){ echo; echo "${C_BLD}── $* ──${C_RST}"; }

usage() {
    cat <<EOF
LiveQX Engine — установщик

Использование:
    sudo $0 [install|uninstall|check] [опции]

Команды:
    install     Установить LiveQX (по умолчанию)
    uninstall   Удалить LiveQX полностью (БД, конфиги, пользователь liveqx)
    check       Проверить состояние установки

Опции:
    -y, --yes       Не задавать вопросов
    -h, --help      Показать справку

Перед запуском install убедитесь что собрано:
    build/liveqx               — основной бинарь
    build/liveqx-mountd        — helper для монтирования шар
    ui/dist/                   — Web UI

Команда сборки:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \\
        -DENABLE_NVENC=ON -DENABLE_QSV=ON -DENABLE_VAAPI=ON \\
        -DENABLE_WEBRTC_PREVIEW=ON -DENABLE_SYSTEMD=ON
    cmake --build build -j\$(nproc) --target liveqx liveqx-mountd
    (cd ui && npm ci && npm run build)
EOF
}

# ── Парсинг аргументов ──────────────────────────────────────────────────────
MODE="install"
ASSUME_YES=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        install|uninstall|check) MODE="$1"; shift ;;
        -y|--yes)  ASSUME_YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Неизвестный аргумент: $1 (см. --help)" ;;
    esac
done

# ── Утилиты ─────────────────────────────────────────────────────────────────
ask_yn() {
    local q="$1" def="${2:-n}" prompt ans
    if [[ "$ASSUME_YES" == "1" ]]; then
        [[ "$def" == "y" ]]; return $?
    fi
    [[ "$def" == "y" ]] && prompt="[Y/n]" || prompt="[y/N]"
    while true; do
        read -r -p "${C_BLD}?${C_RST} ${q} ${prompt} " ans || ans=""
        ans="${ans:-$def}"
        case "$ans" in
            [Yy]|[Yy][Ee][Ss]) return 0 ;;
            [Nn]|[Nn][Oo])     return 1 ;;
            *) echo "Введите y или n" ;;
        esac
    done
}

detect_os() {
    [[ -f /etc/os-release ]] || die "Не найден /etc/os-release"
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-}" in
        ubuntu|debian) ;;
        *) die "Поддерживаются только Ubuntu и Debian (нашёл: ${ID:-unknown})" ;;
    esac
    info "Дистрибутив: ${PRETTY_NAME:-$ID}"
}

is_wsl() { grep -qiE "microsoft|wsl" /proc/version 2>/dev/null; }

require_root() {
    [[ $EUID -eq 0 ]] || die "Запустите через sudo: sudo $0 $MODE"
}

# ════════════════════════════════════════════════════════════════════════════
# INSTALL
# ════════════════════════════════════════════════════════════════════════════
cmd_install() {
    detect_os
    info "Корень репозитория: $REPO_ROOT"

    # ── Артефакты ──
    step "Проверка артефактов"
    [[ -x "$REPO_ROOT/build/liveqx" ]]        || die "Нет build/liveqx — соберите бинарь (см. --help)"
    [[ -x "$REPO_ROOT/build/liveqx-mountd" ]] || die "Нет build/liveqx-mountd — соберите helper"
    [[ -d "$REPO_ROOT/ui/dist" ]]             || die "Нет ui/dist/ — соберите UI: cd ui && npm ci && npm run build"
    [[ -f "$REPO_ROOT/config/default.json" ]] || die "Нет config/default.json"
    [[ -f "$REPO_ROOT/packaging/liveqx.service" ]] || die "Нет packaging/liveqx.service"
    ok "Все артефакты на месте"

    # ── Apt-зависимости ──
    # Runtime-so, к которым линкуется liveqx + liveqx-mountd.
    # spdlog/fmt/cpp-httplib собраны статически (у них ABI между LTS нестабильный).
    step "Системные пакеты"
    info "Обновляю индекс apt..."
    apt-get update -qq
    info "Устанавливаю runtime-зависимости..."
    # libldap имя пакета меняется между дистрибутивами:
    #   Debian 12 / Ubuntu 22.04 → libldap-2.5-0
    #   Debian 13 / Ubuntu 24.04 → libldap-2.6-0
    # Ищем, что реально доступно в apt-cache — иначе install падает.
    LIBLDAP_PKG=""
    for cand in libldap-2.5-0 libldap-2.6-0 libldap2; do
        if apt-cache show "$cand" >/dev/null 2>&1; then
            LIBLDAP_PKG="$cand"
            break
        fi
    done
    [[ -z "$LIBLDAP_PKG" ]] && die "Не найден ни один пакет libldap (пробовал: libldap-2.5-0, libldap-2.6-0, libldap2)"
    info "Пакет libldap: $LIBLDAP_PKG"

    apt-get install -y --no-install-recommends \
        ca-certificates curl python3 cifs-utils \
        libssl3 libsqlite3-0 libsodium23 libsrtp2-1 \
        "$LIBLDAP_PKG" libcurl4 libnss3 libgnutls30 \
        libx264-164 \
        libva2 libva-drm2 libdrm2 libmfx1 \
        || die "apt-get install не сработал. Проверьте, что подключены репозитории universe (Ubuntu) или non-free (Debian) — там лежат libmfx1 и libx264-164."
    ok "Все runtime-пакеты установлены"

    # ── Вопросы по HW-энкодерам ──
    step "HW-энкодеры (H.264/H.265 через GPU)"
    cat <<EOF
    LiveQX поддерживает три аппаратных энкодера. Отметьте те,
    что планируете использовать. Драйверы и SDK устанавливаются
    отдельно (вручную) — сейчас скрипт только зафиксирует выбор.

EOF

    USE_NVENC=0; USE_VAAPI=0; USE_QSV=0
    ask_yn "Будете использовать NVENC (NVIDIA GPU)?" "n" && USE_NVENC=1 || true
    ask_yn "Будете использовать VAAPI (Intel iGPU или AMD GPU)?" "n" && USE_VAAPI=1 || true
    ask_yn "Будете использовать QSV (Intel Quick Sync Video)?" "n" && USE_QSV=1 || true

    # ── Порт Web UI + API ──
    # Спрашиваем только при первой установке; иначе берём порт из существующего конфига.
    API_PORT=8080
    if [[ -f "$CONF_FILE" ]]; then
        API_PORT=$(python3 -c "import json; print(json.load(open('$CONF_FILE')).get('api_port', 8080))" 2>/dev/null || echo 8080)
    elif [[ "$ASSUME_YES" != "1" ]]; then
        step "Порт Web UI и API"
        while true; do
            read -r -p "${C_BLD}?${C_RST} Порт для Web UI и HTTP API [$API_PORT]: " ans || ans=""
            ans="${ans:-$API_PORT}"
            if [[ "$ans" =~ ^[0-9]+$ ]] && (( ans >= 1 && ans <= 65535 )); then
                API_PORT="$ans"
                (( API_PORT < 1024 )) && warn "Порт <1024 требует CAP_NET_BIND_SERVICE в юните — проверьте после старта"
                break
            fi
            warn "Введите число от 1 до 65535"
        done
    fi

    # ── Пользователь и группа ──
    step "Системный пользователь $LIVEQX_USER"
    if ! getent group "$LIVEQX_GROUP" >/dev/null; then
        groupadd --system "$LIVEQX_GROUP"
        ok "Создана группа $LIVEQX_GROUP"
    else
        ok "Группа $LIVEQX_GROUP уже есть"
    fi
    if ! getent passwd "$LIVEQX_USER" >/dev/null; then
        useradd --system --gid "$LIVEQX_GROUP" \
                --home-dir "$STATE_DIR" --no-create-home \
                --shell /usr/sbin/nologin \
                "$LIVEQX_USER"
        ok "Создан пользователь $LIVEQX_USER"
    else
        ok "Пользователь $LIVEQX_USER уже есть"
    fi

    # ── Каталоги ──
    step "Каталоги и права"
    install -d -o "$LIVEQX_USER" -g "$LIVEQX_GROUP" -m 0750 "$STATE_DIR"
    install -d -o root           -g root           -m 0755 "$CONF_DIR"
    install -d -o root           -g root           -m 0755 "$UI_DIR"
    install -d -o root           -g root           -m 0755 "$DOC_DIR"
    install -d -o root           -g root           -m 0755 "$MOUNT_ROOT"
    ok "Каталоги созданы"

    # ── Бинарники ──
    step "Бинарники → $BIN_DIR"
    # Остановить перед копированием: иначе ETXTBSY на работающем бинаре.
    if systemctl is-active --quiet liveqx 2>/dev/null; then
        info "Останавливаю работающий liveqx..."
        systemctl stop liveqx.service        || true
        systemctl stop liveqx-mountd.service || true
    fi
    install -m 0755 "$REPO_ROOT/build/liveqx"        "$BIN_DIR/liveqx"
    install -m 0755 "$REPO_ROOT/build/liveqx-mountd" "$BIN_DIR/liveqx-mountd"
    ok "Установлены /usr/local/bin/liveqx и liveqx-mountd"

    # Проверка ldd: без неё нехватка shared library даёт бесконечный рестарт
    # systemd без понятной причины — лучше упасть здесь с явным списком.
    local missing
    missing=$(ldd "$BIN_DIR/liveqx" 2>/dev/null | awk '/not found/ {print $1}' | sort -u || true)
    missing+=$'\n'$(ldd "$BIN_DIR/liveqx-mountd" 2>/dev/null | awk '/not found/ {print $1}' | sort -u || true)
    missing=$(echo "$missing" | grep -v '^$' | sort -u || true)
    if [[ -n "$missing" ]]; then
        err "Не найдены shared libraries — бинарь не запустится:"
        echo "$missing" | sed 's/^/      /'
        echo
        echo "  Поищите пакеты для них:"
        echo "    apt-file search <имя_библиотеки>"
        echo "  и установите через apt-get install."
        exit 1
    fi
    ok "Все shared libraries разрешены (ldd чисто)"

    # ── UI ──
    step "Web-интерфейс → $UI_DIR"
    rm -rf "${UI_DIR:?}"/*
    cp -r "$REPO_ROOT/ui/dist/." "$UI_DIR/"
    chown -R root:root "$UI_DIR"
    ok "UI установлен"

    # ── Конфиг ──
    step "Конфигурация"
    if [[ ! -f "$CONF_FILE" ]]; then
        install -m 0644 "$REPO_ROOT/config/default.json" "$CONF_FILE"
        API_PORT="$API_PORT" python3 - "$CONF_FILE" <<'PY'
import json, os, sys
p = sys.argv[1]
with open(p) as f: cfg = json.load(f)
cfg['ui_dir']       = '/usr/share/liveqx/ui'
cfg['state_dir']    = '/var/lib/liveqx/state'
cfg['log_dir']      = '/var/lib/liveqx/logs'
cfg['channels_dir'] = '/var/lib/liveqx/channels'
cfg['api_port']     = int(os.environ['API_PORT'])
with open(p, 'w') as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write('\n')
PY
        ok "Создан $CONF_FILE (порт API $API_PORT)"
    else
        warn "Конфиг $CONF_FILE уже существует — не трогаю (правьте вручную при необходимости)"
    fi

    # Documentation= в юнитах ссылается на эти файлы.
    for d in SYSTEMD.md MOUNTS.md AUTH.md; do
        [[ -f "$REPO_ROOT/docs/$d" ]] && install -m 0644 "$REPO_ROOT/docs/$d" "$DOC_DIR/" || true
    done

    # ── Systemd-юниты ──
    step "Systemd-юниты"
    install -m 0644 "$REPO_ROOT/packaging/liveqx.service"            "$SYSTEMD_DIR/"
    install -m 0644 "$REPO_ROOT/packaging/liveqx-mountd.service"     "$SYSTEMD_DIR/"
    install -m 0644 "$REPO_ROOT/packaging/liveqx-mount-init.service" "$SYSTEMD_DIR/"
    systemctl daemon-reload
    ok "Юниты установлены"

    # ── WSL2 ──
    if is_wsl; then
        warn "Детектирован WSL2 — делаю корневой mount shared (нужно для propagation CIFS из mountd)"
        mount --make-rshared / || warn "mount --make-rshared / не сработал; возможно ядро WSL не поддерживает — проверьте после старта"
    fi

    # ── Запуск ──
    step "Запуск служб"
    systemctl enable --now liveqx-mount-init.service
    systemctl enable --now liveqx-mountd.service
    systemctl enable --now liveqx.service

    info "Жду готовности liveqx (до 15с)..."
    local ready=0
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        sleep 1
        if systemctl is-active --quiet liveqx; then ready=1; break; fi
    done
    if [[ "$ready" == "1" ]]; then
        ok "liveqx.service: active"
    else
        err "liveqx.service не запустился"
        echo "    Журнал:  sudo journalctl -u liveqx -n 80 --no-pager"
        exit 1
    fi

    # API по умолчанию в HTTPS (auto), но TLS может быть отключён — пробуем оба
    # и запоминаем, какая схема ответила, чтобы вывести правильный URL пользователю.
    local api_port api_scheme=""
    api_port=$(python3 -c "import json; print(json.load(open('$CONF_FILE')).get('api_port', 8080))" 2>/dev/null || echo 8080)
    if curl -fsSk --max-time 3 "https://localhost:${api_port}/api/health" >/dev/null 2>&1; then
        api_scheme="https"
        ok "/api/health: OK (https, порт $api_port)"
    elif curl -fsS --max-time 3 "http://localhost:${api_port}/api/health" >/dev/null 2>&1; then
        api_scheme="http"
        ok "/api/health: OK (http, порт $api_port)"
    else
        # По умолчанию наш конфиг делает self-signed HTTPS — предполагаем https.
        api_scheme="https"
        warn "/api/health пока не отвечает (может быть нормально для первого старта)"
    fi

    # ── Финальные подсказки ──
    echo
    echo "${C_GRN}════════════════════════════════════════════════════════════════${C_RST}"
    echo "${C_GRN}  Установка завершена${C_RST}"
    echo "${C_GRN}════════════════════════════════════════════════════════════════${C_RST}"
    local ip
    ip=$(hostname -I 2>/dev/null | awk '{print $1}')
    echo
    echo "  Web UI:        ${api_scheme}://${ip:-localhost}:${api_port}/"
    if [[ "$api_scheme" == "https" ]]; then
        echo "                 ${C_YEL}(self-signed сертификат — браузер покажет предупреждение при первом входе)${C_RST}"
    fi
    echo "  Журнал:        sudo journalctl -u liveqx -f"
    echo "  Статус:        systemctl status liveqx"
    echo "  Конфиг:        $CONF_FILE"
    echo "  Данные:        $STATE_DIR"
    echo "  Шары:          $MOUNT_ROOT/<id>/"

    # ── Bootstrap-пароль первого админа ──
    # Файл (0600, owner=liveqx) создаёт сервис на первом старте и авто-удаляет
    # при первой смене пароля.
    local pw_file="$STATE_DIR/state/initial_admin_password.txt"
    if [[ -s "$pw_file" ]]; then
        local pw
        pw=$(cat "$pw_file")
        echo
        echo "${C_BLD}── Логин администратора ──${C_RST}"
        echo "  Пользователь:  admin"
        echo "  Пароль:        ${C_GRN}${pw}${C_RST}"
        echo "  ${C_YEL}Смените при первом входе — файл авто-удалится:${C_RST}"
        echo "    $pw_file"
    fi

    # Зафиксированный выбор HW-энкодеров (драйверы пользователь ставит сам)
    if [[ "$USE_NVENC" == "1" || "$USE_VAAPI" == "1" || "$USE_QSV" == "1" ]]; then
        echo
        echo "  ${C_BLD}Выбранные HW-энкодеры:${C_RST}"
        [[ "$USE_NVENC" == "1" ]] && echo "    • NVENC"
        [[ "$USE_VAAPI" == "1" ]] && echo "    • VAAPI"
        [[ "$USE_QSV"   == "1" ]] && echo "    • QSV"
        echo "    Драйверы и SDK для них установите вручную, затем:"
        echo "      sudo systemctl restart liveqx"
    fi

    echo
    echo "${C_BLD}── Дальнейшие шаги ──${C_RST}"
    echo "  1. Откройте Web UI и завершите начальную настройку"
    echo "  2. Для продакшена настройте TPM2-credstore для master.key"
    echo "     (см. комментарии в /etc/systemd/system/liveqx.service)"
    echo "  3. CIFS-шары добавляются через UI → Настройки → Шары"
    echo
}

# ════════════════════════════════════════════════════════════════════════════
# UNINSTALL
# ════════════════════════════════════════════════════════════════════════════
cmd_uninstall() {
    detect_os

    echo
    warn "Будет удалено ВСЁ кроме apt-пакетов:"
    echo "    • Службы:   liveqx, liveqx-mountd, liveqx-mount-init"
    echo "    • Бинарь:   $BIN_DIR/liveqx, $BIN_DIR/liveqx-mountd"
    echo "    • Конфиг:   $CONF_DIR/  (включая config.json)"
    echo "    • Данные:   $STATE_DIR/ (БД, master.key, состояние каналов)"
    echo "    • UI:       $UI_DIR/"
    echo "    • Доки:     $DOC_DIR/"
    echo "    • Юниты:    $SYSTEMD_DIR/liveqx*.service + динамические mnt-liveqx-*.{mount,automount}"
    echo "    • Шары:     размонтирование $MOUNT_ROOT/* и удаление каталога"
    echo "    • Юзер:     $LIVEQX_USER, группа $LIVEQX_GROUP"
    echo "    • Runtime:  $RUNTIME_DIR/"
    echo
    echo "    apt-пакеты (cifs-utils, libsrtp2-1, nvidia-driver, ...) НЕ удаляются."
    echo

    if ! ask_yn "Точно удалить?" "n"; then
        info "Отмена"; exit 0
    fi

    step "Остановка служб"
    for u in liveqx liveqx-mountd liveqx-mount-init; do
        systemctl stop    "$u.service" 2>/dev/null || true
        systemctl disable "$u.service" 2>/dev/null || true
    done

    # Динамические юниты шар (создаются mountd'ом)
    info "Останавливаю динамические mount-юниты..."
    while IFS= read -r u; do
        [[ -z "$u" ]] && continue
        systemctl stop    "$u" 2>/dev/null || true
        systemctl disable "$u" 2>/dev/null || true
    done < <(systemctl list-unit-files --no-legend 'mnt-liveqx-*' 2>/dev/null | awk '{print $1}')

    step "Размонтирование $MOUNT_ROOT"
    if mountpoint -q "$MOUNT_ROOT" 2>/dev/null; then
        # Сначала отмонтировать вложенные точки (если есть CIFS)
        awk -v root="$MOUNT_ROOT/" '$2 ~ "^"root {print $2}' /proc/mounts | tac | while read -r mp; do
            umount "$mp" 2>/dev/null || umount -l "$mp" 2>/dev/null || true
        done
        umount "$MOUNT_ROOT" 2>/dev/null || umount -l "$MOUNT_ROOT" 2>/dev/null || true
    fi

    step "Удаление юнитов"
    rm -f "$SYSTEMD_DIR/liveqx.service" \
          "$SYSTEMD_DIR/liveqx-mountd.service" \
          "$SYSTEMD_DIR/liveqx-mount-init.service"
    # Динамические шары
    rm -f "$SYSTEMD_DIR"/mnt-liveqx-*.mount     2>/dev/null || true
    rm -f "$SYSTEMD_DIR"/mnt-liveqx-*.automount 2>/dev/null || true
    systemctl daemon-reload
    systemctl reset-failed 2>/dev/null || true

    step "Удаление бинарников"
    rm -f "$BIN_DIR/liveqx" "$BIN_DIR/liveqx-mountd"

    step "Удаление данных, конфигов, UI"
    rm -rf "$STATE_DIR" "$CONF_DIR" "$UI_DIR" "$DOC_DIR" "$RUNTIME_DIR"

    # Каталог /mnt/liveqx — пустой после umount, удаляем
    if [[ -d "$MOUNT_ROOT" ]]; then
        rmdir "$MOUNT_ROOT" 2>/dev/null || warn "$MOUNT_ROOT не пустой — не удалён"
    fi
    # Родитель /usr/share/liveqx если пуст
    [[ -d /usr/share/liveqx ]] && rmdir /usr/share/liveqx 2>/dev/null || true

    step "Удаление пользователя и группы"
    if getent passwd "$LIVEQX_USER" >/dev/null; then
        userdel "$LIVEQX_USER" 2>/dev/null || warn "userdel $LIVEQX_USER вернул ошибку (возможно есть открытые процессы)"
    fi
    if getent group "$LIVEQX_GROUP" >/dev/null; then
        groupdel "$LIVEQX_GROUP" 2>/dev/null || warn "groupdel $LIVEQX_GROUP вернул ошибку"
    fi

    echo
    ok "Полное удаление завершено"
    echo
    echo "  apt-пакеты оставлены установленными. При желании удалить вручную:"
    echo "    sudo apt-get autoremove --purge cifs-utils libsrtp2-1"
    echo
}

# ════════════════════════════════════════════════════════════════════════════
# CHECK
# ════════════════════════════════════════════════════════════════════════════
cmd_check() {
    detect_os
    echo
    info "Состояние установки LiveQX"

    step "Пользователь и группа"
    getent passwd "$LIVEQX_USER" >/dev/null && ok "user $LIVEQX_USER" || err "user $LIVEQX_USER отсутствует"
    getent group  "$LIVEQX_GROUP" >/dev/null && ok "group $LIVEQX_GROUP" || err "group $LIVEQX_GROUP отсутствует"

    step "Файлы"
    [[ -x "$BIN_DIR/liveqx" ]]        && ok "$BIN_DIR/liveqx"        || err "нет $BIN_DIR/liveqx"
    [[ -x "$BIN_DIR/liveqx-mountd" ]] && ok "$BIN_DIR/liveqx-mountd" || err "нет $BIN_DIR/liveqx-mountd"
    [[ -f "$CONF_FILE" ]]             && ok "$CONF_FILE"             || err "нет $CONF_FILE"
    [[ -d "$STATE_DIR" ]]             && ok "$STATE_DIR"              || err "нет $STATE_DIR"
    if [[ -d "$UI_DIR" ]] && [[ -n "$(ls -A "$UI_DIR" 2>/dev/null)" ]]; then
        ok "$UI_DIR (есть файлы)"
    else
        err "UI пустой или отсутствует"
    fi

    step "Systemd-юниты"
    for u in liveqx-mount-init liveqx-mountd liveqx; do
        if [[ -f "$SYSTEMD_DIR/$u.service" ]]; then
            local state
            state=$(systemctl is-active "$u.service" 2>/dev/null || echo inactive)
            if [[ "$state" == "active" ]]; then
                ok "$u.service: $state"
            else
                warn "$u.service: $state"
            fi
        else
            err "$u.service: юнит не установлен"
        fi
    done

    step "Mount propagation"
    if mountpoint -q "$MOUNT_ROOT" 2>/dev/null; then
        ok "$MOUNT_ROOT: bind-mount активен"
    else
        warn "$MOUNT_ROOT: не bind-mount (liveqx-mount-init не отработал?)"
    fi

    step "HTTP health"
    local api_port=8080
    [[ -f "$CONF_FILE" ]] && api_port=$(python3 -c "import json; print(json.load(open('$CONF_FILE')).get('api_port', 8080))" 2>/dev/null || echo 8080)
    if curl -fsSk --max-time 3 "https://localhost:${api_port}/api/health" >/dev/null 2>&1 \
       || curl -fsS  --max-time 3 "http://localhost:${api_port}/api/health"  >/dev/null 2>&1; then
        ok "/api/health отвечает на порту $api_port"
    else
        err "/api/health не отвечает на порту $api_port"
    fi
    echo
}

# ── Диспетчер ───────────────────────────────────────────────────────────────
require_root
case "$MODE" in
    install)   cmd_install ;;
    uninstall) cmd_uninstall ;;
    check)     cmd_check ;;
    *)         die "Неизвестный режим: $MODE" ;;
esac
