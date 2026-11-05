# Sourced by build-android.sh: the Hyperspace it builds. One fork holds the iPad
# game's Hyperspace for every port (Android, Linux, Switch).
# REF is pinned to a commit so a build always gets the same Hyperspace; move it on
# deliberately. HS_REPO and HS_REF pick another clone, branch or commit.
REPO=${HS_REPO:-https://github.com/The-Dumb-Dino/FTL-Hyperspace-Dino.git}
REF=${HS_REF:-59bd00458c606086321568532f0c4b5e817d802a}  # branch iOS-binary-ports

# Checks out REF in SRC, fetching only that commit
hs_checkout() {
  [ -d "$SRC/.git" ] || git init -q "$SRC"
  git -C "$SRC" fetch -q --depth 1 "$REPO" "$REF"
  git -C "$SRC" checkout -q -f FETCH_HEAD
}
