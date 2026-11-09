# Sourced by build-android.sh: the Hyperspace it builds. One fork holds the iPad
# game's Hyperspace for every port (Android, Linux, Switch).
# REF is pinned to a commit so a build always gets the same Hyperspace; move it on
# deliberately. HS_REPO and HS_REF pick another clone, branch or commit (a commit
# by its full hash: git fetches no abbreviated ones).
REPO=${HS_REPO:-https://github.com/Sidd065/FTL-Hyperspace.git}
REF=${HS_REF:-2d74b71fd640e95d3ec7bddd7ad6e5a0e0457910}  # branch ipad-player-reports, until iOS-binary-ports has it

# Checks out REF in SRC, fetching only that commit
hs_checkout() {
  [ -d "$SRC/.git" ] || git init -q "$SRC"
  git -C "$SRC" fetch -q --depth 1 "$REPO" "$REF"
  git -C "$SRC" checkout -q -f FETCH_HEAD
}
