#!/bin/bash
# Builds bassepeder/yabai (upstream + community macOS 27 fixes), installs it
# outside Homebrew, signs it with a stable self-signed cert so Accessibility/
# Screen Recording grants survive rebuilds, loads the scripting addition when
# SIP is disabled and checks that space switching actually works.
#
#   curl -fsSL https://raw.githubusercontent.com/bassepeder/yabai/macos27/scripts/install.sh | bash

set -euo pipefail

REPO="https://github.com/bassepeder/yabai.git"
BRANCH="macos27"
SRC="$HOME/src/yabai"
BIN="/opt/homebrew/bin/yabai"
CERT="yabai-cert"
SUDOERS="/private/etc/sudoers.d/yabai"
ERR_LOG="/tmp/yabai_$USER.err.log"

step() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -m)" == arm64 ]] || die "Apple Silicon only"
SA=false
if csrutil status | grep -q disabled; then
  SA=true
  nvram boot-args 2>/dev/null | grep -q -- -arm64e_preview_abi \
    || die "run: sudo nvram boot-args=-arm64e_preview_abi, then reboot"
fi
xcode-select -p >/dev/null 2>&1 || die "run: xcode-select --install"
command -v jq >/dev/null || brew install jq

echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), scripting addition: $SA"
sudo -v

step "Code signing certificate"
if ! security find-certificate -c "$CERT" >/dev/null 2>&1; then
  tmp="$(mktemp -d)"
  cat > "$tmp/cert.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $CERT
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF
  /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$tmp/cert.cnf" \
    -keyout "$tmp/key.pem" -out "$tmp/cert.pem" 2>/dev/null
  /usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" \
    -out "$tmp/cert.p12" -passout pass:yabai
  security import "$tmp/cert.p12" -k "$HOME/Library/Keychains/login.keychain-db" -P yabai -T /usr/bin/codesign
  echo "macOS will ask for your password to trust the certificate for code signing."
  security add-trusted-cert -r trustRoot -p codeSign -k "$HOME/Library/Keychains/login.keychain-db" "$tmp/cert.pem"
  rm -rf "$tmp"
fi
echo "✓ $CERT"

step "Source"
if [[ -d "$SRC/.git" ]]; then
  git -C "$SRC" remote set-url origin "$REPO"
  git -C "$SRC" fetch -q origin
  git -C "$SRC" checkout -q -B "$BRANCH" "origin/$BRANCH"
else
  git clone -q -b "$BRANCH" "$REPO" "$SRC"
fi
git -C "$SRC" log -1 --oneline

step "Build"
make -C "$SRC" clean >/dev/null 2>&1 || true
make -C "$SRC" >/dev/null
[[ -f "$SRC/bin/yabai" ]] || die "build produced no bin/yabai"

step "Install"
yabai --stop-service 2>/dev/null || true
# A brew-managed yabai is upstream, which has no macOS 27 support; `brew upgrade`
# would silently replace this build with it.
if brew list yabai >/dev/null 2>&1; then
  brew uninstall --ignore-dependencies yabai
fi
rm -f "$BIN"
cp "$SRC/bin/yabai" "$BIN"
chmod 755 "$BIN"
if ! codesign -fs "$CERT" "$BIN"; then
  echo "WARNING: signing with $CERT failed, falling back to ad-hoc (permissions will need re-granting after every rebuild)"
  codesign -fs - "$BIN"
fi
"$BIN" --version

if $SA; then
  step "sudoers"
  rule="$USER ALL=(root) NOPASSWD: sha256:$(shasum -a 256 "$BIN" | awk '{print $1}') $BIN --load-sa"
  tmp="$(mktemp)"
  echo "$rule" > "$tmp"
  sudo visudo -cf "$tmp" >/dev/null || die "invalid sudoers rule: $rule"
  sudo install -m 440 -o root -g wheel "$tmp" "$SUDOERS"
  rm -f "$tmp"
  echo "✓ $rule"
fi

start_yabai() {
  : > "$ERR_LOG"
  "$BIN" --install-service >/dev/null 2>&1 || true
  "$BIN" --restart-service >/dev/null 2>&1 || "$BIN" --start-service
  for _ in $(seq 20); do
    "$BIN" -m query --spaces >/dev/null 2>&1 && return 0
    sleep 0.5
  done
  return 1
}

grant() {
  echo
  echo "yabai needs $1 permission."
  echo "In the window that opens: remove any existing 'yabai' entry (–), then add $BIN (+) and enable it."
  open "x-apple.systempreferences:com.apple.preference.security?Privacy_$2"
  read -r -p "Press Enter when done... " < /dev/tty
}

step "Start yabai"
until start_yabai; do
  tccutil reset Accessibility com.asmvik.yabai >/dev/null 2>&1 || true
  grant Accessibility Accessibility
done
if grep -q "Screen Recording" "$ERR_LOG"; then
  grant "Screen Recording" ScreenCapture
  start_yabai || die "yabai not responding after restart, see $ERR_LOG"
fi
echo "✓ yabai running"

if $SA; then
  step "Scripting addition"
  # Fresh Dock so the addition is injected once, via yabairc's dock_did_restart signal;
  # a stale Dock stops handling the native ctrl-N "Switch to Desktop" shortcuts.
  killall Dock
  sleep 4
  grep -qs -- --load-sa "$HOME/.config/yabai/yabairc" "$HOME/.yabairc" || sudo "$BIN" --load-sa
fi

step "Space switching test"
cur="$("$BIN" -m query --spaces --space | jq .index)"
other="$("$BIN" -m query --spaces --display | jq "[.[] | select(.index != $cur)][0].index // empty")"
if [[ -z "$other" ]]; then
  echo "only one space on this display, create another to test"
else
  "$BIN" -m space --focus "$other"
  sleep 0.7
  now="$("$BIN" -m query --spaces --space | jq .index)"
  "$BIN" -m space --focus "$cur" || true
  if [[ "$now" == "$other" ]]; then
    echo "✓ space $cur -> $other -> $cur works"
  else
    echo "✗ space focus did not switch. The Dock patterns likely don't match this macOS build;"
    echo "  see https://github.com/asmvik/yabai/issues/2832 (27.2) and #2802 (27.0)."
    exit 1
  fi
fi

if command -v skhd >/dev/null; then
  step "skhd"
  skhd --restart-service >/dev/null 2>&1 || skhd --start-service
  echo "✓ skhd restarted"
fi

echo
echo "Done."
