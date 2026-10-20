#!/bin/sh
# Explicit, non-destructive helper installation. Existing files/config are kept.
set -eu

if [ "$(uname -s)" != Darwin ]; then
  printf '%s\n' 'These helpers require macOS.' >&2
  exit 1
fi
dry_run=false
case "${1:-}" in
  --dry-run) dry_run=true ;;
  '') ;;
  *) printf '%s\n' 'Usage: install-macos-helpers.sh [--dry-run]' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
helper_prefix=${NCHAT_HELPER_PREFIX:-$HOME/.local}
kitty_dir=${NCHAT_KITTY_CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/kitty}
applications_dir=${NCHAT_APPLICATIONS_DIR:-$HOME/Applications}
if [ -n "${NCHAT_CONFIG_DIR:-}" ]; then
  nchat_config=$NCHAT_CONFIG_DIR
elif [ -d "$HOME/.nchat" ]; then
  nchat_config=$HOME/.nchat
else
  nchat_config=${XDG_CONFIG_HOME:-$HOME/.config}/nchat
fi

install_missing() {
  source=$1 destination=$2 mode=$3
  if [ -e "$destination" ] || [ -L "$destination" ]; then
    printf 'Kept existing: %s\n' "$destination"
  elif $dry_run; then
    printf 'Would install: %s\n' "$destination"
  else
    /usr/bin/install -d "$(dirname -- "$destination")"
    /usr/bin/install -m "$mode" "$source" "$destination"
    printf 'Installed: %s\n' "$destination"
  fi
}

for helper in nchat-image nchat-thumbnails nchat-image-clear nchat-peek; do
  install_missing "$source_dir/$helper" "$helper_prefix/bin/$helper" 755
done
install_missing "$source_dir/nchat-peek-view.py" "$helper_prefix/lib/nchat-peek-view.py" 644
for helper in nchat_badge.py nchat_badge_auth.py; do
  install_missing "$source_dir/$helper" "$kitty_dir/$helper" 644
done
install_missing "$source_dir/kitty-nchat.conf" "$kitty_dir/nchat-example.conf" 644

for bundle in 'nchat Unread.app' nchat-transcribe.app nchat-record.app; do
  source_bundle="$source_dir/../build/$bundle"
  if [ "$bundle" = 'nchat Unread.app' ]; then
    destination_bundle="$applications_dir/$bundle"
  else
    destination_bundle="$nchat_config/helpers/$bundle"
  fi
  if [ ! -d "$source_bundle" ]; then
    printf 'Not built yet: %s\n' "$bundle"
  elif [ -e "$destination_bundle" ] || [ -L "$destination_bundle" ]; then
    printf 'Kept existing: %s\n' "$destination_bundle"
  elif $dry_run; then
    printf 'Would install: %s\n' "$destination_bundle"
  else
    /usr/bin/install -d "$(dirname -- "$destination_bundle")"
    /usr/bin/ditto "$source_bundle" "$destination_bundle"
    printf 'Installed: %s\n' "$destination_bundle"
  fi
done

printf '\n%s\n' \
  'No nchat or Kitty configuration was changed, and no app was started.' \
  'Add the helper bin directory to PATH, then review nchat-example.conf before including it in Kitty.' \
  'Set attachment_open_command and thumbnail_command in ui.conf if those helpers are not on PATH.' \
  'Optional dependencies: fzf for nchat-peek; chafa for terminal images (otherwise native Quick Look opens).'
