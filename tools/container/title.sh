#!/bin/bash
# Builds the libraries, DXVK and the game for the PS5 and writes the title folder to /work/dist.
source "$(dirname "$0")/common.sh"
mkdir -p "$OUT"
install_crt
cd "$TREE/ps5-spike"

# The tile and launch backgrounds: the user's own art/icon.png and art/wallpaper.png when
# present, otherwise PS5_Vulkan's placeholder art.
sce=$TREE/ps5-spike/title/sce_sys
mkdir -p "$sce"
if [ -f /work/art/icon.png ] && [ -f /work/art/wallpaper.png ]; then
    log "artwork from art/"
    (cd "$VK" && bash tools/prepare-assets.sh --icon /work/art/icon.png --background /work/art/wallpaper.png \
        --output-directory "$sce") > "$OUT.art.log" 2>&1 || { tail -20 "$OUT.art.log"; exit 1; }
else
    log "placeholder artwork (add art/icon.png and art/wallpaper.png to use your own)"
    cp "$VK/sce_sys/icon0.png" "$VK/sce_sys/pic0.dds" "$VK/sce_sys/pic1.dds" "$sce/"
fi

log "libraries"
bash build_ps5_deps.sh
log "DXVK"
bash build_ps5_dxvk.sh all
log "game and title"
bash build_headless.sh --title
rm -rf "/work/dist/$TITLE_ID" && mkdir -p /work/dist
cp -R "$OUT/title/$TITLE_ID" /work/dist/
echo "title: /work/dist/$TITLE_ID"
