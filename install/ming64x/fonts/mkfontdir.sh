#!/bin/sh

# Regenerate fonts.dir and fonts.scale for all directories
for dir in */; do
    echo "$dir"...
    mkfontdir "$dir"
    mkfontscale "$dir"
done

# Regenerate fonts.alias from *.alias files
for dir in */; do
    echo "$dir"...
    update-fonts-alias "$dir"
done

