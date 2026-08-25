#!/bin/sh
set -eu

upstream=$1
overlay_dir=$2
output=$3

rm -rf "$output"
mkdir -p "$output"
cp -R "$upstream"/. "$output"/
rm -rf "$output/.git"
cp -R "$overlay_dir/files"/. "$output"/
