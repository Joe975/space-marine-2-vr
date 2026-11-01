#!/usr/bin/env bash
# Fetches the portable reverse-engineering tools and reference source this project uses into
# tools/ and refs/ (both gitignored). Safe to re-run: downloads resume, finished steps are skipped.
# Run from Git Bash:  bash scripts/fetch-tools.sh
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/tools"; REFS="$ROOT/refs"; DL="$TOOLS/_dl"
SEVENZIP="/c/Program Files/7-Zip/7z.exe"
mkdir -p "$DL" "$REFS"

# Pinned versions. SHA-256 values are the digests GitHub publishes for the release assets.
GHIDRA_TAG="Ghidra_12.1.4_build"
GHIDRA_ZIP="ghidra_12.1.4_PUBLIC_20260921.zip"
GHIDRA_SHA="ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db"
X64DBG_TAG="2026.05.27"
X64DBG_ZIP="snapshot_2026-05-27_12-11.zip"
X64DBG_SHA="d41966dfc5b435a372798245300ca0ab7bb8e48bdbf48512c6fb20fcca427697"
RENDERDOC_VER="1.46"

fetch() {  # fetch <url> <file> [sha256]   (resumes <file>.part; long transfers drop on a busy link)
  local url="$1" out="$DL/$2" sha="${3:-}" try
  if [ -f "$out" ]; then echo "have   $2"; return 0; fi
  echo "fetch  $2"
  for try in 1 2 3 4 5 6 7 8; do
    curl -fSL -s -C - -o "$out.part" "$url" && break
    [ "$try" = 8 ] && { echo "FAILED $2 (partial kept, re-run to resume)"; return 1; }
  done
  if [ -n "$sha" ] && [ "$(sha256sum "$out.part" | cut -d' ' -f1)" != "$sha" ]; then
    echo "BAD SHA-256 $2 (deleting)"; rm -f "$out.part"; return 1
  fi
  mv "$out.part" "$out"
}

unpack() {  # unpack <file> <dir>   (each archive gets its own directory)
  local zip="$DL/$1" dest="$TOOLS/$2"
  [ -f "$zip" ] || { echo "skip   $2 (download incomplete)"; return 1; }
  if [ -n "$(ls -A "$dest" 2>/dev/null)" ]; then echo "have   $2/"; return 0; fi
  mkdir -p "$dest" && "$SEVENZIP" x -y -bso0 -bsp0 "-o$dest" "$zip" && echo "unpack $2/"
}

ref() {  # ref <owner/repo>   (shallow clone, source only)
  local name="${1##*/}" try
  if [ -d "$REFS/$name/.git" ]; then echo "have   refs/$name"; return 0; fi
  for try in 1 2 3; do
    rm -rf "$REFS/$name"
    git clone -q --depth 1 "https://github.com/$1.git" "$REFS/$name" 2>/dev/null && { echo "clone  refs/$name"; return 0; }
  done
  rm -rf "$REFS/$name"; echo "FAILED refs/$name"; return 1
}

# Tools. Ghidra 12 needs a JDK 21; this machine only has 17, so a portable one sits beside it.
fetch "https://github.com/x64dbg/x64dbg/releases/download/$X64DBG_TAG/$X64DBG_ZIP" "$X64DBG_ZIP" "$X64DBG_SHA"
fetch "https://renderdoc.org/stable/$RENDERDOC_VER/RenderDoc_${RENDERDOC_VER}_64.zip" "RenderDoc_${RENDERDOC_VER}_64.zip"
fetch "https://api.adoptium.net/v3/binary/latest/21/ga/windows/x64/jdk/hotspot/normal/eclipse" "jdk21.zip"
fetch "https://github.com/NationalSecurityAgency/ghidra/releases/download/$GHIDRA_TAG/$GHIDRA_ZIP" "$GHIDRA_ZIP" "$GHIDRA_SHA"

unpack "$X64DBG_ZIP" x64dbg
unpack "RenderDoc_${RENDERDOC_VER}_64.zip" renderdoc
unpack "jdk21.zip" jdk21
unpack "$GHIDRA_ZIP" ghidra

# Otis_Inf/Skall photo-mode cheat table: outdated for current builds, kept for its camera byte patterns.
mkdir -p "$TOOLS/framed-ct"
[ -s "$TOOLS/framed-ct/SM2_PM.CT" ] || curl -fsSL -A "Mozilla/5.0" -o "$TOOLS/framed-ct/SM2_PM.CT" \
  "https://framedsc.com/CheatTables/SM2_PM.CT" || echo "FAILED SM2_PM.CT"

# Reference source: REFramework-derived VR injection for closed D3D12 engines, plus the libraries they use.
for r in elliotttate/vrframework mutars/starfield2vr mutars/anvilengine2vr KhronosGroup/OpenXR-SDK \
         cursey/safetyhook praydog/UEVR praydog/REFramework; do ref "$r"; done
# vrframework's CMake builds glm/imgui/spdlog/json/openvr out of REFramework's dependencies/ submodules.
if [ -d "$REFS/REFramework/.git" ] && [ ! -f "$REFS/REFramework/dependencies/glm/CMakeLists.txt" ]; then
  git -C "$REFS/REFramework" submodule update -q --init --depth 1 && echo "submod refs/REFramework" \
    || echo "FAILED REFramework submodules"
fi

# Framework core the mod is built against (CMakeLists.txt expects refs/vrframework-core). This is the
# working core behind starfield2vr / anvilengine2vr; the author's own repo is no longer public, so it
# comes from a public MIT mirror, pinned to the commit the mod was last built with.
CORE_REPO="https://github.com/joeyhodge/vrframework.git"
CORE_SHA="462b4358ac2088bcc08c55fb7bfb343c48132f99"
if [ ! -d "$REFS/vrframework-core/.git" ]; then
  git clone -q "$CORE_REPO" "$REFS/vrframework-core" && git -C "$REFS/vrframework-core" checkout -q "$CORE_SHA" \
    && echo "clone  refs/vrframework-core @ ${CORE_SHA:0:7}" || echo "FAILED refs/vrframework-core"
else
  echo "have   refs/vrframework-core"
fi

echo "--- tools ---"; ls "$TOOLS"; echo "--- refs ---"; ls "$REFS"
